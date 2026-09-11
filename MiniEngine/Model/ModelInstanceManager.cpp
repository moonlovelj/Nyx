#include "ModelInstanceManager.h"
#include "Model.h"
#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"
#include "CommandBucketer.h"
#include "GeometryStreaming.h"
#include "../Core/Math/Random.h"

#include <algorithm>

namespace ModelInstanceManager
{
    std::shared_ptr<Model> s_SourceModel;
    std::vector<ModelInstance> s_ModelInstances;
    UploadBuffer s_InstanceConstantsCPU;
    ByteAddressBuffer s_InstanceConstantsGPU;
    Math::Vector3 s_InstanceDistributionCenter(Math::kZero);
    Math::Vector3 s_InstanceDistributionHalfExtents(Math::kZero);
    float s_InstanceDistributionRadius = 0.0f;

    // Local bounds are shared by model instances; transforms are per Mesh instance.
    std::vector<Math::AxisAlignedBox> s_MeshLocalBounds;
    std::vector<Math::AffineTransform> s_PublishedMeshTransforms;
    std::vector<SceneObjectUpdate> s_SceneObjectUpdates;
    bool s_HasPublishedMeshTransforms = false;

    bool AreTransformsEqual(const Math::AffineTransform& a, const Math::AffineTransform& b)
    {
        return DirectX::XMVector3Equal(a.GetX(), b.GetX()) &&
            DirectX::XMVector3Equal(a.GetY(), b.GetY()) &&
            DirectX::XMVector3Equal(a.GetZ(), b.GetZ()) &&
            DirectX::XMVector3Equal(a.GetTranslation(), b.GetTranslation());
    }

    Math::AxisAlignedBox TransformBounds(const Math::AxisAlignedBox& bounds, const Math::AffineTransform& transform)
    {
        const Math::Vector3 center = transform * bounds.GetCenter();
        const Math::Vector3 extent = bounds.GetDimensions() * 0.5f;
        const Math::Vector3 worldExtent = Math::Abs(transform.GetX()) * extent.GetX() +
            Math::Abs(transform.GetY()) * extent.GetY() + Math::Abs(transform.GetZ()) * extent.GetZ();
        return Math::AxisAlignedBox(center - worldExtent, center + worldExtent);
    }

	void Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount)
	{
		ASSERT(sourceModel != nullptr, "Source model is null");
		ASSERT(instanceCount > 0, "Instance count must be greater than zero");

		s_SourceModel = sourceModel;
        s_MeshLocalBounds.clear();
        for (const Mesh* mesh : sourceModel->m_Meshes)
        {
            Math::AxisAlignedBox bounds;
            for (uint32_t drawIndex = 0; drawIndex < mesh->numDraws; ++drawIndex)
            {
                const Mesh::Draw& draw = mesh->draw[drawIndex];
                bounds.AddPoint(Math::Vector3(draw.boundingBoxMin[0], draw.boundingBoxMin[1], draw.boundingBoxMin[2]));
                bounds.AddPoint(Math::Vector3(draw.boundingBoxMax[0], draw.boundingBoxMax[1], draw.boundingBoxMax[2]));
            }
            s_MeshLocalBounds.push_back(bounds);
        }
        s_PublishedMeshTransforms.resize(static_cast<size_t>(instanceCount) * s_MeshLocalBounds.size());
        s_SceneObjectUpdates.clear();
        s_HasPublishedMeshTransforms = false;

		InstanceResourceManager::Initialize(
			std::max(sourceModel->m_NumNodes * instanceCount, 1u),
			std::max(sourceModel->m_NumJoints * instanceCount, 1u),
			sizeof(MeshConstants),
			sizeof(Joint));

		s_ModelInstances.clear();
		s_ModelInstances.reserve(instanceCount);

		const float modelRadius = std::max((float)sourceModel->m_BoundingSphere.GetRadius(), 0.001f);
		const float spacing = modelRadius * 2.0f;

		auto CeilDiv = [](uint32_t numerator, uint32_t denominator) -> uint32_t
		{
			return (numerator + denominator - 1) / denominator;
		};

		constexpr uint32_t kGridAspectWidth = 1;
		constexpr uint32_t kGridAspectHeight = 1;
		uint32_t gridRows = 1;
		uint32_t gridCols = 1;
		while (true)
		{
			gridCols = CeilDiv(gridRows * kGridAspectWidth, kGridAspectHeight);
			if (static_cast<uint64_t>(gridRows) * static_cast<uint64_t>(gridCols) >= static_cast<uint64_t>(instanceCount))
				break;
			++gridRows;
		}

		const float halfCols = (gridCols > 0) ? (static_cast<float>(gridCols - 1) * 0.5f) : 0.0f;
		const float halfRows = (gridRows > 0) ? (static_cast<float>(gridRows - 1) * 0.5f) : 0.0f;

		std::vector<Math::Vector3> instancePositions;
		instancePositions.reserve(instanceCount);
		for (uint32_t i = 0; i < instanceCount; ++i)
		{
			const uint32_t row = i / gridCols;
			const uint32_t col = i % gridCols;
			const float x = (static_cast<float>(col) - halfCols) * spacing;
			const float z = (static_cast<float>(row) - halfRows) * spacing;
			instancePositions.emplace_back(x, 0.0f, z);
		}

		Math::RandomNumberGenerator rng;

		const uint32_t numModelDraws = sourceModel->GetNumTotalDraws();
		s_InstanceConstantsCPU.Create(L"Model Instance Constants CPU", instanceCount * numModelDraws * sizeof(InstanceConstants));
		InstanceConstants* instanceConstantsCPU = (InstanceConstants*)s_InstanceConstantsCPU.Map();
		for (uint32_t i = 0; i < instanceCount; ++i)
		{
			s_ModelInstances.emplace_back(sourceModel);
			ModelInstance& instance = s_ModelInstances.back();
			instance.LoopAllAnimations();
            instance.SetRotation(Math::Quaternion(Math::Vector3(Math::kYUnitVector), Math::Scalar((i > 0) * rng.NextFloat(0.0f, XM_2PI))));
			instance.SetPosition(instancePositions[i]);
			instance.SetupInstanceData(instanceConstantsCPU + i * numModelDraws);
		}
		s_InstanceConstantsCPU.Unmap();

		Math::Vector3 minCenter = instancePositions[0];
		Math::Vector3 maxCenter = instancePositions[0];
		for (const Math::Vector3& position : instancePositions)
		{
			minCenter = Math::Min(minCenter, position);
			maxCenter = Math::Max(maxCenter, position);
		}

		s_InstanceDistributionCenter = (minCenter + maxCenter) * 0.5f;
		s_InstanceDistributionHalfExtents = Math::Max((maxCenter - minCenter) * 0.5f, Math::Vector3(Math::kZero));
		s_InstanceDistributionRadius = 0.0f;
		for (const ModelInstance& instance : s_ModelInstances)
		{
			const float distanceToCenter = Length(instance.GetCenter() - s_InstanceDistributionCenter);
			const float boundRadius = distanceToCenter + static_cast<float>(instance.GetRadius());
			s_InstanceDistributionRadius = std::max(s_InstanceDistributionRadius, boundRadius);
		}

		s_InstanceConstantsGPU.Create(
			L"Model Instance Constants GPU",
			instanceCount * numModelDraws,
			sizeof(InstanceConstants),
			s_InstanceConstantsCPU
		);

		{
			Renderer::SetBindlessResourceDescriptor(SRV_MESH_CONSTANTS_BUFFER, InstanceResourceManager::GetMeshConstantsBuffer().GetSRV());
			Renderer::SetBindlessResourceDescriptor(SRV_JOINTS_BUFFER, InstanceResourceManager::GetJointsBuffer().GetSRV());
			Renderer::SetBindlessResourceDescriptor(SRV_MATERIAL_CONSTANTS_BUFFER, sourceModel->m_MaterialConstants.GetSRV());
			Renderer::SetBindlessResourceDescriptor(SRV_INSTANCE_CONSTANTS_BUFFER, s_InstanceConstantsGPU.GetSRV());
		}

		DrawCommandManager::FinalizeIndirectCommands();

		GeometryStreaming::Initialize(
			sourceModel->m_Nodes,
			static_cast<uint32_t>(sourceModel->m_GroupMetadatas.size()),
			static_cast<uint32_t>(sourceModel->m_PageMetadatas.size()));

		GeometryStreaming::PinRootPages(sourceModel.get());
	}

	void Update(GraphicsContext& gfxContext, float deltaTime)
	{
        s_SceneObjectUpdates.clear();
		MeshConstants* meshConstantsCPU = (MeshConstants*)InstanceResourceManager::GetMeshConstantsCPU().Map();
		Joint* jointCPU = (Joint*)InstanceResourceManager::GetJointsCPU().Map();

		uint32_t meshConstOffset = 0;
		uint32_t jointOffset = 0;
		for (auto& instance : s_ModelInstances)
		{
			instance.Update(deltaTime, meshConstantsCPU + meshConstOffset, jointCPU + jointOffset);
			meshConstOffset += instance.GetInstanceAllocation().meshConstantCount;
			jointOffset += instance.GetInstanceAllocation().jointCount;
		}

		InstanceResourceManager::GetMeshConstantsCPU().Unmap();
		InstanceResourceManager::GetJointsCPU().Unmap();

		InstanceResourceManager::FlushBufferUpdate(gfxContext);

        uint32_t objectId = 0;
        for (const ModelInstance& instance : s_ModelInstances)
        {
            for (uint32_t meshIndex = 0; meshIndex < s_MeshLocalBounds.size(); ++meshIndex, ++objectId)
            {
                const Math::AffineTransform& current = instance.GetMeshWorldTransform(meshIndex);
                Math::AffineTransform& previous = s_PublishedMeshTransforms[objectId];
                if (s_HasPublishedMeshTransforms && !AreTransformsEqual(current, previous))
                {
                    const Math::AxisAlignedBox& bounds = s_MeshLocalBounds[meshIndex];
                    s_SceneObjectUpdates.push_back({objectId, TransformBounds(bounds, previous), TransformBounds(bounds, current)});
                }
                previous = current;
            }
        }
        s_HasPublishedMeshTransforms = true;
	}

    const std::vector<SceneObjectUpdate>& GetSceneObjectUpdates()
    {
        return s_SceneObjectUpdates;
    }

	void Cleanup()
	{
        s_MeshLocalBounds.clear();
        s_PublishedMeshTransforms.clear();
        s_SceneObjectUpdates.clear();
        s_HasPublishedMeshTransforms = false;
		GeometryStreaming::Shutdown();
		s_ModelInstances.clear();
		s_SourceModel.reset();
		InstanceResourceManager::Cleanup();
		DrawCommandManager::Cleanup();
		s_InstanceConstantsCPU.Destroy();
		s_InstanceConstantsGPU.Destroy();
		s_InstanceDistributionCenter = Math::Vector3(Math::kZero);
		s_InstanceDistributionHalfExtents = Math::Vector3(Math::kZero);
		s_InstanceDistributionRadius = 0.0f;
	}

	uint32_t GetNumModelInstances()
	{
		return (uint32_t)s_ModelInstances.size();
	}

	ModelInstance& GetModelInstance(uint32_t index)
	{
		ASSERT(index < s_ModelInstances.size(), "Model instance index out of range");
		return s_ModelInstances[index];
	}

	Model* GetSourceModel()
	{
		return s_SourceModel.get();
	}

	Math::Vector3 GetInstanceDistributionCenter()
	{
		return s_InstanceDistributionCenter;
	}

	Math::Vector3 GetInstanceDistributionHalfExtents()
	{
		return s_InstanceDistributionHalfExtents;
	}

	float GetInstanceDistributionRadius()
	{
		return s_InstanceDistributionRadius;
	}
}

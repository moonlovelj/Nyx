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

	void Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount)
	{
		ASSERT(sourceModel != nullptr, "Source model is null");
		ASSERT(instanceCount > 0, "Instance count must be greater than zero");

		s_SourceModel = sourceModel;

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
	}

	void Cleanup()
	{
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

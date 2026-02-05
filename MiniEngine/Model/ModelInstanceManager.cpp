#include "ModelInstanceManager.h"
#include "Model.h"
#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"
#include "CommandBucketer.h"
#include "GeometryStreaming.h"

namespace ModelInstanceManager
{
    std::shared_ptr<Model> s_SourceModel;
    std::vector<ModelInstance> s_ModelInstances;
    UploadBuffer s_InstanceConstantsCPU;
    ByteAddressBuffer s_InstanceConstantsGPU;

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
		
		const float radius = sourceModel->m_BoundingSphere.GetRadius();
		const float spacing = radius * 1.0f;
		const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(instanceCount))));
		const float half = (gridSide > 0) ? (static_cast<float>(gridSide - 1) * 0.5f) : 0.0f;

		const uint32_t numModelDraws = sourceModel->GetNumTotalDraws();
		s_InstanceConstantsCPU.Create(L"Model Instance Constants CPU", instanceCount * numModelDraws * sizeof(InstanceConstants));
		InstanceConstants* instanceConstantsCPU = (InstanceConstants*)s_InstanceConstantsCPU.Map();
		for (uint32_t i = 0; i < instanceCount; ++i)
		{
			s_ModelInstances.emplace_back(sourceModel);
			s_ModelInstances[i].LoopAllAnimations();

			const uint32_t row = i / gridSide;
			const uint32_t col = i % gridSide;

			const float x = (static_cast<float>(col) - half) * spacing;
			const float z = (static_cast<float>(row) - half) * spacing;

			s_ModelInstances[i].SetPosition(Math::Vector3(x, 0.0f, z));
			s_ModelInstances[i].SetupInstanceData(instanceConstantsCPU + i * numModelDraws);
		}
		s_InstanceConstantsCPU.Unmap();

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

	void Render(Renderer::MeshSorter& sorter)
	{
		for (auto& instance : s_ModelInstances)
		{
			instance.Render(sorter);
		}
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
		InstanceResourceManager::Cleanup();
		DrawCommandManager::Cleanup();
		s_InstanceConstantsCPU.Destroy();
		s_InstanceConstantsGPU.Destroy();
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
}

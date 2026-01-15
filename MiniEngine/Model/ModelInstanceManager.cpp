#include "ModelInstanceManager.h"
#include "Model.h"
#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"
#include "CommandBucketer.h"
#include "GeometryStreaming.h"

void ModelInstanceManager::Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount)
{
	ASSERT(sourceModel != nullptr, "Source model is null");
	ASSERT(instanceCount > 0, "Instance count must be greater than zero");

	m_SourceModel = sourceModel;

	InstanceResourceManager::Get().Initialize(
		std::max(sourceModel->m_NumNodes * instanceCount, 1u),
		std::max(sourceModel->m_NumJoints * instanceCount, 1u),
		sizeof(MeshConstants),
		sizeof(Joint));

	m_ModelInstances.clear();
	m_ModelInstances.reserve(instanceCount);
	
	const float radius = sourceModel->m_BoundingSphere.GetRadius();
	const float spacing = radius * 1.0f;
	const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(instanceCount))));
	const float half = (gridSide > 0) ? (static_cast<float>(gridSide - 1) * 0.5f) : 0.0f;

	const uint32_t numModelDraws = sourceModel->GetNumTotalDraws();
	m_InstanceConstantsCPU.Create(L"Model Instance Constants CPU", instanceCount * numModelDraws * sizeof(InstanceConstants));
	InstanceConstants* instanceConstantsCPU = (InstanceConstants*)m_InstanceConstantsCPU.Map();
	for (uint32_t i = 0; i < instanceCount; ++i)
	{
		m_ModelInstances.emplace_back(sourceModel);
		m_ModelInstances[i].LoopAllAnimations();

		const uint32_t row = i / gridSide;
		const uint32_t col = i % gridSide;

		const float x = (static_cast<float>(col) - half) * spacing;
		const float z = (static_cast<float>(row) - half) * spacing;

		m_ModelInstances[i].SetPosition(Math::Vector3(x, 0.0f, z));
		m_ModelInstances[i].SetupInstanceData(instanceConstantsCPU + i * numModelDraws);
	}
	m_InstanceConstantsCPU.Unmap();

	m_InstanceConstantsGPU.Create(
		L"Model Instance Constants GPU",
		instanceCount * numModelDraws,
		sizeof(InstanceConstants),
		m_InstanceConstantsCPU
	);

	{
		Renderer::SetBindlessResourceDescriptor(SRV_MESH_CONSTANTS_BUFFER, InstanceResourceManager::Get().GetMeshConstantsBuffer().GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_JOINTS_BUFFER, InstanceResourceManager::Get().GetJointsBuffer().GetSRV());
		//Renderer::SetBindlessResourceDescriptor(SRV_VERTEX_BUFFER, sourceModel->m_DataBuffer.GetSRV());
		//Renderer::SetBindlessResourceDescriptor(SRV_INDEX_BUFFER, sourceModel->m_DataBuffer.GetSRV());
		//Renderer::SetBindlessResourceDescriptor(SRV_MESHLET_BUFFER, sourceModel->m_MeshletConstants.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_MATERIAL_CONSTANTS_BUFFER, sourceModel->m_MaterialConstants.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_INSTANCE_CONSTANTS_BUFFER, m_InstanceConstantsGPU.GetSRV());
		//Renderer::SetBindlessResourceDescriptor(SRV_GEOMETRY_BUFFER, sourceModel->m_DataBuffer.GetSRV());
	}

	Renderer::CommandBucketer::Get().FinalizeIndirectCommands();

	GeometryStreaming::Initialize(
		sourceModel->m_Nodes,
		static_cast<uint32_t>(sourceModel->m_GroupMetadatas.size()),
		static_cast<uint32_t>(sourceModel->m_PageMetadatas.size()));
	//GeometryStreaming::LoadAllGeometries(
	//	sourceModel->m_StreamingFilePath,
	//	sourceModel->m_GeometryBlobOffsetInFile,
	//	sourceModel->m_PageMetadatas,
	//	sourceModel->m_GroupMetadatas);

	GeometryStreaming::PinRootPages(sourceModel.get());
}

void ModelInstanceManager::Render(Renderer::MeshSorter& sorter) const
{
	for (auto& instance : m_ModelInstances)
	{
		instance.Render(sorter);
	}
}

void ModelInstanceManager::Update(GraphicsContext& gfxContext, float deltaTime)
{
	MeshConstants* meshConstantsCPU = (MeshConstants*)InstanceResourceManager::Get().GetMeshConstantsCPU().Map();
	Joint* jointCPU = (Joint*)InstanceResourceManager::Get().GetJointsCPU().Map();

	uint32_t meshConstOffset = 0;
	uint32_t jointOffset = 0;
	for (auto& instance : m_ModelInstances)
	{
		instance.Update(deltaTime, meshConstantsCPU + meshConstOffset, jointCPU + jointOffset);
		meshConstOffset += instance.GetInstanceAllocation().meshConstantCount;
		jointOffset += instance.GetInstanceAllocation().jointCount;
	}

	InstanceResourceManager::Get().GetMeshConstantsCPU().Unmap();
	InstanceResourceManager::Get().GetJointsCPU().Unmap();

	InstanceResourceManager::Get().FlushBufferUpdate(gfxContext);
}

void ModelInstanceManager::Cleanup()
{
	GeometryStreaming::Shutdown();
	m_ModelInstances.clear();
	InstanceResourceManager::Get().Cleanup();
	Renderer::CommandBucketer::Get().ResetAll();
	m_InstanceConstantsCPU.Destroy();
	m_InstanceConstantsGPU.Destroy();
}

ModelInstance& ModelInstanceManager::GetModelInstance(uint32_t index)
{
	ASSERT(index < m_ModelInstances.size(), "Model instance index out of range");
	return m_ModelInstances[index];
}
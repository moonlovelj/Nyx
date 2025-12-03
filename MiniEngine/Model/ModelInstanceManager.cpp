#include "ModelInstanceManager.h"
#include "Model.h"
#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"
#include "GPUDriven/CommandBucketer.h"

void ModelInstanceManager::Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount)
{
	ASSERT(sourceModel != nullptr, "Source model is null");
	ASSERT(instanceCount > 0, "Instance count must be greater than zero");

	InstanceResourceManager::Get().Initialize(
		std::max(sourceModel->m_NumNodes * instanceCount, 1u),
		std::max(sourceModel->m_NumJoints * instanceCount, 1u),
		std::max(sourceModel->GetNumTotalDraws() * instanceCount, 1u),
		sizeof(MeshConstants),
		sizeof(Joint),
		sizeof(GPUDriven::IndirectCommand)
	);

	m_ModelInstances.clear();
	m_ModelInstances.reserve(instanceCount);
	
	const float radius = sourceModel->m_BoundingSphere.GetRadius();
	const float spacing = radius * 1.0f;
	const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(instanceCount))));
	const float half = (gridSide > 0) ? (static_cast<float>(gridSide - 1) * 0.5f) : 0.0f;

	m_InstanceConstantsCPU.Create(L"Model Instance Constants CPU", instanceCount * sizeof(InstanceConstants));
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

		instanceConstantsCPU[i].MeshConstantsBase = m_ModelInstances[i].GetInstanceAllocation().meshConstantBase;
		instanceConstantsCPU[i].JointBase = m_ModelInstances[i].GetInstanceAllocation().jointBase;
	}
	m_InstanceConstantsCPU.Unmap();

	m_InstanceConstantsGPU.Create(
		L"Model Instance Constants GPU",
		instanceCount,
		sizeof(InstanceConstants),
		m_InstanceConstantsCPU
	);

	{
		Renderer::SetBindlessResourceDescriptor(SRV_MESH_CONSTANTS_BUFFER, InstanceResourceManager::Get().GetMeshConstantsBuffer().GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_JOINTS_BUFFER, InstanceResourceManager::Get().GetJointsBuffer().GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_VERTEX_BUFFER, sourceModel->m_DataBuffer.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_MESHLET_BUFFER, sourceModel->m_MeshletConstants.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_MATERIAL_CONSTANTS_BUFFER, sourceModel->m_MaterialConstants.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_INSTANCE_CONSTANTS_BUFFER, m_InstanceConstantsGPU.GetSRV());
	}

	GPUDriven::CommandBucketer::Get().FinalizeAll();
}

void ModelInstanceManager::Render(Renderer::MeshSorter& sorter) const
{
	sorter.SetMeshConstantsBuffer(InstanceResourceManager::Get().GetMeshConstantsBuffer().GetSRV());
	sorter.SetJointsBuffer(InstanceResourceManager::Get().GetJointsBuffer().GetSRV());

	for (auto& instance : m_ModelInstances)
	{
		instance.Render(sorter);
	}
}

void ModelInstanceManager::Update(GraphicsContext& gfxContext, float deltaTime)
{
	for (auto& instance : m_ModelInstances)
	{
		instance.Update(gfxContext, deltaTime);
	}

	InstanceResourceManager::Get().FlushScheduledUpdates(gfxContext);
}

void ModelInstanceManager::Cleanup()
{
	m_ModelInstances.clear();
	InstanceResourceManager::Get().Cleanup();
	GPUDriven::CommandBucketer::Get().ResetAll();
	m_InstanceConstantsCPU.Destroy();
	m_InstanceConstantsGPU.Destroy();
}

ModelInstance& ModelInstanceManager::GetModelInstance(uint32_t index)
{
	ASSERT(index < m_ModelInstances.size(), "Model instance index out of range");
	return m_ModelInstances[index];
}
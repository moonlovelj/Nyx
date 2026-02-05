#include "CommandBucketer.h"
#include "Renderer.h"
#include "../Core/GraphicsCommon.h"

using namespace Renderer;

namespace DrawCommandManager
{
    std::vector<DrawItem> m_PotentialDrawItemsCPU;

    StructuredBuffer m_PotentialDrawItemsGPU;
    StructuredBuffer m_InstanceCulledDrawGPU;
    StructuredBuffer m_TaskQueueStateGPU;
    ByteAddressBuffer m_TaskQueueGPU;
    ByteAddressBuffer m_MeshletBatchGPU;
    ByteAddressBuffer m_CandidateMeshletGPU;
    StructuredBuffer m_VisibleMeshletBufferGPU;
    StructuredBuffer m_IndirectDispatchMeshGPU;
} // namespace DrawCommandManager

void  DrawCommandManager::FinalizeIndirectCommands()
{
	m_PotentialDrawItemsGPU.Create(
		L"Potential Draw Items Buffer",
		std::max(1u, (uint32_t)m_PotentialDrawItemsCPU.size()),
		(uint32_t)sizeof(DrawItem),
		m_PotentialDrawItemsCPU.data());

	Renderer::SetBindlessResourceDescriptor(SRV_POTENTIAL_DRAW_ITEM_BUFFER, m_PotentialDrawItemsGPU.GetSRV());

	m_InstanceCulledDrawGPU.Create(
		L"Instance Culled Draw Items Buffer",
		std::max(1u, (uint32_t)m_PotentialDrawItemsCPU.size()),
		(uint32_t)sizeof(DrawItem),
		nullptr);
	Renderer::SetBindlessResourceDescriptor(SRV_INSTANCE_CULLED_DRAW_BUFFER, m_InstanceCulledDrawGPU.GetSRV());
	Renderer::SetBindlessResourceDescriptor(UAV_INSTANCE_CULLED_DRAW_BUFFER, m_InstanceCulledDrawGPU.GetUAV());

	m_TaskQueueStateGPU.Create(
		L"Task Queue State Buffer",
		1,
		sizeof(QueueState),
		nullptr);
	Renderer::SetBindlessResourceDescriptor(SRV_TASK_QUEUE_STATE_BUFFER, m_TaskQueueStateGPU.GetSRV());
	Renderer::SetBindlessResourceDescriptor(UAV_TASK_QUEUE_STATE_BUFFER, m_TaskQueueStateGPU.GetUAV());

	const uint32_t taskQueueSize = MAX_NODES + MAX_BVH_NODES_PER_GROUP;
	m_TaskQueueGPU.Create(
		L"Task Queue Buffer",
		taskQueueSize,
		sizeof(DrawItem),
		nullptr);
	Renderer::SetBindlessResourceDescriptor(SRV_TASK_QUEUE_BUFFER, m_TaskQueueGPU.GetSRV());
	Renderer::SetBindlessResourceDescriptor(UAV_TASK_QUEUE_BUFFER, m_TaskQueueGPU.GetUAV());

	m_MeshletBatchGPU.Create(
		L"Meshlet Batch Buffer",
		MAX_CANDIDATE_MESHLETS_BATCH + 1u,
		sizeof(uint32_t),
		nullptr
	);
	Renderer::SetBindlessResourceDescriptor(UAV_MESHLET_BATCH_BUFFER, m_MeshletBatchGPU.GetUAV());

	m_CandidateMeshletGPU.Create(
		L"Candidate Meshlet Buffer",
		MAX_CANDIDATE_MESHLETS + DAG_CULL_GROUP_SIZE,
		MESHLET_BYTE_STRIDE,
		nullptr);
	Renderer::SetBindlessResourceDescriptor(UAV_CANDIDATE_MESHLET_BUFFER, m_CandidateMeshletGPU.GetUAV());

	m_VisibleMeshletBufferGPU.Create(
		L"Visible Meshlet Buffer",
		MAX_VISIBLE_MESHLETS,
		sizeof(VisibleMeshletPayload),
		nullptr);
	Renderer::SetBindlessResourceDescriptor(SRV_VISIBLE_MESHLET_BUFFER, m_VisibleMeshletBufferGPU.GetSRV());
	Renderer::SetBindlessResourceDescriptor(UAV_VISIBLE_MESHLET_BUFFER, m_VisibleMeshletBufferGPU.GetUAV());

	m_IndirectDispatchMeshGPU.Create(
		L"Indirect Dispatch Mesh Buffer",
		1,
		(uint32_t)sizeof(Renderer::DispatchMeshCommand),
		nullptr);
	Renderer::SetBindlessResourceDescriptor(UAV_INDIRECT_DISPATCH_MESHES_BASE, m_IndirectDispatchMeshGPU.GetUAV());
}

void DrawCommandManager::AddDrawItem(const DrawItem& item)
{
	m_PotentialDrawItemsCPU.push_back(item);
}

void DrawCommandManager::Cleanup()
{
	m_PotentialDrawItemsCPU.clear();
	m_PotentialDrawItemsGPU.Destroy();

	m_InstanceCulledDrawGPU.Destroy();
	m_TaskQueueStateGPU.Destroy();
	m_TaskQueueGPU.Destroy();
	m_MeshletBatchGPU.Destroy();
	m_IndirectDispatchMeshGPU.Destroy();
	m_CandidateMeshletGPU.Destroy();
	m_VisibleMeshletBufferGPU.Destroy();
}

GpuBuffer& DrawCommandManager::GetPotentialDrawItemsGPU()
{
    return m_PotentialDrawItemsGPU;
}
GpuBuffer& DrawCommandManager::GetInstanceCulledDrawGPU()
{
    return m_InstanceCulledDrawGPU;
}
StructuredBuffer& DrawCommandManager::GetTaskQueueStateGPU()
{
    return m_TaskQueueStateGPU;
}
ByteAddressBuffer& DrawCommandManager::GetTaskQueueGPU()
{
    return m_TaskQueueGPU;
}
StructuredBuffer& DrawCommandManager::GetVisibleMeshletBufferGPU()
{
    return m_VisibleMeshletBufferGPU;
}
StructuredBuffer& DrawCommandManager::GetIndirectDispatchMeshGPU()
{
    return m_IndirectDispatchMeshGPU;
}
ByteAddressBuffer& DrawCommandManager::GetMeshletBatchGPU()
{
    return m_MeshletBatchGPU;
}
ByteAddressBuffer& DrawCommandManager::GetCandidateMeshletGPU()
{
    return m_CandidateMeshletGPU;
}

uint32_t DrawCommandManager::GetNumPotentialDrawItems()
{
    return static_cast<uint32_t>(m_PotentialDrawItemsCPU.size());
}

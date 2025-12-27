#include "CommandBucketer.h"
#include "Renderer.h"
#include "../Core/GraphicsCommon.h"

using namespace Renderer;

CommandBucketer& CommandBucketer::Get()
{
	static CommandBucketer s_Instance;
	return s_Instance;
}

void CommandBucketer::AppendIndirectCommand(uint32_t psoIdx, const IndirectCommand& cmd)
{
	auto& vec = m_IndirectCommandsCPU[psoIdx];
	vec.push_back(cmd);
}

void CommandBucketer::FinalizeIndirectCommands()
{
	for (const auto& kvp : m_IndirectCommandsCPU)
	{
		size_t cmdCount = kvp.second.size();
		if (cmdCount == 0) 	continue;

		ASSERT(cmdCount < 0x01FFFFFF, "Too many indirect commands for PSO %d: %zu", kvp.first, cmdCount);

		m_IndirectCommandsGPU[kvp.first].Create(
			L"Indirect Commands Buffer",
			(uint32_t)cmdCount,
			(uint32_t)sizeof(IndirectCommand),
			kvp.second.data());

		Renderer::SetBindlessResourceDescriptor(SRV_INDIRECT_COMMANDS_BASE + (uint32_t)kvp.first, 
			m_IndirectCommandsGPU[kvp.first].GetSRV());

		m_IndirectVisibleFlags[kvp.first].Create(
			L"Indirect Visible Flags Buffer",
			(uint32_t)cmdCount,
			(uint32_t)sizeof(uint32_t),
			nullptr);

		Renderer::SetBindlessResourceDescriptor(SRV_INDIRECT_VISIBLE_FLAGS_BASE + (uint32_t)kvp.first,
			m_IndirectVisibleFlags[kvp.first].GetSRV());
		Renderer::SetBindlessResourceDescriptor(UAV_INDIRECT_VISIBLE_FLAGS_BASE + (uint32_t)kvp.first,
			m_IndirectVisibleFlags[kvp.first].GetUAV());

		m_IndirectCullingResults[kvp.first].Create(
			L"Indirect Culling Results Buffer",
			(uint32_t)cmdCount,
			(uint32_t)sizeof(uint32_t),
			nullptr);

		Renderer::SetBindlessResourceDescriptor(SRV_INDIRECT_CULLING_RESULTS_BASE + (uint32_t)kvp.first,
			m_IndirectCullingResults[kvp.first].GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_INDIRECT_CULLING_RESULTS_COUNTER_BASE + (uint32_t)kvp.first,
			m_IndirectCullingResults[kvp.first].GetCounterBuffer().GetSRV());
		Renderer::SetBindlessResourceDescriptor(UAV_INDIRECT_CULLING_RESULTS_BASE + (uint32_t)kvp.first,
			m_IndirectCullingResults[kvp.first].GetUAV());

		m_IndirectDispatchMeshes[kvp.first].Create(
			L"Indirect Dispatch Mesh Commands Buffer",
			1,
			(uint32_t)sizeof(Renderer::DispatchMeshCommand),
			nullptr);
		//Renderer::SetBindlessResourceDescriptor(UAV_INDIRECT_DISPATCH_MESHES_BASE + (uint32_t)kvp.first,
		//	m_IndirectDispatchMeshes[kvp.first].GetUAV());
	}

	m_PotentialDrawItemsGPU.Create(
		L"Potential Draw Items Buffer",
		std::max(1u, (uint32_t)m_PotentialDrawItemsCPU.size()),
		(uint32_t)sizeof(DrawItem),
		m_PotentialDrawItemsCPU.data());

	Renderer::SetBindlessResourceDescriptor(SRV_POTENTIAL_DRAW_ITEM_BUFFER, m_PotentialDrawItemsGPU.GetSRV());
	//static const uint32_t kMaxMeshletsPerPass = 1 << 24;
	
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

bool CommandBucketer::HasAnyCommands(uint32_t psoIdx) const
{
	auto it = m_IndirectCommandsCPU.find(psoIdx);
	if (it != m_IndirectCommandsCPU.end() && !it->second.empty())
		return true;
	return false;
}


void CommandBucketer::AddDrawItem(const DrawItem& item)
{
	m_PotentialDrawItemsCPU.push_back(item);
}

GpuBuffer& CommandBucketer::GetIndirectCommandsGPU(uint32_t psoIdx)
{
	return m_IndirectCommandsGPU.at(psoIdx);
}
GpuBuffer& CommandBucketer::GetIndirectVisibleFlags(uint32_t psoIdx)
{
	return m_IndirectVisibleFlags.at(psoIdx);
}
GpuBuffer& CommandBucketer::GetIndirectCullingResults(uint32_t psoIdx)
{
	return m_IndirectCullingResults.at(psoIdx);
}
//GpuBuffer& CommandBucketer::GetIndirectDispatchMeshes(uint32_t psoIdx)
//{
//	return m_IndirectDispatchMeshes.at(psoIdx);
//}

GpuBuffer& CommandBucketer::GetIndirectCullingResultsCounter(uint32_t psoIdx)
{
	return m_IndirectCullingResults.at(psoIdx).GetCounterBuffer();
}

uint32_t CommandBucketer::GetMaxCommands(uint32_t psoIdx) const
{
	return (uint32_t)m_IndirectCommandsCPU.at(psoIdx).size();
}

void CommandBucketer::ResetAll() 
{
	m_IndirectCommandsCPU.clear();
	for (auto& kvp: m_IndirectCommandsGPU)
	{
		kvp.second.Destroy();
	}
	m_IndirectCommandsGPU.clear();

	for (auto& kvp : m_IndirectVisibleFlags)
	{
		kvp.second.Destroy();
	}
	m_IndirectVisibleFlags.clear();

	for (auto& kvp : m_IndirectCullingResults)
	{
		kvp.second.Destroy();
	}
	m_IndirectCullingResults.clear();

	for (auto& kvp : m_IndirectDispatchMeshes)
	{
		kvp.second.Destroy();
	}
	m_IndirectDispatchMeshes.clear();

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

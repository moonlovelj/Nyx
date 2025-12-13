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
			(uint32_t)sizeof(IndirectCommand),
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
		Renderer::SetBindlessResourceDescriptor(UAV_INDIRECT_DISPATCH_MESHES_BASE + (uint32_t)kvp.first,
			m_IndirectDispatchMeshes[kvp.first].GetUAV());
	}
}

bool CommandBucketer::HasAnyCommands(uint32_t psoIdx) const
{
	auto it = m_IndirectCommandsCPU.find(psoIdx);
	if (it != m_IndirectCommandsCPU.end() && !it->second.empty())
		return true;
	return false;
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
GpuBuffer& CommandBucketer::GetIndirectDispatchMeshes(uint32_t psoIdx)
{
	return m_IndirectDispatchMeshes.at(psoIdx);
}

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
}

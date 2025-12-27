#pragma once

#include "Renderer.h"
#include "StructsIO.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>

namespace Renderer
{
	struct IndirectCommand;
	struct DispatchMeshCommand;

	class CommandBucketer
	{
	public:
		static CommandBucketer& Get();
		void ResetAll();
		void AppendIndirectCommand(uint32_t psoIdx, const IndirectCommand& cmd);
		void FinalizeIndirectCommands();
		bool HasAnyCommands(uint32_t psoIdx) const;

		void AddDrawItem(const DrawItem& item);

		GpuBuffer& GetIndirectCommandsGPU(uint32_t psoIdx);
		GpuBuffer& GetIndirectVisibleFlags(uint32_t psoIdx);
		GpuBuffer& GetIndirectCullingResults(uint32_t psoIdx);
		//GpuBuffer& GetIndirectDispatchMeshes(uint32_t psoIdx);
		GpuBuffer& GetIndirectCullingResultsCounter(uint32_t psoIdx);
		GpuBuffer& GetPotentialDrawItemsGPU() { return m_PotentialDrawItemsGPU; }
		GpuBuffer& GetInstanceCulledDrawGPU() { return m_InstanceCulledDrawGPU; }
		StructuredBuffer& GetTaskQueueStateGPU() { return m_TaskQueueStateGPU; }
		ByteAddressBuffer& GetTaskQueueGPU() { return m_TaskQueueGPU; }
		StructuredBuffer& GetVisibleMeshletBufferGPU() { return m_VisibleMeshletBufferGPU; }
		StructuredBuffer& GetIndirectDispatchMeshGPU() { return m_IndirectDispatchMeshGPU; }
		ByteAddressBuffer& GetMeshletBatchGPU() { return m_MeshletBatchGPU; }
		ByteAddressBuffer& GetCandidateMeshletGPU() { return m_CandidateMeshletGPU; }

		uint32_t GetMaxCommands(uint32_t psoIdx) const;
		uint32_t GetNumPotentialDrawItems() const { return static_cast<uint32_t>(m_PotentialDrawItemsCPU.size()); }

	private:
		CommandBucketer() = default;
		std::unordered_map<uint32_t, std::vector<IndirectCommand>> m_IndirectCommandsCPU;

		std::unordered_map<uint32_t, ByteAddressBuffer> m_IndirectCommandsGPU;
		std::unordered_map<uint32_t, ByteAddressBuffer> m_IndirectVisibleFlags;
		std::unordered_map<uint32_t, StructuredBuffer> m_IndirectCullingResults;
		std::unordered_map<uint32_t, StructuredBuffer> m_IndirectDispatchMeshes;

		std::vector<DrawItem> m_PotentialDrawItemsCPU;
		StructuredBuffer m_PotentialDrawItemsGPU;
		StructuredBuffer m_InstanceCulledDrawGPU;

		StructuredBuffer m_TaskQueueStateGPU;
		ByteAddressBuffer m_TaskQueueGPU;
		ByteAddressBuffer m_MeshletBatchGPU;
		ByteAddressBuffer m_CandidateMeshletGPU;

		StructuredBuffer m_VisibleMeshletBufferGPU;
		StructuredBuffer m_IndirectDispatchMeshGPU;
	};
}
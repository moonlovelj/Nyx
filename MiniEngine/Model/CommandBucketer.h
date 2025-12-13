#pragma once

#include "Renderer.h"
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

		GpuBuffer& GetIndirectCommandsGPU(uint32_t psoIdx);
		GpuBuffer& GetIndirectVisibleFlags(uint32_t psoIdx);
		GpuBuffer& GetIndirectCullingResults(uint32_t psoIdx);
		GpuBuffer& GetIndirectDispatchMeshes(uint32_t psoIdx);
		GpuBuffer& GetIndirectCullingResultsCounter(uint32_t psoIdx);

		uint32_t GetMaxCommands(uint32_t psoIdx) const;

	private:
		CommandBucketer() = default;
		std::unordered_map<uint32_t, std::vector<IndirectCommand>> m_IndirectCommandsCPU;

		std::unordered_map<uint32_t, ByteAddressBuffer> m_IndirectCommandsGPU;
		std::unordered_map<uint32_t, ByteAddressBuffer> m_IndirectVisibleFlags;
		std::unordered_map<uint32_t, StructuredBuffer> m_IndirectCullingResults;
		std::unordered_map<uint32_t, StructuredBuffer> m_IndirectDispatchMeshes;
	};
}
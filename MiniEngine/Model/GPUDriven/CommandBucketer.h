#pragma once

#include "ExecuteIndirect.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>

namespace GPUDriven
{
	struct PSORun
	{
		uint16_t psoIdx;   // Renderer::sm_PSOs 索引（基础 PSO 索引；等深时渲染加 +1）
		uint32_t startCmd; // 合并后全局缓冲中的命令起始索引
		uint32_t count;    // 命令数量
	};

	class CommandBucketer
	{
	public:
		static CommandBucketer& Get();

		// Shadow（4 桶：skin×alphaTest），使用 ZPass 命令
		void AppendShadow(uint32_t bucketId, const IndirectCommand& cmd);
		bool HasShadow() const { return m_ShadowFinalized; }
		const std::vector<PSORun>& GetShadowRuns() const { return m_ShadowRuns; }
		IndirectArgsBuffer& GetShadowArgsBuffer() { return m_ShadowArgs; }
		void ResetShadow();

		// Depth（ZPass 非阴影）（4 桶：skin×alphaTest）
		void AppendDepth(uint32_t bucketId, const IndirectCommand& cmd);
		bool HasDepth() const { return m_DepthFinalized; }
		const std::vector<PSORun>& GetDepthRuns() const { return m_DepthRuns; }
		IndirectArgsBuffer& GetDepthArgsBuffer() { return m_DepthArgs; }
		void ResetDepth();

		// Color（Opaque/GBuffer）：按基础 PSO 分桶；区分是否等深（EqualDepth）
		void AppendColor(uint16_t basePsoIdx, const IndirectCommand& cmd, bool equalDepth);
		bool HasColor() const { return m_ColorFinalized; }
		const std::vector<PSORun>& GetColorRunsRW() const { return m_ColorRunsRW; }   // 常规深度
		const std::vector<PSORun>& GetColorRunsEQ() const { return m_ColorRunsEQ; }   // 等深
		IndirectArgsBuffer& GetColorArgsBuffer() { return m_ColorArgs; }
		void ResetColor();

		void ResetAll();

		void FinalizeAll();

		size_t CalculateMaxIndirectArgsBufferSize();

		ByteAddressBuffer& GetArgsVisibleFlagsBuffer(uint16_t psoIdx);
		StructuredBuffer& GetCullingResultArgsBuffer(uint16_t psoIdx);

	private:
		void FinalizeShadow();
		void FinalizeDepth();
		void FinalizeColor();

		CommandBucketer() = default;

		// shadow/depth：固定4桶
		std::array<std::vector<IndirectCommand>, 4> m_ShadowBucketsCPU;
		std::array<std::vector<IndirectCommand>, 4> m_DepthBucketsCPU;

		// color：基础 PSO -> 命令列表（区分等深/常规）
		std::unordered_map<uint16_t, std::vector<IndirectCommand>> m_ColorBucketsCPU_RW;
		std::unordered_map<uint16_t, std::vector<IndirectCommand>> m_ColorBucketsCPU_EQ;

		// 合并后的结果
		std::vector<PSORun> m_ShadowRuns;
		std::vector<PSORun> m_DepthRuns;
		std::vector<PSORun> m_ColorRunsRW;
		std::vector<PSORun> m_ColorRunsEQ;

		IndirectArgsBuffer m_ShadowArgs;
		IndirectArgsBuffer m_DepthArgs;
		IndirectArgsBuffer m_ColorArgs;

		std::unordered_map<uint16_t, ByteAddressBuffer> m_ArgsVisibleFlags; // 用于剔除的中间数据
		std::unordered_map<uint16_t, StructuredBuffer> m_CullingResultArgs;

		bool m_ShadowFinalized = false;
		bool m_DepthFinalized = false;
		bool m_ColorFinalized = false;
	};
}
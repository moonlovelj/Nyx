#include "CommandBucketer.h"

using namespace GPUDriven;

static void BuildRunsFromBuckets(
	const std::vector<std::vector<IndirectCommand>>& buckets,
	const std::vector<uint16_t>& psoIndices,
	std::vector<IndirectCommand>& outMerged,
	std::vector<PSORun>& outRuns)
{
	outMerged.clear();
	outRuns.clear();
	uint32_t head = 0;
	for (size_t i = 0; i < buckets.size(); ++i)
	{
		const auto& src = buckets[i];
		if (src.empty()) continue;
		PSORun run{ psoIndices[(uint32_t)i], head, (uint32_t)src.size() };
		outMerged.insert(outMerged.end(), src.begin(), src.end());
		head += run.count;
		outRuns.push_back(run);
	}
}

static void BuildRunsFromMap(
	const std::unordered_map<uint16_t, std::vector<IndirectCommand>>& mapBuckets,
	std::vector<IndirectCommand>& outMerged,
	std::vector<PSORun>& outRuns)
{
	outMerged.clear();
	outRuns.clear();

	// 为稳定/可预测排序，按 PSO 索引升序遍历
	std::vector<uint16_t> keys;
	keys.reserve(mapBuckets.size());
	for (auto& kv : mapBuckets) if (!kv.second.empty()) keys.push_back(kv.first);
	std::sort(keys.begin(), keys.end());

	uint32_t head = 0;
	for (uint16_t psoIdx : keys)
	{
		const auto& src = mapBuckets.at(psoIdx);
		if (src.empty()) continue;
		PSORun run{ psoIdx, head, (uint32_t)src.size() };
		outMerged.insert(outMerged.end(), src.begin(), src.end());
		head += run.count;
		outRuns.push_back(run);
	}
}

CommandBucketer& CommandBucketer::Get()
{
	static CommandBucketer s_Instance;
	return s_Instance;
}

void CommandBucketer::AppendShadow(uint32_t bucketId, const IndirectCommand& cmd)
{
	if (bucketId < m_ShadowBucketsCPU.size())
	{
		m_ShadowBucketsCPU[bucketId].push_back(cmd);
		m_ShadowFinalized = false;
	}
}

void CommandBucketer::FinalizeShadow()
{
	// shadow PSO 索引：Renderer::Initialize 推入后位于 4..7
	std::vector<std::vector<IndirectCommand>> buckets(4);
	std::vector<uint16_t> psoIdx = { 4, 5, 6, 7 };
	for (uint32_t i = 0; i < 4; ++i) buckets[i] = std::move(m_ShadowBucketsCPU[i]);

	std::vector<IndirectCommand> merged;
	BuildRunsFromBuckets(buckets, psoIdx, merged, m_ShadowRuns);

	m_ShadowArgs.Destroy();
	if (merged.empty())
		return;

	m_ShadowArgs.Create(L"Shadow Prebucket Indirect Args", (uint32_t)merged.size(), (uint32_t)sizeof(IndirectCommand), merged.data());
	m_ShadowFinalized = true;
}

void CommandBucketer::ResetShadow()
{
	for (auto& b : m_ShadowBucketsCPU) b.clear();
	m_ShadowRuns.clear();
	m_ShadowArgs.Destroy();
	m_ShadowFinalized = false;
}

void CommandBucketer::AppendDepth(uint32_t bucketId, const IndirectCommand& cmd)
{
	if (bucketId < m_DepthBucketsCPU.size())
	{
		m_DepthBucketsCPU[bucketId].push_back(cmd);
		m_DepthFinalized = false;
	}
}

void CommandBucketer::FinalizeDepth()
{
	// depth PSO 索引：Renderer::Initialize 时的前4个 0..3
	std::vector<std::vector<IndirectCommand>> buckets(4);
	std::vector<uint16_t> psoIdx = { 0, 1, 2, 3 };
	for (uint32_t i = 0; i < 4; ++i) buckets[i] = std::move(m_DepthBucketsCPU[i]);

	std::vector<IndirectCommand> merged;
	BuildRunsFromBuckets(buckets, psoIdx, merged, m_DepthRuns);

	m_DepthArgs.Destroy();
	if (merged.empty())
		return;
		
	m_DepthArgs.Create(L"Depth Prebucket Indirect Args", (uint32_t)merged.size(), (uint32_t)sizeof(IndirectCommand), merged.data());
	m_DepthFinalized = true;
}

void CommandBucketer::ResetDepth()
{
	for (auto& b : m_DepthBucketsCPU) b.clear();
	m_DepthRuns.clear();
	m_DepthArgs.Destroy();
	m_DepthFinalized = false;
}

void CommandBucketer::AppendColor(uint16_t basePsoIdx, const IndirectCommand& cmd, bool equalDepth)
{
	auto& mapRef = equalDepth ? m_ColorBucketsCPU_EQ : m_ColorBucketsCPU_RW;
	mapRef[basePsoIdx].push_back(cmd);
	m_ColorFinalized = false;
}

void CommandBucketer::FinalizeColor()
{
	// RW 深度
	std::vector<IndirectCommand> mergedRW;
	BuildRunsFromMap(m_ColorBucketsCPU_RW, mergedRW, m_ColorRunsRW);
	// 等深
	std::vector<IndirectCommand> mergedEQ;
	BuildRunsFromMap(m_ColorBucketsCPU_EQ, mergedEQ, m_ColorRunsEQ);

	// 合并 RW + EQ 到同一块 GPU 缓冲，便于一次资源管理
	m_ColorArgs.Destroy();

	const uint32_t total = (uint32_t)mergedRW.size() + (uint32_t)mergedEQ.size();
	if (total == 0)
		return;

	// 拼接并调整 EQ 的 startCmd 基址
	std::vector<IndirectCommand> mergedAll;
	mergedAll.reserve(total);
	mergedAll.insert(mergedAll.end(), mergedRW.begin(), mergedRW.end());
	const uint32_t rwCount = (uint32_t)mergedRW.size();
	for (auto& run : m_ColorRunsEQ) run.startCmd += rwCount;
	mergedAll.insert(mergedAll.end(), mergedEQ.begin(), mergedEQ.end());

	m_ColorArgs.Create(L"Color Prebucket Indirect Args", total, (uint32_t)sizeof(IndirectCommand), mergedAll.data());
	m_ColorFinalized = true;
}

void CommandBucketer::ResetColor()
{
	m_ColorBucketsCPU_RW.clear();
	m_ColorBucketsCPU_EQ.clear();
	m_ColorRunsRW.clear();
	m_ColorRunsEQ.clear();
	m_ColorArgs.Destroy();
	m_ColorFinalized = false;
}

size_t CommandBucketer::CalculateMaxIndirectArgsBufferSize()
{
	return std::max({ m_ShadowArgs.GetBufferSize(), m_DepthArgs.GetBufferSize(), m_ColorArgs.GetBufferSize() });
}

void CommandBucketer::ResetAll() 
{
	ResetShadow(); 
	ResetDepth(); 
	ResetColor();
}
#include "GeometryStreaming.h"
#include "../Core/GpuBuffer.h"
#include "../Core/ReadbackBuffer.h"
#include "../Core/GraphicsCore.h"
#include "Model.h"
#include "Renderer.h"
#include "ModelInstanceManager.h"

#include <numeric>
#include <condition_variable>
#include <intrin.h>

namespace GeometryStreaming
{
	const uint32_t kMaxChunks = 16;
	constexpr uint32_t kNumReadbackBuffers = 3;
	StructuredBuffer m_HierarchyNodesGPU;
	std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	StructuredBuffer m_GroupDataLocationGPU;
	std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;
	uint32_t m_GroupCount = 0;

	struct PageResidency {
		uint32_t ChunkIndex = INVALID_CHUNK_INDEX;
		uint32_t SlotIndex = 0; // 在 Chunk 内的槽位索引
		uint64_t LastUsedFrame = 0;
		bool IsPinned = false;
		bool IsLoading = false;
	};
	std::vector<PageResidency> m_PageTableCPU;

	struct PhysicalSlot {
		uint32_t ChunkIndex;
		uint32_t SlotIndex; // 在该 Chunk 内的索引
	};
	std::vector<PhysicalSlot> m_FreePool;

	std::set<uint32_t> m_PagesToLoad;

	ReadbackBuffer m_ReadbackRequestBuffer[kNumReadbackBuffers];
	ByteAddressBuffer m_GPURequestBuffer;
	StructuredBuffer m_GeometryStreamingStateGPU;
	ByteAddressBuffer m_GeometryStreamingRequestMaskGPU;
	ReadbackBuffer m_ReadbackRequestMaskBuffer[kNumReadbackBuffers];
	uint64_t m_FenceValues[kNumReadbackBuffers] = {};

	struct LoadedPage {
		uint32_t PageIndex;
		std::vector<uint8_t> Data;
	};
	std::mutex m_CompletedPagesMutex;
	std::deque<LoadedPage> m_CompletedPagesQueue;

	bool m_NeedSyncAddressTable = false;

	struct IORequest {
		uint32_t PageIdx;
		std::wstring FilePath;
		uint64_t BaseOffset;
	};
	std::thread m_IOThread;
	std::deque<IORequest> m_IOQueue;
	std::mutex m_IOQueueMutex;
	std::condition_variable m_IOCV;
	std::atomic<bool> m_StopIOThread{ false };
	void IOThreadFunc()
	{
		while (true)
		{
			IORequest req;
			{
				std::unique_lock<std::mutex> lock(m_IOQueueMutex);
				m_IOCV.wait(lock, [] { return !m_IOQueue.empty() || m_StopIOThread; });

				if (m_IOQueue.empty() && m_StopIOThread)
					break;

				req = m_IOQueue.front();
				m_IOQueue.pop_front();
			}

			std::vector<uint8_t> pageData(Renderer::kPageSizeInBytes);
			std::ifstream fs(req.FilePath, std::ios::in | std::ios::binary);
			if (fs)
			{
				fs.seekg(req.BaseOffset + (static_cast<uint64_t>(req.PageIdx) * Renderer::kPageSizeInBytes), std::ios::beg);
				fs.read((char*)pageData.data(), pageData.size());
				OnPageIOComplete(req.PageIdx, std::move(pageData));
			}
			else
			{
				// Error handling: maybe mark as not loading or log
				// currently just decrementing counter to avoid hangs
			}
		}
	}
}

void GeometryStreaming::Initialize(const std::vector<Renderer::HierarchyNode>& nodes, uint32_t maxGroupSize, uint32_t numPages)
{
	m_GroupCount = maxGroupSize;

	m_StopIOThread = false;
	m_IOThread = std::thread(IOThreadFunc);

	m_PageTableCPU.assign(numPages, PageResidency{});

	m_GPURequestBuffer.Create(L"GPU Request Buffer", MAX_STREAMING_REQUESTS + 64, sizeof(GeometryStreamingRequest));
	Renderer::SetBindlessResourceDescriptor(UAV_GEOMETRY_STREAMING_REQUESTS_BUFFER, m_GPURequestBuffer.GetUAV());
	m_GeometryStreamingStateGPU.Create(L"Geometry Streaming State", 1, sizeof(GeometryStreamingState));
	Renderer::SetBindlessResourceDescriptor(UAV_GEOMETRY_STREAMING_STATE_BUFFER, m_GeometryStreamingStateGPU.GetUAV());
	m_GeometryStreamingRequestMaskGPU.Create(L"Geometry Streaming Request Mask", (m_GroupCount + 31) / 32, sizeof(uint32_t));
	Renderer::SetBindlessResourceDescriptor(UAV_GEOMETRY_STREAMING_REQUEST_MASK_BUFFER, m_GeometryStreamingRequestMaskGPU.GetUAV());

	for (int i = 0; i < kNumReadbackBuffers; ++i)
	{
		m_ReadbackRequestBuffer[i].Create(L"Readback Request Buffer", MAX_STREAMING_REQUESTS + 64, sizeof(GeometryStreamingRequest));
		m_ReadbackRequestMaskBuffer[i].Create(L"Readback Request Mask Buffer", (m_GroupCount + 31) / 32, sizeof(uint32_t));
		m_FenceValues[i] = 0;
	}

	uint32_t slotsPerChunk = Renderer::kChunkSizeInBytes / Renderer::kPageSizeInBytes;
	m_FreePool.clear();
	for (uint32_t i = 0; i < kMaxChunks; ++i)
	{
		ByteAddressBuffer chunk;
		chunk.Create(L"Geometry Chunk ", Renderer::kChunkSizeInBytes / 4, 4);
		m_GeometryChunksGPU.push_back(std::move(chunk));
		Renderer::SetBindlessResourceDescriptor(
			SRV_GEOMETRY_CHUNK_DATA_BUFFER + i,
			m_GeometryChunksGPU[i].GetSRV()
		);

		for (uint32_t s = 0; s < slotsPerChunk; ++s)
			m_FreePool.push_back({ i, s });
	}

	//std::reverse(m_FreePool.begin(), m_FreePool.end());

	uint32_t numNodes = static_cast<uint32_t>(nodes.size());
	m_HierarchyNodesGPU.Create(L"Hierarchy Nodes GPU", numNodes, sizeof(Renderer::HierarchyNode), nodes.data());

	Renderer::SetBindlessResourceDescriptor(
		SRV_HIERARCHY_NODES_BUFFER,
		m_HierarchyNodesGPU.GetSRV()
	);

	uint32_t groupCount = std::max(1u, maxGroupSize);
	m_GroupDataLocationGPU.Create(L"Group Data Location GPU", groupCount, sizeof(Renderer::GroupDataLocation), nullptr);
	Renderer::SetBindlessResourceDescriptor(
		SRV_GROUP_DATA_LOCATION_BUFFER,
		m_GroupDataLocationGPU.GetSRV()
	);

	m_GroupDataLocations.resize(groupCount, { INVALID_CHUNK_INDEX , 0});
}

void GeometryStreaming::PinRootPages(Model* model)
{
	if (!model) return;

	std::set<uint32_t> rootPageIndices;
	for (const auto& node : model->m_Nodes)
	{
		if (node.Internal.IsGroup && node.MaxParrentError == std::numeric_limits<float>::infinity())
		{
			uint32_t groupIdx = node.Leaf.GroupIndex;
			if (groupIdx < model->m_GroupMetadatas.size())
			{
				uint32_t pageIdx = model->m_GroupMetadatas[groupIdx].PageIndex;
				rootPageIndices.insert(pageIdx);
			}
		}
	}

	if (rootPageIndices.empty()) return;

	const uint64_t requiredMemory = static_cast<uint64_t>(rootPageIndices.size()) * static_cast<uint64_t>(Renderer::kPageSizeInBytes);
	const uint64_t availableMemory = static_cast<uint64_t>(m_FreePool.size()) * static_cast<uint64_t>(Renderer::kPageSizeInBytes);
	if (requiredMemory > availableMemory)
	{
		Utility::Printf(L"GeometryStreaming: Not enough VRAM to pin root pages! Required: %llu MB, Available: %llu MB\n",
			requiredMemory / (1024 * 1024),
			availableMemory / (1024 * 1024));
	}

	Renderer::GroupDataLocation* locations = m_GroupDataLocations.data();
	GraphicsContext& gfx = GraphicsContext::Begin(L"Pin Root Pages Upload");

	for (uint32_t pageIdx : rootPageIndices)
	{
		if (m_PageTableCPU[pageIdx].ChunkIndex != INVALID_CHUNK_INDEX) continue;

		if (m_FreePool.empty()) {
			ASSERT(false, "VRAM Pool too small to even fit Root Pages!");
			break;
		}

		PhysicalSlot slot = m_FreePool.back();
		m_FreePool.pop_back();

		std::vector<uint8_t> pageData(Renderer::kPageSizeInBytes);
		std::ifstream fs(model->m_StreamingFilePath, std::ios::in | std::ios::binary);
		//fs.seekg(model->m_GeometryBlobOffsetInFile + (pageIdx * Renderer::kPageSizeInBytes));
		uint64_t fileOffset = model->m_GeometryBlobOffsetInFile + (static_cast<uint64_t>(pageIdx) * static_cast<uint64_t>(Renderer::kPageSizeInBytes));
		fs.seekg(fileOffset);
		fs.read((char*)pageData.data(), pageData.size());

		uint32_t byteOffset = slot.SlotIndex * Renderer::kPageSizeInBytes;
		gfx.WriteBuffer(m_GeometryChunksGPU[slot.ChunkIndex], byteOffset, pageData.data(), pageData.size());

		const auto& pageMeta = model->m_PageMetadatas[pageIdx];
		for (uint32_t i = 0; i < pageMeta.GroupCount; ++i)
		{
			uint32_t gIdx = pageMeta.StartGroupIndex + i;
			locations[gIdx].ChunkIndex = slot.ChunkIndex;
			locations[gIdx].ByteOffset = byteOffset + model->m_GroupMetadatas[gIdx].OffsetInPage;
		}

		m_PageTableCPU[pageIdx].ChunkIndex = slot.ChunkIndex;
		m_PageTableCPU[pageIdx].SlotIndex = slot.SlotIndex;
		m_PageTableCPU[pageIdx].IsPinned = true;
		m_PageTableCPU[pageIdx].IsLoading = false;
	}

	for (auto& chunk : m_GeometryChunksGPU)
		gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);

	gfx.WriteBuffer(m_GroupDataLocationGPU, 0, m_GroupDataLocations.data(), m_GroupDataLocations.size() * sizeof(Renderer::GroupDataLocation));
	gfx.TransitionResource(m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

	gfx.Finish(true);

	Utility::Printf(L"GeometryStreaming: Pinned %zu root pages.\n", rootPageIndices.size());
}

void GeometryStreaming::Shutdown()
{
	std::lock_guard<std::mutex> lock(m_IOQueueMutex);
	m_StopIOThread = true;
	m_IOCV.notify_all();

	if (m_IOThread.joinable())
		m_IOThread.join();

	m_IOQueue.clear();

	m_HierarchyNodesGPU.Destroy();
	for (auto& chunk : m_GeometryChunksGPU)
	{
		chunk.Destroy();
	}
	m_GeometryChunksGPU.clear();

	//m_GroupDataLocationCPU.Destroy();
	m_GroupDataLocationGPU.Destroy();

	m_GPURequestBuffer.Destroy();
	m_GeometryStreamingStateGPU.Destroy();
	m_GeometryStreamingRequestMaskGPU.Destroy();
	for (int i = 0; i < kNumReadbackBuffers; ++i)
	{
		m_ReadbackRequestBuffer[i].Destroy();
		m_ReadbackRequestMaskBuffer[i].Destroy();
	}
}

void GeometryStreaming::Update(uint32_t frameIndex)
{
	if (frameIndex < 1) return; // 避免读到未初始化的数据

	Model* sourceModel = ModelInstanceManager::Get().GetSourceModel();
	if (nullptr == sourceModel) return;

	const uint32_t readbackBufferIndex = frameIndex % kNumReadbackBuffers;
	const uint64_t currentFenceValue = m_FenceValues[readbackBufferIndex];
	if (currentFenceValue > 0 && Graphics::g_CommandManager.IsFenceComplete(currentFenceValue))
	{
		m_FenceValues[readbackBufferIndex] = 0;

		uint32_t* requestedGroupsMask = (uint32_t*)m_ReadbackRequestMaskBuffer[readbackBufferIndex].Map();
		const uint32_t numRequests = (m_GroupCount + 31) / 32;
		for (uint32_t i = 0; i < numRequests; ++i)
		{
			uint32_t mask = requestedGroupsMask[i];
			unsigned long bitIndex;
			while (_BitScanForward(&bitIndex, mask))
			{
				uint32_t groupIndex = (i * 32) + bitIndex;
				mask &= ~(1u << bitIndex);
				if (groupIndex >= sourceModel->m_GroupMetadatas.size()) continue;
				const auto& groupMeta = sourceModel->m_GroupMetadatas[groupIndex];
				const uint32_t pageIdx = groupMeta.PageIndex;
				m_PageTableCPU[pageIdx].LastUsedFrame = frameIndex;
				if (m_PageTableCPU[pageIdx].ChunkIndex == INVALID_CHUNK_INDEX && !m_PageTableCPU[pageIdx].IsLoading)
				{
					m_PagesToLoad.insert(pageIdx);
				}
			}
		}

		m_ReadbackRequestMaskBuffer[readbackBufferIndex].Unmap();

		for (uint32_t pageIdx : m_PagesToLoad)
		{
			m_PageTableCPU[pageIdx].IsLoading = true;
			EnqueueAsyncLoad(pageIdx);
		}
		m_PagesToLoad.clear();
	}

	SyncMemoryAndAddressTable(frameIndex);
	
	if (m_FenceValues[readbackBufferIndex] == 0)
	{
		// 读取请求
		GraphicsContext& gfxContext = GraphicsContext::Begin(L"GeometryStreaming Update");

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_COPY_SOURCE);
		//gfxContext.TransitionResource(m_GPURequestBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
		gfxContext.CopyBufferRegion(
			m_ReadbackRequestMaskBuffer[readbackBufferIndex], 0,
			m_GeometryStreamingRequestMaskGPU, 0, (m_GroupCount + 31) / 32 * 4);

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		gfxContext.ClearUAV(m_GeometryStreamingRequestMaskGPU,0);

		m_FenceValues[readbackBufferIndex] = gfxContext.Finish();
	}
}

void GeometryStreaming::SyncMemoryAndAddressTable(uint32_t frameIndex)
{
	std::deque<LoadedPage> readyPages;
	{
		std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
		if (m_CompletedPagesQueue.empty()) return;
		readyPages.swap(m_CompletedPagesQueue);
	}

	Model* sourceModel = ModelInstanceManager::Get().GetSourceModel();
	Renderer::GroupDataLocation* locations = m_GroupDataLocations.data();
	GraphicsContext& gfx = GraphicsContext::Begin(L"Streaming Upload");

	ImmediateEvict(frameIndex, locations);

	auto it = readyPages.begin();
	for (; it != readyPages.end(); ++it)
	{
		if (m_FreePool.empty()) break;

		auto& item = *it;
		PhysicalSlot slot = m_FreePool.back();
		m_FreePool.pop_back();

		uint32_t byteOffset = slot.SlotIndex * Renderer::kPageSizeInBytes;
		gfx.WriteBuffer(m_GeometryChunksGPU[slot.ChunkIndex], byteOffset, item.Data.data(), item.Data.size());

		// 更新 Page 下所有组的映射
		const auto& pageMeta = sourceModel->m_PageMetadatas[item.PageIndex];
		for (uint32_t i = 0; i < pageMeta.GroupCount; ++i)
		{
			uint32_t gIdx = pageMeta.StartGroupIndex + i;
			locations[gIdx].ChunkIndex = slot.ChunkIndex;
			locations[gIdx].ByteOffset = byteOffset + sourceModel->m_GroupMetadatas[gIdx].OffsetInPage;
			m_NeedSyncAddressTable = true;
		}

		m_PageTableCPU[item.PageIndex].ChunkIndex = slot.ChunkIndex;
		m_PageTableCPU[item.PageIndex].SlotIndex = slot.SlotIndex;
		m_PageTableCPU[item.PageIndex].LastUsedFrame = frameIndex;
		m_PageTableCPU[item.PageIndex].IsLoading = false;
	}

	if (it != readyPages.end())
	{
		std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
		// 将未处理的插回队列头部
		m_CompletedPagesQueue.insert(
			m_CompletedPagesQueue.begin(),
			std::make_move_iterator(it),
			std::make_move_iterator(readyPages.end())
		);
	}

	if (m_NeedSyncAddressTable)
	{
		m_NeedSyncAddressTable = false;
		gfx.WriteBuffer(m_GroupDataLocationGPU, 0, m_GroupDataLocations.data(), m_GroupDataLocations.size() * sizeof(Renderer::GroupDataLocation));
		gfx.TransitionResource(m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
	}

	for (auto& chunk : m_GeometryChunksGPU)
	{
		gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
	}
	gfx.Finish();
}

void GeometryStreaming::EnqueueAsyncLoad(uint32_t pageIdx)
{
	Model* sourceModel = ModelInstanceManager::Get().GetSourceModel();
	{
		std::lock_guard<std::mutex> lock(m_IOQueueMutex);
		m_IOQueue.push_back({
			pageIdx,
			sourceModel->m_StreamingFilePath,
			sourceModel->m_GeometryBlobOffsetInFile
			});
	}
	m_IOCV.notify_one();
}

void GeometryStreaming::OnPageIOComplete(uint32_t pageIdx, std::vector<uint8_t>&& pageData)
{
	std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
	m_CompletedPagesQueue.push_back({ pageIdx, std::move(pageData) });
}

void GeometryStreaming::ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* pLocationTable)
{
	struct Candidate {
		uint32_t pageIdx;
		uint64_t lastFrame;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(m_PageTableCPU.size());

	const uint32_t kEvictionGraceFrames = 120;
	for (uint32_t i = 0; i < (uint32_t)m_PageTableCPU.size(); ++i)
	{
		const auto& info = m_PageTableCPU[i];
		if (info.ChunkIndex != INVALID_CHUNK_INDEX && !info.IsPinned && !info.IsLoading)
		{
			if (info.LastUsedFrame + kEvictionGraceFrames < currentFrame)
			{
				candidates.push_back({ i, info.LastUsedFrame });
			}
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.lastFrame < b.lastFrame;
		});

	uint32_t evictedCount = 0;

	Model* sourceModel = ModelInstanceManager::Get().GetSourceModel();

	for (const auto& cand : candidates)
	{
		uint32_t pIdx = cand.pageIdx;
		PageResidency& page = m_PageTableCPU[pIdx];
		const auto& pageMeta = sourceModel->m_PageMetadatas[pIdx];
		for (uint32_t j = 0; j < pageMeta.GroupCount; ++j)
		{
			uint32_t gIdx = pageMeta.StartGroupIndex + j;
			pLocationTable[gIdx].ChunkIndex = INVALID_CHUNK_INDEX;
			pLocationTable[gIdx].ByteOffset = 0;
		}

		m_FreePool.push_back({ page.ChunkIndex, page.SlotIndex });
		page.ChunkIndex = INVALID_CHUNK_INDEX;
		page.SlotIndex = 0;
		evictedCount++;
	}

	if (evictedCount > 0)
	{
		m_NeedSyncAddressTable = true;
	}
	else
	{
		//static uint64_t lastWarnFrame = 0;
		//if (currentFrame > lastWarnFrame + 100) {
		//	Utility::Printf("WARNING: Geometry Cache Pool is full and no pages can be evicted! Increase kMaxChunks.\n");
		//	lastWarnFrame = currentFrame;
		//}
	}
}
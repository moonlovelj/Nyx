#include "GeometryPageLayout.h"
#include "GeometryStreaming.h"
#include "../Core/GpuBuffer.h"
#include "../Core/ReadbackBuffer.h"
#include "../Core/GraphicsCore.h"
#include "Model.h"
#include "Renderer.h"
#include "ModelInstanceManager.h"
#include "lz4.h"

#include <numeric>
#include <algorithm>
#include <condition_variable>
#include <intrin.h>
#include <cstring>
#include <fstream>

namespace GeometryStreaming
{
	const uint32_t kMaxChunks = 32;
	const uint32_t kInitialChunks = 1;
	constexpr uint32_t kNumReadbackBuffers = 3;
	uint32_t m_SlotsPerChunk = 0;
	StructuredBuffer m_HierarchyNodesGPU;
    StructuredBuffer m_GroupNodeIndicesGPU;
	std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	StructuredBuffer m_GroupDataLocationGPU;
	std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;
	uint32_t m_GroupCount = 0;
	static uint32_t s_UploadVersion = 0;

	struct PageResidency {
		uint32_t ChunkIndex = INVALID_CHUNK_INDEX;
		uint32_t SlotIndex = 0; // Slot index within the chunk
		uint64_t LastUsedFrame = 0;
		bool IsPinned = false;
		bool IsLoading = false;
	};
	std::vector<PageResidency> m_PageTableCPU;

	struct PhysicalSlot {
		uint32_t ChunkIndex;
		uint32_t SlotIndex; // Index within the chunk
	};
	std::vector<PhysicalSlot> m_FreePool;

	std::set<uint32_t> m_PagesToLoad;

	ByteAddressBuffer m_GeometryStreamingRequestMaskGPU;
	ReadbackBuffer m_ReadbackRequestMaskBuffer[kNumReadbackBuffers];
	uint64_t m_FenceValues[kNumReadbackBuffers] = {};
	bool m_HadFreshCull = false;
	bool m_ReadbackAllowsEviction[kNumReadbackBuffers] = {};

	struct LoadedPage {
		uint32_t PageIndex;
		std::vector<uint8_t> Data;
		bool Success;
	};
	std::mutex m_CompletedPagesMutex;
	std::deque<LoadedPage> m_CompletedPagesQueue;

	bool m_NeedSyncAddressTable = false;

	struct IORequest {
		uint32_t PageIdx;
		uint64_t FileOffset;
		uint32_t CompressedSize;
		uint32_t CompressionType;
	};
	std::thread m_IOThread;
	std::deque<IORequest> m_IOQueue;
	std::mutex m_IOQueueMutex;
	std::condition_variable m_IOCV;
	std::atomic<bool> m_StopIOThread{ false };
	static std::ifstream s_StreamingFile;
	static std::wstring s_StreamingFilePath;
	static std::mutex s_StreamingFileMutex;

	static void LogChunkStats(const wchar_t* tag)
	{
		const uint64_t allocatedChunks = static_cast<uint64_t>(m_GeometryChunksGPU.size());
		const uint64_t allocatedBytes = allocatedChunks * static_cast<uint64_t>(Renderer::kChunkSizeInBytes);
		const size_t allocatedSlots = static_cast<size_t>(allocatedChunks) * static_cast<size_t>(m_SlotsPerChunk);
		const size_t freeSlots = m_FreePool.size();
		const size_t usedSlots = allocatedSlots > freeSlots ? (allocatedSlots - freeSlots) : 0;
		const uint64_t usedBytes = static_cast<uint64_t>(usedSlots) * static_cast<uint64_t>(Renderer::kPageSizeInBytes);
		Utility::Printf(L"GeometryStreaming: %ls chunks=%llu/%u, allocated=%llu MB, used=%.2f MB, freeSlots=%zu\n",
			tag,
			allocatedChunks,
			kMaxChunks,
			allocatedBytes / (1024 * 1024),
			usedBytes / (1024.0 * 1024.0),
			freeSlots);
	}

	static bool AllocateChunk()
	{
		if (m_GeometryChunksGPU.size() >= kMaxChunks)
		{
			LogChunkStats(L"chunk limit reached");
			return false;
		}

		ByteAddressBuffer chunk;
		chunk.Create(L"Geometry Chunk ", Renderer::kChunkSizeInBytes / 4, 4);

		const uint32_t chunkIndex = static_cast<uint32_t>(m_GeometryChunksGPU.size());
		m_GeometryChunksGPU.push_back(std::move(chunk));
		Renderer::SetBindlessResourceDescriptor(
			SRV_GEOMETRY_CHUNK_DATA_BUFFER + chunkIndex,
			m_GeometryChunksGPU[chunkIndex].GetSRV()
		);

		for (uint32_t s = 0; s < m_SlotsPerChunk; ++s)
			m_FreePool.push_back({ chunkIndex, s });

		LogChunkStats(L"allocated chunk");
		return true;
	}

	static bool EnsureFreeSlots(uint32_t requiredSlots)
	{
		while (m_FreePool.size() < requiredSlots)
		{
			if (!AllocateChunk())
			{
				LogChunkStats(L"out of VRAM for request");
				return false;
			}
		}
		return true;
	}

    static IORequest MakePageReadRequest(const Model& model, uint32_t pageIndex)
    {
        IORequest req{};
        req.PageIdx = pageIndex;
        req.CompressionType = model.m_GeometryCompressionType;
        req.FileOffset = model.m_GeometryBlobOffsetInFile;
        if (req.CompressionType == Renderer::kGeometryCompressionLZ4)
        {
            const auto& info = model.m_PageCompressionInfos[pageIndex];
            req.FileOffset += info.CompressedOffset;
            req.CompressedSize = info.CompressedSize;
        }
        else
        {
            req.FileOffset += uint64_t(pageIndex) * Renderer::kPageSizeInBytes;
            req.CompressedSize = Renderer::kPageSizeInBytes;
        }
        return req;
    }
	static bool ReadPageFromFile(const IORequest& req, std::vector<uint8_t>& pageData)
	{
		const bool isCompressed = req.CompressionType == Renderer::kGeometryCompressionLZ4;
		std::vector<char> compressed(isCompressed ? req.CompressedSize : 0);
		char* destination = isCompressed ? compressed.data() : reinterpret_cast<char*>(pageData.data());
		const size_t size = isCompressed ? compressed.size() : pageData.size();
		{
			// Root uploads and the IO worker share the stream's cursor and error state.
			std::lock_guard<std::mutex> lock(s_StreamingFileMutex);
			s_StreamingFile.clear();
			s_StreamingFile.seekg(static_cast<std::streamoff>(req.FileOffset), std::ios::beg);
			s_StreamingFile.read(destination, static_cast<std::streamsize>(size));
			if (!s_StreamingFile) return false;
		}

		if (!isCompressed) return true;
		const int decoded = LZ4_decompress_safe(
			compressed.data(), reinterpret_cast<char*>(pageData.data()),
			static_cast<int>(compressed.size()), static_cast<int>(pageData.size()));
		return decoded == static_cast<int>(pageData.size());
	}

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

			std::vector<uint8_t> pageData(Renderer::kPageSizeInBytes, 0);
			const bool success = ReadPageFromFile(req, pageData);
			if (!success)
				Utility::Printf(L"GeometryStreaming: read/decode failed: %ls page %u\n", s_StreamingFilePath.c_str(), req.PageIdx);
			OnPageIOComplete(req.PageIdx, std::move(pageData), success);
		}
	}
}

uint32_t GeometryStreaming::GetUploadVersion()
{
	return s_UploadVersion;
}

bool GeometryStreaming::Initialize(const Model& model)
{
	s_StreamingFilePath = model.m_StreamingFilePath;
	s_StreamingFile.clear();
	s_StreamingFile.open(s_StreamingFilePath, std::ios::in | std::ios::binary);
	if (!s_StreamingFile)
	{
		Utility::Printf(L"GeometryStreaming: cannot open %ls\n", s_StreamingFilePath.c_str());
		return false;
	}

	const auto& nodes = model.m_Nodes;
	const uint32_t maxGroupSize = static_cast<uint32_t>(model.m_GroupMetadatas.size());
	const uint32_t numPages = static_cast<uint32_t>(model.m_PageMetadatas.size());
	m_GroupCount = maxGroupSize;
	m_HadFreshCull = false;
	std::fill_n(m_ReadbackAllowsEviction, kNumReadbackBuffers, false);

	m_PageTableCPU.assign(numPages, PageResidency{});

    const uint32_t requestWordCount = (m_GroupCount + 31) / 32;
    std::vector<uint32_t> initialRequestMask(requestWordCount, 0u);
	m_GeometryStreamingRequestMaskGPU.Create(L"Geometry Streaming Request Mask", requestWordCount, sizeof(uint32_t), initialRequestMask.data());
	Renderer::SetBindlessResourceDescriptor(UAV_GEOMETRY_STREAMING_REQUEST_MASK_BUFFER, m_GeometryStreamingRequestMaskGPU.GetUAV());

	for (int i = 0; i < kNumReadbackBuffers; ++i)
	{
		m_ReadbackRequestMaskBuffer[i].Create(L"Readback Request Mask Buffer", (m_GroupCount + 31) / 32, sizeof(uint32_t));
		m_FenceValues[i] = 0;
	}

	m_SlotsPerChunk = Renderer::kChunkSizeInBytes / Renderer::kPageSizeInBytes;
	m_FreePool.clear();
	m_GeometryChunksGPU.clear();
	m_GeometryChunksGPU.reserve(kMaxChunks);
	const uint32_t initialChunks = std::min(kInitialChunks, kMaxChunks);
	for (uint32_t i = 0; i < initialChunks; ++i)
	{
		if (!AllocateChunk())
			break;
	}

	//std::reverse(m_FreePool.begin(), m_FreePool.end());

	uint32_t numNodes = static_cast<uint32_t>(nodes.size());
	m_HierarchyNodesGPU.Create(L"Hierarchy Nodes GPU", numNodes, sizeof(Renderer::HierarchyNode), nodes.data());

	Renderer::SetBindlessResourceDescriptor(
		SRV_HIERARCHY_NODES_BUFFER,
		m_HierarchyNodesGPU.GetSRV()
	);

	uint32_t groupCount = std::max(1u, maxGroupSize);
    // LOD decisions must remain available even while the group's blob is absent.
    std::vector<uint32_t> groupNodeIndices(Math::AlignUp(groupCount, 4u), INVALID_NODE_INDEX);
    for (uint32_t nodeIndex = 0; nodeIndex < numNodes; ++nodeIndex)
    {
        const auto& node = nodes[nodeIndex];
        if (node.Internal.IsGroup && node.Leaf.GroupIndex < maxGroupSize)
            groupNodeIndices[node.Leaf.GroupIndex] = nodeIndex;
    }
    m_GroupNodeIndicesGPU.Create(L"Group Hierarchy Node Indices", groupCount,
        sizeof(uint32_t), groupNodeIndices.data());
    Renderer::SetBindlessResourceDescriptor(SRV_GROUP_NODE_INDICES_BUFFER, m_GroupNodeIndicesGPU.GetSRV());

	// Every model starts nonresident. resize() would preserve addresses from the
	// previous model when switching to a model with the same or fewer groups.
	m_GroupDataLocations.assign(groupCount, { INVALID_CHUNK_INDEX, 0 });
	m_GroupDataLocationGPU.Create(L"Group Data Location GPU", groupCount, sizeof(Renderer::GroupDataLocation), m_GroupDataLocations.data());
	Renderer::SetBindlessResourceDescriptor(
		SRV_GROUP_DATA_LOCATION_BUFFER,
		m_GroupDataLocationGPU.GetSRV()
	);
	m_StopIOThread = false;
	m_IOThread = std::thread(IOThreadFunc);
	return true;
}

bool GeometryStreaming::PinRootPages(Model* model)
{
    if (!model) return false;
    // LoadModel validates the directory; only check streaming state here.
    if (model->m_PageMetadatas.size() != m_PageTableCPU.size())
    {
        Utility::Printf("Root initialization: invalid page counts\n");
        return false;
    }
    const uint32_t count = model->m_RootPageCount;
    if (!count) return true;
    uint32_t needed = 0;
    for (uint32_t page = 0; page < count; ++page)
        if (!m_PageTableCPU[page].IsPinned) ++needed;
    const uint64_t available = m_FreePool.size() +
        uint64_t(kMaxChunks - m_GeometryChunksGPU.size()) * m_SlotsPerChunk;
    if (needed > available || !EnsureFreeSlots(needed))
    {
        Utility::Printf("Root initialization: need %u slots, available %llu\n", needed, available);
        return false;
    }
    if (!needed) return true;

    // Bounded upload batches. Never publish the address table until every root is valid.
    constexpr uint32_t batchSize = 16;
    std::vector<uint8_t> bytes(Renderer::kPageSizeInBytes);
    bool success = true;
    for (uint32_t begin = 0; begin < count && success; begin += batchSize)
    {
        GraphicsContext& gfx = GraphicsContext::Begin(L"Pin Root Pages Upload");
        for (uint32_t page = begin; page < std::min(count, begin + batchSize); ++page)
        {
            if (m_PageTableCPU[page].IsPinned) continue;
            const auto req = MakePageReadRequest(*model, page);
            if (!ReadPageFromFile(req, bytes))
            {
                Utility::Printf(L"Root initialization: read/decode failed: %ls page %u\n", s_StreamingFilePath.c_str(), page);
                success = false;
                break;
            }
            const auto slot = m_FreePool.back();
            m_FreePool.pop_back();
            const uint32_t offset = slot.SlotIndex * Renderer::kPageSizeInBytes;
            gfx.WriteBuffer(m_GeometryChunksGPU[slot.ChunkIndex], offset, bytes.data(), bytes.size());
            for (uint32_t id : model->GetPageGroups(page))
                m_GroupDataLocations[id] = {slot.ChunkIndex, offset + model->m_GroupMetadatas[id].OffsetInPage};
            auto& residency = m_PageTableCPU[page];
            residency.ChunkIndex = slot.ChunkIndex;
            residency.SlotIndex = slot.SlotIndex;
            residency.IsPinned = true;
            residency.IsLoading = false;
        }
        for (auto& chunk : m_GeometryChunksGPU)
            gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
        gfx.Finish(true); // Also finish and wait on failure before any resource cleanup.
    }
    if (!success) return false; // Initialize tears down the unpublished model after the waits above.
    GraphicsContext& gfx = GraphicsContext::Begin(L"Publish Root Geometry");
    gfx.WriteBuffer(m_GroupDataLocationGPU, 0, m_GroupDataLocations.data(),
        m_GroupDataLocations.size() * sizeof(Renderer::GroupDataLocation));
    gfx.TransitionResource(m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
    gfx.Finish(true);
    ++s_UploadVersion;
    Utility::Printf("GeometryStreaming: pinnedRootPages=%u chunkAllocatedBytes=%llu\n", count,
        uint64_t(m_GeometryChunksGPU.size()) * Renderer::kChunkSizeInBytes);
    LogChunkStats(L"after pinning root pages");
    return true;
}

void GeometryStreaming::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_IOQueueMutex);
		m_StopIOThread = true;
        m_IOQueue.clear();
	}
	m_IOCV.notify_all();

	if (m_IOThread.joinable())
		m_IOThread.join();

	// Initialization, root pinning and shutdown run on the main thread.
	if (s_StreamingFile.is_open())
		s_StreamingFile.close();
	s_StreamingFile.clear();
	s_StreamingFilePath.clear();

	m_IOQueue.clear();
	// The worker can finish an old page while Shutdown waits for it. Discard
	// those completions before another model reuses page and group indices.
	{
		std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
		m_CompletedPagesQueue.clear();
	}
	m_PagesToLoad.clear();
	m_NeedSyncAddressTable = false;
	m_GroupDataLocations.clear();
	m_PageTableCPU.clear();
	m_FreePool.clear();
	std::fill_n(m_FenceValues, kNumReadbackBuffers, 0ull);
	m_HadFreshCull = false;
	std::fill_n(m_ReadbackAllowsEviction, kNumReadbackBuffers, false);

	m_HierarchyNodesGPU.Destroy();
    m_GroupNodeIndicesGPU.Destroy();
	for (auto& chunk : m_GeometryChunksGPU)
	{
		chunk.Destroy();
	}
	m_GeometryChunksGPU.clear();

	//m_GroupDataLocationCPU.Destroy();
	m_GroupDataLocationGPU.Destroy();
	m_GeometryStreamingRequestMaskGPU.Destroy();
	for (int i = 0; i < kNumReadbackBuffers; ++i)
	{
		m_ReadbackRequestMaskBuffer[i].Destroy();
	}
}

void GeometryStreaming::NotifyCullFrame()
{
	m_HadFreshCull = true;
}

void GeometryStreaming::Update(uint32_t frameIndex)
{
	if (frameIndex < 1) return; // Avoid reading uninitialized data

	Model* sourceModel = ModelInstanceManager::GetSourceModel();
	if (nullptr == sourceModel) return;

	// A frozen/absent main-view cull cannot refresh the pages referenced by
	// its cached visible list. On resume, wait for feedback from a new cull.
	const bool canEvict = !Renderer::IsCullFrozen() && m_HadFreshCull;
	m_HadFreshCull = false;
	if (!canEvict)
		std::fill_n(m_ReadbackAllowsEviction, kNumReadbackBuffers, false);
	bool hasEvictionFeedback = false;

	const uint32_t readbackBufferIndex = frameIndex % kNumReadbackBuffers;
	for (uint32_t slot = 0; slot < kNumReadbackBuffers; ++slot)
	{
		const uint64_t currentFenceValue = m_FenceValues[slot];
		if (currentFenceValue == 0 || !Graphics::g_CommandManager.IsFenceComplete(currentFenceValue))
			continue;
		m_FenceValues[slot] = 0;
		hasEvictionFeedback |= m_ReadbackAllowsEviction[slot];
		m_ReadbackAllowsEviction[slot] = false;

		uint32_t* requestedGroupsMask = (uint32_t*)m_ReadbackRequestMaskBuffer[slot].Map();
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

		m_ReadbackRequestMaskBuffer[slot].Unmap();
	}

	// Reclaim before checking capacity, even if no IO completed this frame.
	if (canEvict && hasEvictionFeedback)
		ImmediateEvict(frameIndex, m_GroupDataLocations.data());

	for (auto it = m_PagesToLoad.begin(); it != m_PagesToLoad.end(); )
	{
		if (!EnsureFreeSlots(1))
			break;

		uint32_t pageIdx = *it;
		m_PageTableCPU[pageIdx].IsLoading = true;
		EnqueueAsyncLoad(pageIdx);
		it = m_PagesToLoad.erase(it);
	}

	SyncMemoryAndAddressTable(frameIndex);
	
	if (m_FenceValues[readbackBufferIndex] == 0)
	{
		// Read requests
		GraphicsContext& gfxContext = GraphicsContext::Begin(L"GeometryStreaming Update");

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_COPY_SOURCE);
		//gfxContext.TransitionResource(m_GPURequestBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
		gfxContext.CopyBufferRegion(
			m_ReadbackRequestMaskBuffer[readbackBufferIndex], 0,
			m_GeometryStreamingRequestMaskGPU, 0, (m_GroupCount + 31) / 32 * 4);

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		gfxContext.ClearUAV(m_GeometryStreamingRequestMaskGPU,0);

		m_FenceValues[readbackBufferIndex] = gfxContext.Finish();
		m_ReadbackAllowsEviction[readbackBufferIndex] = canEvict;
	}
}

void GeometryStreaming::SyncMemoryAndAddressTable(uint32_t frameIndex)
{
	std::deque<LoadedPage> readyPages;
	{
		std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
		readyPages.swap(m_CompletedPagesQueue);
	}
	if (readyPages.empty() && !m_NeedSyncAddressTable) return;

	Model* sourceModel = ModelInstanceManager::GetSourceModel();
	Renderer::GroupDataLocation* locations = m_GroupDataLocations.data();
	// Previous draws, slot overwrites and address-table publication all run
	// on the graphics queue, before the next render. WriteBuffer supplies the
	// COPY_DEST barriers; slots can be reused without a CPU fence wait.
	GraphicsContext& gfx = GraphicsContext::Begin(L"Streaming Residency Update");

	bool uploadedPages = false;
	auto it = readyPages.begin();
	for (; it != readyPages.end(); ++it)
	{
		auto& item = *it;
		if (!item.Success)
		{
			m_PageTableCPU[item.PageIndex].IsLoading = false;
			continue; // Leave nonresident; fresh GPU feedback can request it again.
		}
		if (m_FreePool.empty() && !AllocateChunk())
			break;

		PhysicalSlot slot = m_FreePool.back();
		m_FreePool.pop_back();

		uint32_t byteOffset = slot.SlotIndex * Renderer::kPageSizeInBytes;
		gfx.WriteBuffer(m_GeometryChunksGPU[slot.ChunkIndex], byteOffset, item.Data.data(), item.Data.size());
		uploadedPages = true;

		// Update mappings for all groups in the page
		for (uint32_t gIdx : sourceModel->GetPageGroups(item.PageIndex))
		{
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
		// Push unprocessed ones back to the front of the queue
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
	if (uploadedPages)
		++s_UploadVersion;
}

void GeometryStreaming::EnqueueAsyncLoad(uint32_t pageIdx)
{
	Model* sourceModel = ModelInstanceManager::GetSourceModel();
	auto req = MakePageReadRequest(*sourceModel, pageIdx);
	{
		std::lock_guard<std::mutex> lock(m_IOQueueMutex);
		m_IOQueue.push_back(std::move(req));
	}
	m_IOCV.notify_one();
}

void GeometryStreaming::OnPageIOComplete(uint32_t pageIdx, std::vector<uint8_t>&& pageData, bool success)
{
	std::lock_guard<std::mutex> lock(m_CompletedPagesMutex);
	m_CompletedPagesQueue.push_back({ pageIdx, std::move(pageData), success });
}

void GeometryStreaming::ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* pLocationTable)
{
	struct Candidate {
		uint32_t pageIdx;
		uint64_t lastFrame;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(m_PageTableCPU.size());

	const uint32_t kEvictionGraceFrames = 16;
	for (uint32_t i = 0; i < (uint32_t)m_PageTableCPU.size(); ++i)
	{
		const auto& info = m_PageTableCPU[i];
		if (info.ChunkIndex != INVALID_CHUNK_INDEX && !info.IsPinned && !info.IsLoading)
		{
			if (currentFrame >= info.LastUsedFrame &&
				currentFrame - info.LastUsedFrame >= kEvictionGraceFrames)
			{
				candidates.push_back({ i, info.LastUsedFrame });
			}
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.lastFrame < b.lastFrame;
		});

	uint32_t evictedCount = 0;

	Model* sourceModel = ModelInstanceManager::GetSourceModel();

	for (const auto& cand : candidates)
	{
		uint32_t pIdx = cand.pageIdx;
		PageResidency& page = m_PageTableCPU[pIdx];
		for (uint32_t gIdx : sourceModel->GetPageGroups(pIdx))
		{
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

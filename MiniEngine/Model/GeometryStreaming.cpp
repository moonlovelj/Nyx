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
#include <thread>
#include <mutex>
#include <cassert>
#include <system_error>

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
	constexpr uint32_t kMaxPendingPages = 128;
	constexpr uint32_t kMaxUploadPagesPerFrame = 64;
	uint32_t m_PendingPageCount = 0;

	static void SyncMemoryAndAddressTable(uint32_t frameIndex);
	static void ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* locations);

	std::vector<uint32_t> m_DirtyGroupIndices;

	static void UploadAddressRange(GraphicsContext& gfx, const DynAlloc& upload,
		size_t firstGroup, size_t groupCount, size_t uploadOffset)
	{
		const size_t byteCount = groupCount * sizeof(Renderer::GroupDataLocation);
		// Entries are 8 bytes; WriteBuffer's aligned SIMD copy can overread a range.
		std::memcpy(static_cast<uint8_t*>(upload.DataPtr) + uploadOffset,
			m_GroupDataLocations.data() + firstGroup, byteCount);
		gfx.CopyBufferRegion(m_GroupDataLocationGPU, firstGroup * sizeof(Renderer::GroupDataLocation),
			upload.Buffer, upload.Offset + uploadOffset, byteCount);
	}

	static void UploadDirtyAddressTable(GraphicsContext& gfx)
	{
		if (m_DirtyGroupIndices.empty()) return;
		std::sort(m_DirtyGroupIndices.begin(), m_DirtyGroupIndices.end());
		m_DirtyGroupIndices.erase(std::unique(m_DirtyGroupIndices.begin(), m_DirtyGroupIndices.end()),
			m_DirtyGroupIndices.end());
		const auto upload = gfx.ReserveUploadMemory(m_DirtyGroupIndices.size() * sizeof(Renderer::GroupDataLocation));
		for (size_t begin = 0; begin < m_DirtyGroupIndices.size(); )
		{
			size_t end = begin + 1;
			while (end < m_DirtyGroupIndices.size() &&
				m_DirtyGroupIndices[end] == m_DirtyGroupIndices[end - 1] + 1)
				++end;
			UploadAddressRange(gfx, upload, m_DirtyGroupIndices[begin], end - begin,
				begin * sizeof(Renderer::GroupDataLocation));
			begin = end;
		}
		gfx.TransitionResource(m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
	}

	struct IORequest {
		uint32_t PageIdx;
		uint64_t FileOffset;
		uint32_t CompressedSize;
		uint32_t CompressionType;
	};

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
    // Each reader owns its cursor and scratch; requests contain validated metadata.
    struct PageReader
    {
        std::ifstream File;
        std::vector<char> CompressedScratch;

        bool Open(const std::wstring& path)
        {
            File.open(path, std::ios::binary);
            return File.is_open();
        }

        bool Read(const IORequest& req, std::vector<uint8_t>& data)
        {
            data.resize(Renderer::kPageSizeInBytes);
            const bool compressed = req.CompressionType == Renderer::kGeometryCompressionLZ4;
            if (compressed) CompressedScratch.resize(req.CompressedSize);
            char* destination = compressed ? CompressedScratch.data() : reinterpret_cast<char*>(data.data());
            File.clear();
            File.seekg(static_cast<std::streamoff>(req.FileOffset));
            File.read(destination, req.CompressedSize);
            if (!File) return false;
            return !compressed || LZ4_decompress_safe(CompressedScratch.data(),
                reinterpret_cast<char*>(data.data()), static_cast<int>(req.CompressedSize),
                static_cast<int>(data.size())) == static_cast<int>(data.size());
        }
    };

    // Lifecycle and submissions belong to the main thread. Workers only exchange values.
    class PageIO
    {
    public:
        ~PageIO() { Stop(); }

        bool Start(const std::wstring& path, uint32_t count)
        {
            assert(m_Workers.empty());
            m_Workers.resize(count); // Stable addresses before any thread starts.
            for (auto& worker : m_Workers)
                if (!worker.Reader.Open(path))
                {
                    Utility::Printf(L"GeometryStreaming: cannot open %ls\n", path.c_str());
                    Stop();
                    return false;
                }
            m_Stopping = false;
            try
            {
                for (auto& worker : m_Workers)
                    worker.Thread = std::thread([this, ptr = &worker] { WorkerLoop(*ptr); });
            }
            catch (const std::system_error& error)
            {
                Utility::Printf("GeometryStreaming: cannot start IO workers: %s\n", error.what());
                Stop();
                return false;
            }
            return true;
        }

        void Stop()
        {
            {
                std::lock_guard<std::mutex> lock(m_RequestMutex);
                m_Stopping = true;
                m_Requests.clear();
            }
            m_WorkAvailable.notify_all();
            for (auto& worker : m_Workers)
                if (worker.Thread.joinable()) worker.Thread.join();
            m_Workers.clear();
            m_Completed.clear(); // No producers remain; discard old-model results.
        }

        void Enqueue(IORequest request)
        {
            {
                std::lock_guard<std::mutex> lock(m_RequestMutex);
                assert(!m_Stopping);
                m_Requests.push_back(request);
            }
            m_WorkAvailable.notify_one();
        }

        bool TryPopCompleted(LoadedPage& result)
        {
            std::lock_guard<std::mutex> lock(m_CompletedMutex);
            if (m_Completed.empty()) return false;
            result = std::move(m_Completed.front());
            m_Completed.pop_front();
            return true;
        }

    private:
        struct Worker
        {
            PageReader Reader;
            std::thread Thread;
        };

        void WorkerLoop(Worker& worker)
        {
            for (;;)
            {
                IORequest request;
                {
                    std::unique_lock<std::mutex> lock(m_RequestMutex);
                    m_WorkAvailable.wait(lock, [this] { return m_Stopping || !m_Requests.empty(); });
                    if (m_Stopping) return;
                    request = m_Requests.front();
                    m_Requests.pop_front();
                }
                LoadedPage result{request.PageIdx, {}, false};
                result.Success = worker.Reader.Read(request, result.Data);
                if (!result.Success)
                    Utility::Printf("GeometryStreaming: read/decode failed: page %u\n", request.PageIdx);
                std::lock_guard<std::mutex> lock(m_CompletedMutex);
                m_Completed.push_back(std::move(result));
            }
        }

        std::vector<Worker> m_Workers;
        std::deque<IORequest> m_Requests;
        std::deque<LoadedPage> m_Completed;
        std::mutex m_RequestMutex, m_CompletedMutex;
        std::condition_variable m_WorkAvailable;
        bool m_Stopping = true;
    };

    static PageIO s_IO;

    static bool TrySchedulePage(const Model& model, uint32_t pageIndex)
    {
        // Reserve capacity by count, including completed pages awaiting upload.
        if (m_PendingPageCount >= kMaxPendingPages || !EnsureFreeSlots(m_PendingPageCount + 1))
            return false;
        s_IO.Enqueue(MakePageReadRequest(model, pageIndex));
        m_PageTableCPU[pageIndex].IsLoading = true;
        ++m_PendingPageCount;
        return true;
    }

    static void InstallPage(GraphicsContext& gfx, const Model& model,
        uint32_t pageIndex, const std::vector<uint8_t>& data)
    {
        assert(!m_FreePool.empty());
        const auto slot = m_FreePool.back();
        m_FreePool.pop_back();
        const uint32_t offset = slot.SlotIndex * Renderer::kPageSizeInBytes;
        gfx.WriteBuffer(m_GeometryChunksGPU[slot.ChunkIndex], offset, data.data(), data.size());
        for (uint32_t id : model.GetPageGroups(pageIndex))
        {
            m_GroupDataLocations[id] = {slot.ChunkIndex, offset + model.m_GroupMetadatas[id].OffsetInPage};
            m_DirtyGroupIndices.push_back(id);
        }
        auto& residency = m_PageTableCPU[pageIndex];
        residency.ChunkIndex = slot.ChunkIndex;
        residency.SlotIndex = slot.SlotIndex;
    }

}

uint32_t GeometryStreaming::GetUploadVersion()
{
	return s_UploadVersion;
}

bool GeometryStreaming::Initialize(const Model& model)
{

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
	m_DirtyGroupIndices.clear();
	m_GroupDataLocationGPU.Create(L"Group Data Location GPU", groupCount, sizeof(Renderer::GroupDataLocation));
	GraphicsContext& gfx = GraphicsContext::Begin(L"Initialize Geometry Addresses");
	const auto upload = gfx.ReserveUploadMemory(m_GroupDataLocations.size() * sizeof(Renderer::GroupDataLocation));
	UploadAddressRange(gfx, upload, 0, groupCount, 0);
	gfx.TransitionResource(m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
	gfx.Finish(true);
	Renderer::SetBindlessResourceDescriptor(
		SRV_GROUP_DATA_LOCATION_BUFFER,
		m_GroupDataLocationGPU.GetSRV()
	);
    const uint32_t workerCount = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 16u);
    Utility::Printf("GeometryStreaming: IO workers=%u\n", workerCount);
    return s_IO.Start(model.m_StreamingFilePath, workerCount);
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
    assert(m_PendingPageCount == 0);
    PageReader reader;
    if (!reader.Open(model->m_StreamingFilePath)) return false;

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
            if (!reader.Read(req, bytes))
            {
                Utility::Printf(L"Root initialization: read/decode failed: %ls page %u\n", model->m_StreamingFilePath.c_str(), page);
                success = false;
                break;
            }
            InstallPage(gfx, *model, page, bytes);
            m_PageTableCPU[page].IsPinned = true;
        }
        for (auto& chunk : m_GeometryChunksGPU)
            gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
        gfx.Finish(true); // Also finish and wait on failure before any resource cleanup.
    }
    if (!success) return false; // Initialize tears down the unpublished model after the waits above.
    GraphicsContext& gfx = GraphicsContext::Begin(L"Publish Root Geometry");
    UploadDirtyAddressTable(gfx);
    gfx.Finish(true);
    m_DirtyGroupIndices.clear();
    ++s_UploadVersion;
    Utility::Printf("GeometryStreaming: pinnedRootPages=%u chunkAllocatedBytes=%llu\n", count,
        uint64_t(m_GeometryChunksGPU.size()) * Renderer::kChunkSizeInBytes);
    LogChunkStats(L"after pinning root pages");
    return true;
}

void GeometryStreaming::Shutdown()
{
    s_IO.Stop();
    m_PendingPageCount = 0;
	m_PagesToLoad.clear();
	m_DirtyGroupIndices.clear();
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

    SyncMemoryAndAddressTable(frameIndex);
    for (auto it = m_PagesToLoad.begin(); it != m_PagesToLoad.end(); )
    {
        if (!TrySchedulePage(*sourceModel, *it)) break;
        it = m_PagesToLoad.erase(it);
    }

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
    LoadedPage page;
    bool hasPage = s_IO.TryPopCompleted(page);
    if (!hasPage && m_DirtyGroupIndices.empty()) return;

    Model* sourceModel = ModelInstanceManager::GetSourceModel();
    // Slot reuse and address publication precede the next draw on the graphics queue.
    GraphicsContext& gfx = GraphicsContext::Begin(L"Streaming Residency Update");
    uint32_t uploadedPages = 0;
    while (hasPage)
    {
        assert(m_PendingPageCount > 0 && m_PageTableCPU[page.PageIndex].IsLoading);
        if (page.Success)
        {
            InstallPage(gfx, *sourceModel, page.PageIndex, page.Data);
            m_PageTableCPU[page.PageIndex].LastUsedFrame = frameIndex;
            ++uploadedPages;
        }
        // Failed pages remain nonresident; fresh GPU feedback may request them again.
        m_PageTableCPU[page.PageIndex].IsLoading = false;
        --m_PendingPageCount;
        if (uploadedPages == kMaxUploadPagesPerFrame) break;
        hasPage = s_IO.TryPopCompleted(page);
    }
    assert(m_FreePool.size() >= m_PendingPageCount);

	UploadDirtyAddressTable(gfx);

	for (auto& chunk : m_GeometryChunksGPU)
	{
		gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
	}
	gfx.Finish();
	m_DirtyGroupIndices.clear();
	if (uploadedPages)
		++s_UploadVersion;
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

	Model* sourceModel = ModelInstanceManager::GetSourceModel();

	for (const auto& cand : candidates)
	{
		uint32_t pIdx = cand.pageIdx;
		PageResidency& page = m_PageTableCPU[pIdx];
		for (uint32_t gIdx : sourceModel->GetPageGroups(pIdx))
		{
			pLocationTable[gIdx].ChunkIndex = INVALID_CHUNK_INDEX;
			pLocationTable[gIdx].ByteOffset = 0;
			m_DirtyGroupIndices.push_back(gIdx);
		}

		m_FreePool.push_back({ page.ChunkIndex, page.SlotIndex });
		page.ChunkIndex = INVALID_CHUNK_INDEX;
		page.SlotIndex = 0;
	}
}

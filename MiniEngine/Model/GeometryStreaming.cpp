#include "GeometryPageLayout.h"
#include "GeometryStreaming.h"
#include "GeometryStreamingShared.h"
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
#include <chrono>

namespace GeometryStreaming
{
    using Clock = std::chrono::steady_clock;
    struct ChunkLifetime
    {
        uint32_t Descriptor = INVALID_CHUNK_INDEX;
        std::vector<uint32_t> FreeSlots;
        bool Draining = false;
        Clock::time_point EmptySince{};
        uint64_t Fence = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> RetiredResource;
    };
    static std::vector<ChunkLifetime> s_ChunkLifetimes;
    static bool s_MemoryPressure = false;
    static uint64_t s_AllocatedBytes = 0;
    static size_t s_FreeSlotCount = 0; // Allocatable slots only; excludes draining chunks.
    // Nonzero records failed growth until allocation succeeds; expiry only permits another attempt.
    static Clock::time_point s_RetryAllocationAfter{};

    static uint64_t AllocationHeadroom()
    {
        Graphics::VideoMemoryInfo info{};
        return Graphics::QueryVideoMemoryInfo(info) && info.Budget > info.CurrentUsage
            ? info.Budget - info.CurrentUsage : 0;
    }

    // Raw process budget: enter below 10% headroom, leave at 15%.
    static uint64_t UpdateMemoryBudget(const Graphics::VideoMemoryInfo& info)
    {
        if (!info.Budget) return s_AllocatedBytes; // No reliable sample; do not start new budget eviction.
        const uint64_t stopUsage = info.Budget - info.Budget * 15 / 100;
        if (info.CurrentUsage <= stopUsage)
            s_MemoryPressure = false;
        else if (info.CurrentUsage > info.Budget - info.Budget / 10)
            s_MemoryPressure = true;
        if (!s_MemoryPressure) return s_AllocatedBytes;
        const uint64_t reclaimBytes = info.CurrentUsage - stopUsage;
        return s_AllocatedBytes > reclaimBytes ? s_AllocatedBytes - reclaimBytes : 0;
    }

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

    std::deque<uint32_t> m_PagesToLoad;
    // Main bits describe the latest main cull; shadow bits persist until installation.
    std::vector<uint8_t> m_GroupRequests;
    std::vector<uint8_t> m_PagePriorities;

    static uint32_t RequestWordCount(uint32_t groupCount)
    {
        return std::max(1u, (groupCount + GEOMETRY_REQUEST_GROUPS_PER_WORD - 1u) /
            GEOMETRY_REQUEST_GROUPS_PER_WORD);
    }

	ByteAddressBuffer m_GeometryStreamingRequestMaskGPU;
	ReadbackBuffer m_ReadbackRequestMaskBuffer[kNumReadbackBuffers];
	uint64_t m_FenceValues[kNumReadbackBuffers] = {};
	bool m_HadFreshCull = false;
	bool m_ReadbackAllowsEviction[kNumReadbackBuffers] = {};
    bool m_ReadbackHasMainCull[kNumReadbackBuffers] = {};
    bool m_RequestMaskHasMainCull = false;

	struct LoadedPage {
		uint32_t PageIndex;
		std::vector<uint8_t> Data;
		bool Success;
	};
	constexpr uint32_t kMaxPendingPages = 128;
	constexpr uint32_t kMaxUploadPagesPerFrame = 64;
	uint32_t m_PendingPageCount = 0;

	static void SyncMemoryAndAddressTable(uint32_t frameIndex);
	static void ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* locations,
        size_t requiredFreeSlots = SIZE_MAX, uint64_t budget = UINT64_MAX);
    static void TrimChunks(uint32_t currentFrame, uint64_t budget);

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

    // Best fit: fill the fullest usable chunk before touching a sparse one.
    static uint32_t FindAllocationChunk()
    {
        uint32_t best = INVALID_CHUNK_INDEX;
        for (uint32_t index = 0; index < s_ChunkLifetimes.size(); ++index)
        {
            const auto& chunk = s_ChunkLifetimes[index];
            if (chunk.Draining || chunk.FreeSlots.empty()) continue;
            if (best == INVALID_CHUNK_INDEX || chunk.FreeSlots.size() < s_ChunkLifetimes[best].FreeSlots.size())
                best = index;
        }
        return best;
    }

    static void LogChunkStats(const wchar_t* tag)
    {
        Utility::Printf(L"GeometryStreaming: %ls allocated=%llu MB, pressure=%u, freeSlots=%zu\n",
            tag, s_AllocatedBytes >> 20, unsigned(s_MemoryPressure), s_FreeSlotCount);
    }

    static bool AllocateChunk()
    {
        const auto now = Clock::now();
        if (now < s_RetryAllocationAfter) return false;
        // Any failed growth attempt backs off; success clears the deadline below.
        s_RetryAllocationAfter = now + std::chrono::milliseconds(500);
        if (AllocationHeadroom() < Renderer::kChunkSizeInBytes) return false;
        uint32_t index = 0;
        while (index < m_GeometryChunksGPU.size() &&
            (m_GeometryChunksGPU[index].GetResource() || s_ChunkLifetimes[index].Fence)) ++index;
        if (index == m_GeometryChunksGPU.size())
        {
            const uint32_t descriptor = Renderer::AllocateGeometryDescriptor();
            if (descriptor == INVALID_CHUNK_INDEX) return false;
            m_GeometryChunksGPU.emplace_back();
            s_ChunkLifetimes.emplace_back();
            s_ChunkLifetimes.back().Descriptor = descriptor;
        }
        auto& chunk = m_GeometryChunksGPU[index];
        const HRESULT result = chunk.TryCreate(L"Geometry Chunk", Renderer::kChunkSizeInBytes / 4, 4);
        if (FAILED(result))
        {
            Utility::Printf("GeometryStreaming: chunk allocation failed (0x%08X)\n", result);
            return false;
        }
        auto& lifetime = s_ChunkLifetimes[index];
        lifetime.EmptySince = now;
        lifetime.Draining = false;
        Graphics::g_Device->CopyDescriptorsSimple(1, Renderer::s_TextureHeap[lifetime.Descriptor],
            chunk.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s_AllocatedBytes += Renderer::kChunkSizeInBytes;
        lifetime.FreeSlots.resize(m_SlotsPerChunk);
        std::iota(lifetime.FreeSlots.begin(), lifetime.FreeSlots.end(), 0u);
        s_FreeSlotCount += m_SlotsPerChunk;
        s_RetryAllocationAfter = {};
        LogChunkStats(L"allocated chunk");
        return true;
    }

    static bool ReactivateDrainingChunk()
    {
        for (uint32_t index = 0; index < s_ChunkLifetimes.size(); ++index)
        {
            auto& chunk = s_ChunkLifetimes[index];
            // Retired chunks no longer have an active resource; leave them to fence collection.
            if (!chunk.Draining || !m_GeometryChunksGPU[index].GetResource()) continue;
            assert(chunk.FreeSlots.size() == m_SlotsPerChunk);
            chunk.Draining = false;
            chunk.EmptySince = Clock::now();
            s_FreeSlotCount += chunk.FreeSlots.size();
            LogChunkStats(L"reused draining chunk");
            return true;
        }
        return false;
    }

    static bool EnsureFreeSlots(uint32_t requiredSlots)
    {
        while (s_FreeSlotCount < requiredSlots)
            if (!ReactivateDrainingChunk() && !AllocateChunk()) return false;
        return true;
    }

    // IO reserves capacity by count; never retire slots needed by pending uploads.
    static void ReleaseEmptyChunks(Clock::time_point now)
    {
        bool released = false; // Called once per frame; spread resource destruction across frames.
        for (uint32_t index = 0; index < m_GeometryChunksGPU.size(); ++index)
        {
            auto& lifetime = s_ChunkLifetimes[index];
            if (lifetime.Fence)
            {
                if (!released && Graphics::g_CommandManager.IsFenceComplete(lifetime.Fence))
                {
                    lifetime.RetiredResource.Reset();
                    released = true;
                    lifetime.Fence = 0;
                    s_AllocatedBytes -= Renderer::kChunkSizeInBytes;
                    LogChunkStats(L"released chunk");
                }
                continue;
            }
            if (!m_GeometryChunksGPU[index].GetResource()) continue;
            if (lifetime.FreeSlots.size() != m_SlotsPerChunk)
            {
                lifetime.EmptySince = now;
                continue;
            }
            if (now - lifetime.EmptySince < std::chrono::seconds(5) ||
                (!lifetime.Draining && s_FreeSlotCount - m_SlotsPerChunk < m_PendingPageCount)) continue;
            if (!lifetime.Draining) s_FreeSlotCount -= m_SlotsPerChunk;
            lifetime.FreeSlots.clear();
            // All geometry consumers and address updates use the graphics queue.
            lifetime.Fence = Graphics::g_CommandManager.GetGraphicsQueue().IncrementFence();
            lifetime.RetiredResource = m_GeometryChunksGPU[index].GetResource();
            m_GeometryChunksGPU[index].Destroy();
        }
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

        // The same lock guards worker dequeue: returned requests have not started IO.
        std::deque<IORequest> TakeQueued()
        {
            std::lock_guard<std::mutex> lock(m_RequestMutex);
            std::deque<IORequest> queued;
            queued.swap(m_Requests);
            return queued;
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

    static void ApplyRequestFeedback(const Model& model, const uint32_t* words,
        bool hasMainCull, uint32_t frameIndex)
    {
        if (hasMainCull)
            for (auto& mask : m_GroupRequests) mask &= GEOMETRY_REQUEST_SHADOW;

        for (uint32_t wordIndex = 0; wordIndex < RequestWordCount(m_GroupCount); ++wordIndex)
        {
            uint32_t word = words[wordIndex];
            unsigned long bit;
            while (_BitScanForward(&bit, word))
            {
                const uint32_t shift = bit & ~1u;
                const uint32_t group = wordIndex * GEOMETRY_REQUEST_GROUPS_PER_WORD + shift / 2u;
                const uint8_t mask = uint8_t((word >> shift) & GEOMETRY_REQUEST_MASK);
                word &= ~(GEOMETRY_REQUEST_MASK << shift);
                if (group >= m_GroupCount) continue; // Padding in the last word.
                const uint32_t page = model.m_GroupMetadatas[group].PageIndex;
                m_PageTableCPU[page].LastUsedFrame = frameIndex;
                m_GroupRequests[group] |= mask;
                if (m_PageTableCPU[page].ChunkIndex != INVALID_CHUNK_INDEX)
                    m_GroupRequests[group] &= GEOMETRY_REQUEST_MAIN;
            }
        }
    }

    static void RebuildPendingPages(const Model& model)
    {
        for (const auto& request : s_IO.TakeQueued())
        {
            m_PageTableCPU[request.PageIdx].IsLoading = false;
            --m_PendingPageCount;
        }
        std::fill(m_PagePriorities.begin(), m_PagePriorities.end(), uint8_t(0));
        for (uint32_t group = 0; group < m_GroupCount; ++group)
        {
            const uint32_t page = model.m_GroupMetadatas[group].PageIndex;
            m_PagePriorities[page] = std::max(m_PagePriorities[page], m_GroupRequests[group]);
        }
        m_PagesToLoad.clear();
        for (uint32_t page = 0; page < m_PageTableCPU.size(); ++page)
            if (m_PagePriorities[page] && !m_PageTableCPU[page].IsLoading &&
                m_PageTableCPU[page].ChunkIndex == INVALID_CHUNK_INDEX)
                m_PagesToLoad.push_back(page);
        std::sort(m_PagesToLoad.begin(), m_PagesToLoad.end(), [](uint32_t a, uint32_t b) {
            return m_PagePriorities[a] != m_PagePriorities[b]
                ? m_PagePriorities[a] > m_PagePriorities[b] : a < b;
        });
    }

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
        const uint32_t chunkIndex = FindAllocationChunk();
        assert(chunkIndex != INVALID_CHUNK_INDEX);
        auto& chunk = s_ChunkLifetimes[chunkIndex];
        const uint32_t slotIndex = chunk.FreeSlots.back();
        chunk.FreeSlots.pop_back();
        --s_FreeSlotCount;
        const uint32_t offset = slotIndex * Renderer::kPageSizeInBytes;
        gfx.WriteBuffer(m_GeometryChunksGPU[chunkIndex], offset, data.data(), data.size());
        for (uint32_t id : model.GetPageGroups(pageIndex))
        {
            m_GroupRequests[id] &= GEOMETRY_REQUEST_MAIN;
            m_GroupDataLocations[id] = {s_ChunkLifetimes[chunkIndex].Descriptor, offset + model.m_GroupMetadatas[id].OffsetInPage};
            m_DirtyGroupIndices.push_back(id);
        }
        auto& residency = m_PageTableCPU[pageIndex];
        s_ChunkLifetimes[chunkIndex].EmptySince = Clock::now();
        residency.ChunkIndex = chunkIndex;
        residency.SlotIndex = slotIndex;
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
    m_GroupRequests.assign(m_GroupCount, 0);
    m_RequestMaskHasMainCull = false;
    m_PagePriorities.assign(numPages, 0);
    m_PagesToLoad.clear();
    std::fill_n(m_ReadbackHasMainCull, kNumReadbackBuffers, false);

    const uint32_t requestWordCount = RequestWordCount(m_GroupCount);
    std::vector<uint32_t> initialRequestMask(requestWordCount, 0u);
	m_GeometryStreamingRequestMaskGPU.Create(L"Geometry Streaming Request Mask", requestWordCount, sizeof(uint32_t), initialRequestMask.data());
	Renderer::SetBindlessResourceDescriptor(UAV_GEOMETRY_STREAMING_REQUEST_MASK_BUFFER, m_GeometryStreamingRequestMaskGPU.GetUAV());

	for (int i = 0; i < kNumReadbackBuffers; ++i)
	{
		m_ReadbackRequestMaskBuffer[i].Create(L"Readback Request Mask Buffer", requestWordCount, sizeof(uint32_t));
		m_FenceValues[i] = 0;
	}

	m_SlotsPerChunk = Renderer::kChunkSizeInBytes / Renderer::kPageSizeInBytes;
	s_FreeSlotCount = 0;
	m_GeometryChunksGPU.clear();
    s_AllocatedBytes = 0;
    s_RetryAllocationAfter = {};
    s_MemoryPressure = false;

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
    if (!EnsureFreeSlots(needed))
    {
        Utility::Printf("Root initialization: cannot allocate %u slots\n", needed);
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
            if (chunk.GetResource()) gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
        gfx.Finish(true); // Also finish and wait on failure before any resource cleanup.
    }
    if (!success) return false; // Initialize tears down the unpublished model after the waits above.
    GraphicsContext& gfx = GraphicsContext::Begin(L"Publish Root Geometry");
    UploadDirtyAddressTable(gfx);
    gfx.Finish(true);
    m_DirtyGroupIndices.clear();
    ++s_UploadVersion;
    Utility::Printf("GeometryStreaming: pinnedRootPages=%u chunkAllocatedBytes=%llu\n", count,
        s_AllocatedBytes);
    LogChunkStats(L"after pinning root pages");
    return true;
}

void GeometryStreaming::Shutdown()
{
    s_IO.Stop();
    m_PendingPageCount = 0;
	m_PagesToLoad.clear();
    m_GroupRequests.clear();
    m_RequestMaskHasMainCull = false;
    m_PagePriorities.clear();
    std::fill_n(m_ReadbackHasMainCull, kNumReadbackBuffers, false);
	m_DirtyGroupIndices.clear();
	m_GroupDataLocations.clear();
	m_PageTableCPU.clear();
	s_FreeSlotCount = 0;
	std::fill_n(m_FenceValues, kNumReadbackBuffers, 0ull);
	m_HadFreshCull = false;
	std::fill_n(m_ReadbackAllowsEviction, kNumReadbackBuffers, false);

    for (auto& lifetime : s_ChunkLifetimes)
    {
        if (lifetime.Fence) Graphics::g_CommandManager.WaitForFence(lifetime.Fence);
        Renderer::FreeGeometryDescriptor(lifetime.Descriptor);
    }
    s_ChunkLifetimes.clear();
    s_AllocatedBytes = 0;
    s_MemoryPressure = false;
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
    const bool hadMainCull = m_HadFreshCull;
    m_RequestMaskHasMainCull |= hadMainCull;
	const bool canEvict = !Renderer::IsCullFrozen() && hadMainCull;
	m_HadFreshCull = false;
	if (!canEvict)
		std::fill_n(m_ReadbackAllowsEviction, kNumReadbackBuffers, false);
	bool hasEvictionFeedback = false;

	const uint32_t readbackBufferIndex = frameIndex % kNumReadbackBuffers;
    // Ring indices are not chronological when multiple readbacks complete together.
    std::vector<uint32_t> readySlots;
    for (uint32_t slot = 0; slot < kNumReadbackBuffers; ++slot)
        if (m_FenceValues[slot] && Graphics::g_CommandManager.IsFenceComplete(m_FenceValues[slot]))
            readySlots.push_back(slot);
    std::sort(readySlots.begin(), readySlots.end(), [](uint32_t a, uint32_t b) {
        return m_FenceValues[a] < m_FenceValues[b];
    });
	for (uint32_t slot : readySlots)
	{
		m_FenceValues[slot] = 0;
		hasEvictionFeedback |= m_ReadbackAllowsEviction[slot];
		m_ReadbackAllowsEviction[slot] = false;

		uint32_t* requestedGroupsMask = (uint32_t*)m_ReadbackRequestMaskBuffer[slot].Map();
        ApplyRequestFeedback(*sourceModel, requestedGroupsMask, m_ReadbackHasMainCull[slot], frameIndex);

		m_ReadbackRequestMaskBuffer[slot].Unmap();
	}

    if (!readySlots.empty()) RebuildPendingPages(*sourceModel);
    Graphics::VideoMemoryInfo memory{};
    Graphics::QueryVideoMemoryInfo(memory);
    const uint64_t budget = UpdateMemoryBudget(memory);
    if (canEvict && hasEvictionFeedback)
    {
        TrimChunks(frameIndex, budget);
        // Slot pressure and physical allocation pressure have separate stopping conditions.
        const size_t requiredSlots = m_PendingPageCount +
            std::min<size_t>(m_PagesToLoad.size(), kMaxPendingPages - m_PendingPageCount);
        // Failed growth can reuse cold slots even after the allocation retry deadline has expired.
        if (s_FreeSlotCount < requiredSlots && (s_MemoryPressure || s_RetryAllocationAfter != Clock::time_point{} ||
            AllocationHeadroom() < Renderer::kChunkSizeInBytes))
            ImmediateEvict(frameIndex, m_GroupDataLocations.data(), requiredSlots);
    }

    SyncMemoryAndAddressTable(frameIndex);
    while (!m_PagesToLoad.empty())
    {
        if (!TrySchedulePage(*sourceModel, m_PagesToLoad.front())) break;
        m_PagesToLoad.pop_front();
    }
    ReleaseEmptyChunks(Clock::now()); // Give new requests first use of idle capacity.

	if (m_FenceValues[readbackBufferIndex] == 0)
	{
		// Read requests
		GraphicsContext& gfxContext = GraphicsContext::Begin(L"GeometryStreaming Update");

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_COPY_SOURCE);
		//gfxContext.TransitionResource(m_GPURequestBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
		gfxContext.CopyBufferRegion(
			m_ReadbackRequestMaskBuffer[readbackBufferIndex], 0,
			m_GeometryStreamingRequestMaskGPU, 0, RequestWordCount(m_GroupCount) * sizeof(uint32_t));

		gfxContext.TransitionResource(m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		gfxContext.ClearUAV(m_GeometryStreamingRequestMaskGPU,0);

		m_FenceValues[readbackBufferIndex] = gfxContext.Finish();
		m_ReadbackAllowsEviction[readbackBufferIndex] = canEvict;
        m_ReadbackHasMainCull[readbackBufferIndex] = m_RequestMaskHasMainCull;
        m_RequestMaskHasMainCull = false;
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
    assert(s_FreeSlotCount >= m_PendingPageCount);

	UploadDirtyAddressTable(gfx);

	for (auto& chunk : m_GeometryChunksGPU)
	{
		if (chunk.GetResource()) gfx.TransitionResource(chunk, D3D12_RESOURCE_STATE_GENERIC_READ);
	}
	gfx.Finish();
	m_DirtyGroupIndices.clear();
	if (uploadedPages)
		++s_UploadVersion;
}

namespace GeometryStreaming
{
    static bool CanEvict(const PageResidency& page, uint32_t frame)
    {
        return !page.IsPinned && !page.IsLoading && frame >= page.LastUsedFrame && frame - page.LastUsedFrame > 16;
    }

    static void EvictPage(uint32_t pageIndex, Renderer::GroupDataLocation* locations)
    {
        auto& page = m_PageTableCPU[pageIndex];
        for (uint32_t group : ModelInstanceManager::GetSourceModel()->GetPageGroups(pageIndex))
        {
            locations[group] = {INVALID_CHUNK_INDEX, 0};
            m_DirtyGroupIndices.push_back(group);
        }
        auto& chunk = s_ChunkLifetimes[page.ChunkIndex];
        chunk.FreeSlots.push_back(page.SlotIndex);
        if (!chunk.Draining) ++s_FreeSlotCount;
        if (chunk.FreeSlots.size() == m_SlotsPerChunk) chunk.EmptySince = Clock::now();
        page.ChunkIndex = INVALID_CHUNK_INDEX;
        page.SlotIndex = 0;
    }

    static void TrimChunks(uint32_t currentFrame, uint64_t budget)
    {
        ImmediateEvict(currentFrame, m_GroupDataLocations.data(), 0, budget);
    }
}

void GeometryStreaming::ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* pLocationTable,
    size_t requiredFreeSlots, uint64_t budget)
{
    // Already-draining and fence-pending resources count toward the planned release.
    uint64_t remainingBytes = 0;
    for (uint32_t index = 0; index < s_ChunkLifetimes.size(); ++index)
        if (m_GeometryChunksGPU[index].GetResource() && !s_ChunkLifetimes[index].Draining)
            remainingBytes += Renderer::kChunkSizeInBytes;
    if (s_FreeSlotCount >= requiredFreeSlots && remainingBytes <= budget) return;

    std::vector<uint32_t> emptyChunks;
    if (remainingBytes > budget)
        for (uint32_t index = 0; index < s_ChunkLifetimes.size(); ++index)
            if (m_GeometryChunksGPU[index].GetResource() && !s_ChunkLifetimes[index].Draining &&
                s_ChunkLifetimes[index].FreeSlots.size() == m_SlotsPerChunk)
                emptyChunks.push_back(index);
    auto drainEmptyChunks = [&]()
    {
        while (remainingBytes > budget && !emptyChunks.empty())
        {
            // Global IO reservations must still fit outside this empty chunk.
            if (s_FreeSlotCount - m_SlotsPerChunk < m_PendingPageCount) break;
            const uint32_t index = emptyChunks.back();
            emptyChunks.pop_back();
            s_ChunkLifetimes[index].Draining = true;
            s_FreeSlotCount -= m_SlotsPerChunk;
            remainingBytes -= Renderer::kChunkSizeInBytes;
            Utility::Printf("GeometryStreaming: draining empty chunk %u\n", index);
        }
    };
    drainEmptyChunks();
    if (s_FreeSlotCount >= requiredFreeSlots && remainingBytes <= budget) return;

	struct Candidate {
		uint32_t pageIdx;
		uint64_t lastFrame;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(m_PageTableCPU.size());

    for (uint32_t page = 0; page < m_PageTableCPU.size(); ++page)
        if (m_PageTableCPU[page].ChunkIndex != INVALID_CHUNK_INDEX && CanEvict(m_PageTableCPU[page], currentFrame))
            candidates.push_back({page, m_PageTableCPU[page].LastUsedFrame});

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.lastFrame != b.lastFrame ? a.lastFrame < b.lastFrame : a.pageIdx < b.pageIdx;
		});

    for (const auto& candidate : candidates)
    {
        if (s_FreeSlotCount >= requiredFreeSlots && remainingBytes <= budget) break;
        const uint32_t chunkIndex = m_PageTableCPU[candidate.pageIdx].ChunkIndex;
        EvictPage(candidate.pageIdx, pLocationTable);
        if (remainingBytes > budget)
        {
            if (s_ChunkLifetimes[chunkIndex].FreeSlots.size() == m_SlotsPerChunk)
                emptyChunks.push_back(chunkIndex);
            // New free slots may also unblock an empty chunk held back by IO reservations.
            drainEmptyChunks();
        }
    }
}

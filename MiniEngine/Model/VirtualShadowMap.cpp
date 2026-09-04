#include "VirtualShadowMap.h"
#include "VirtualShadowMapCullShared.h"

#include "CommandBucketer.h"
#include "GeometryStreaming.h"
#include "Renderer.h"
#include "TemporalEffects.h"
#include "../Core/BufferManager.h"
#include "../Core/DepthBuffer.h"
#include "../Core/EngineProfiling.h"
#include "../Core/EngineTuning.h"
#include "../Core/GpuBuffer.h"
#include "../Core/GraphicsCore.h"
#include "../Core/HierarchicalDepthBuffer.h"
#include "../Core/PipelineState.h"
#include "../Core/ProgramBinder.h"
#include "../Core/ProgramUtils.h"
#include "../Core/ReadbackBuffer.h"
#include "../Core/Utility.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace Graphics;

namespace Renderer::VirtualShadowMap
{
    namespace
    {
        IntVar s_PhysicalPageRenderBudget(
            "Renderer/VSM/Physical Page Render Budget",
            64,
            1,
            static_cast<int32_t>(kPhysicalPageCapacity),
            1);

        IntVar s_RequestedPageCount("Renderer/VSM/Page Statistics/Requested", 0);
        IntVar s_ReusedPageCount("Renderer/VSM/Page Statistics/Reused", 0);
        IntVar s_NewPageCount("Renderer/VSM/Page Statistics/New", 0);
        IntVar s_OverflowPageCount("Renderer/VSM/Page Statistics/Overflow", 0);
        IntVar s_CoarseMappedPageCount("Renderer/VSM/Page Statistics/Coarse Mapped", 0);
        IntVar s_CoarseOverflowPageCount("Renderer/VSM/Page Statistics/Coarse Overflow", 0);
        IntVar s_RenderRequestCount("Renderer/VSM/Page Statistics/Render Request", 0);
        IntVar s_RenderDataPageCount("Renderer/VSM/Page Statistics/Render Data Pages", 0);
        IntVar s_ActiveRenderViewCount("Renderer/VSM/Page Statistics/Active Render Views", 0);
        IntVar s_RenderMaskPageCount("Renderer/VSM/Page Statistics/Render Mask Pages", 0);
        IntVar s_InvalidRenderRequestCount("Renderer/VSM/Page Statistics/Invalid Render Requests", 0);
        IntVar s_RasterItemCount("Renderer/VSM/Cull Statistics/Raster Items", 0);
        IntVar s_CullOverflowCount("Renderer/VSM/Cull Statistics/Overflow", 0);
        IntVar s_RenderBacklogCount("Renderer/VSM/Page Statistics/Render Backlog", 0);
        BoolVar s_PhysicalPoolExhausted("Renderer/VSM/Page Statistics/Physical Pool Exhausted", false);
        BoolVar s_RenderBudgetExceeded("Renderer/VSM/Page Statistics/Render Budget Exceeded", false);
        BoolVar s_PageCountersValid("Renderer/VSM/Page Statistics/Counters Valid", false);

        bool s_Initialized = false;

        ComputePSO s_MarkDirectionalPagesPSO(L"VSM: Mark Directional Pages");
        ComputePSO s_MarkLocalPagesPSO(L"VSM: Mark Local Pages");
        ComputePSO s_CommitResidencyStatesPSO(L"VSM: Commit Residency States");
        ComputePSO s_UpdateResidencyStatesPSO(L"VSM: Update Residency States");
        std::shared_ptr<Program> s_MarkDirectionalPagesProgram;
        std::shared_ptr<Program> s_MarkLocalPagesProgram;
        std::shared_ptr<Program> s_CommitResidencyStatesProgram;
        std::shared_ptr<Program> s_UpdateResidencyStatesProgram;
        ComputePSO s_MarkViewDirtyPSO(L"VSM: Mark View Dirty");
        ComputePSO s_ReuseRequestedPagesPSO(L"VSM: Reuse Requested Pages");
        ComputePSO s_BuildFreePhysicalPageListPSO(L"VSM: Build Free Physical Page List");
        ComputePSO s_AllocateNewPagesPSO(L"VSM: Allocate New Pages");
        ComputePSO s_MarkPhysicalPageRenderedPSO(L"VSM: Mark Physical Page Rendered");
        ComputePSO s_BuildPhysicalPageRenderDataPSO(L"VSM: Build Physical Page Render Data");
        ComputePSO s_BuildPageFlagsMip0PSO(L"VSM: Build Page Flags Mip 0");
        ComputePSO s_BuildPageFlagsMipPSO(L"VSM: Build Page Flags Mip");
        ComputePSO s_BuildRasterWindowMasksPSO(L"VSM: Build Raster Window Masks");
        ComputePSO s_BuildCullDispatchArgsPSO(L"VSM: Build Cull Dispatch Args");
        ComputePSO s_BuildRasterDispatchArgsPSO(L"VSM: Build Raster Dispatch Args");
        ComputePSO s_MultiViewInstanceCullPSO(L"VSM: Multi-View Instance Cull");
        ComputePSO s_MultiViewDAGCullPSO(L"VSM: Multi-View DAG Cull");
        MeshShaderPSO s_DirectionalRasterDepthPSO(L"VSM: Directional Raster Depth");
        ComputePSO s_PhysicalPageInstanceCullPSOs[2] = {
            ComputePSO(L"VSM: Physical Page Instance Cull Pass 0"),
            ComputePSO(L"VSM: Physical Page Instance Cull Pass 1")
        };
        ComputePSO s_PhysicalPageDAGCullPSOs[2] = {
            ComputePSO(L"VSM: Physical Page DAG Cull Pass 0"),
            ComputePSO(L"VSM: Physical Page DAG Cull Pass 1")
        };
        ComputePSO s_PhysicalPageMeshBufferGenPSO(L"VSM: Physical Page Mesh Buffer Gen");
        GraphicsPSO s_ClearRequestedPhysicalPagePSO(L"VSM: Clear Requested Physical Page");
        MeshShaderPSO s_PhysicalPageDepthPSOs[2] = {
            MeshShaderPSO(L"VSM: Physical Page Depth Pass 0"),
            MeshShaderPSO(L"VSM: Physical Page Depth Pass 1")
        };
        std::shared_ptr<Program> s_MarkViewDirtyProgram;
        std::shared_ptr<Program> s_ReuseRequestedPagesProgram;
        std::shared_ptr<Program> s_BuildFreePhysicalPageListProgram;
        std::shared_ptr<Program> s_AllocateNewPagesProgram;
        std::shared_ptr<Program> s_MarkPhysicalPageRenderedProgram;
        std::shared_ptr<Program> s_BuildPhysicalPageRenderDataProgram;
        std::shared_ptr<Program> s_BuildPageFlagsMip0Program;
        std::shared_ptr<Program> s_BuildPageFlagsMipProgram;
        std::shared_ptr<Program> s_BuildRasterWindowMasksProgram;
        std::shared_ptr<Program> s_BuildCullDispatchArgsProgram;
        std::shared_ptr<Program> s_BuildRasterDispatchArgsProgram;
        std::shared_ptr<Program> s_MultiViewInstanceCullProgram;
        std::shared_ptr<Program> s_MultiViewDAGCullProgram;
        std::shared_ptr<Program> s_DirectionalRasterDepthProgram;
        std::shared_ptr<Program> s_ClearRequestedPhysicalPageProgram;
        std::shared_ptr<Program> s_PhysicalPageInstanceCullPrograms[2];
        std::shared_ptr<Program> s_PhysicalPageDAGCullPrograms[2];
        std::shared_ptr<Program> s_PhysicalPageMeshBufferGenProgram;
        std::shared_ptr<Program> s_PhysicalPageDepthPrograms[2];

        StructuredBuffer s_ShadowViewsGpu;
        StructuredBuffer s_DirectionalClipmapsGpu;
        StructuredBuffer s_ResidencyStatesGpu;
        StructuredBuffer s_DirectionalAddressesGpu;
        StructuredBuffer s_ProjectionsGpu;
        ByteAddressBuffer s_PageRequestMaskGpu;
        ByteAddressBuffer s_RequestStatisticsGpu;
        std::array<StructuredBuffer, 2> s_PageTablesGpu;
        StructuredBuffer s_PageRenderRequestsGpu;
        StructuredBuffer s_PhysicalPageMetadataGpu;
        StructuredBuffer s_FreePhysicalPagesGpu;
        ByteAddressBuffer s_PhysicalPageUsedMaskGpu;
        ByteAddressBuffer s_PageManagementCountersGpu;
        ByteAddressBuffer s_RenderRequestPredicateGpu;
        StructuredBuffer s_PhysicalPageViewsGpu;
        ByteAddressBuffer s_PageRenderMaskGpu;
        ByteAddressBuffer s_ActiveViewMaskGpu;
        StructuredBuffer s_ActiveViewsGpu;
        ByteAddressBuffer s_PhysicalPageRenderCountersGpu;

        struct VsmPhysicalPageCullResources
        {
            StructuredBuffer QueueStateGpu;
            StructuredBuffer LegacyVisibleMeshletsGpu;
            StructuredBuffer IndirectDispatchMeshGpu;

            void Create()
            {
                QueueStateGpu.Create(L"VSM Physical Page Cull Queue State", 1, sizeof(QueueState));
                LegacyVisibleMeshletsGpu.Create(
                    L"VSM Physical Page Legacy Visible Meshlets",
                    MAX_VISIBLE_MESHLETS,
                    sizeof(VisibleMeshletPayload));
                IndirectDispatchMeshGpu.Create(
                    L"VSM Physical Page Indirect Dispatch Mesh",
                    1,
                    sizeof(Renderer::DispatchMeshCommand));
            }

            void Destroy()
            {
                QueueStateGpu.Destroy();
                LegacyVisibleMeshletsGpu.Destroy();
                IndirectDispatchMeshGpu.Destroy();
            }
        };

        struct VsmCullResources
        {
            StructuredBuffer PageFlagsGpu;
            StructuredBuffer RasterWindowPageMasksGpu;
            StructuredBuffer QueueStateGpu;
            StructuredBuffer NodeTasksGpu;
            ByteAddressBuffer MeshletBatchGpu;
            StructuredBuffer CandidateMeshletsGpu;
            StructuredBuffer RasterItemsGpu;
            IndirectArgsBuffer CullDispatchArgsGpu;
            IndirectArgsBuffer RasterDispatchArgsGpu;
            ByteAddressBuffer CountersGpu;

            void Create()
            {
                PageFlagsGpu.Create(
                    L"VSM Page Flags",
                    kMaxShadowViews * kPageFlagsNodesPerView,
                    sizeof(uint32_t));
                RasterWindowPageMasksGpu.Create(
                    L"VSM Raster Window Page Masks",
                    kMaxShadowViews * kRasterWindowsPerView,
                    sizeof(uint32_t));
                QueueStateGpu.Create(L"VSM Multi-View Cull Queue State", 1, sizeof(QueueState));
                NodeTasksGpu.Create(L"VSM Multi-View Node Tasks", MAX_NODES + MAX_BVH_NODES_PER_GROUP, sizeof(VsmNodeTask));
                MeshletBatchGpu.Create(
                    L"VSM Multi-View Meshlet Batches",
                    MAX_CANDIDATE_MESHLETS_BATCH + 1u,
                    sizeof(uint32_t));
                CandidateMeshletsGpu.Create(
                    L"VSM Multi-View Candidate Meshlets",
                    MAX_CANDIDATE_MESHLETS + DAG_CULL_GROUP_SIZE,
                    sizeof(VsmCandidateMeshlet));
                RasterItemsGpu.Create(L"VSM Raster Items", MAX_VISIBLE_MESHLETS, sizeof(VsmRasterItem));
                CullDispatchArgsGpu.Create(L"VSM Cull Dispatch Args", 2, sizeof(D3D12_DISPATCH_ARGUMENTS));
                RasterDispatchArgsGpu.Create(
                    L"VSM Raster Dispatch Args",
                    1,
                    sizeof(Renderer::DispatchMeshCommand));
                CountersGpu.Create(L"VSM Cull Counters", 1, VSM_CULL_COUNTERS_SIZE);
            }

            void Destroy()
            {
                PageFlagsGpu.Destroy();
                RasterWindowPageMasksGpu.Destroy();
                QueueStateGpu.Destroy();
                NodeTasksGpu.Destroy();
                MeshletBatchGpu.Destroy();
                CandidateMeshletsGpu.Destroy();
                RasterItemsGpu.Destroy();
                CullDispatchArgsGpu.Destroy();
                RasterDispatchArgsGpu.Destroy();
                CountersGpu.Destroy();
            }
        };

        VsmPhysicalPageCullResources s_PhysicalPageCullResources;
        VsmCullResources s_CullResources;
        DepthBuffer s_PhysicalPagePool;
        std::array<HierarchicalDepthBuffer, 2> s_PhysicalHZBs;

        std::vector<VsmShadowView> s_Views;
        std::vector<VsmShadowView> s_PreviousViews;
        std::vector<DirectionalVsmClipmapGpu> s_DirectionalClipmapsGpuData;
        std::vector<DirectionalVsmAddressGpu> s_DirectionalAddressesGpuData;
        std::vector<VsmProjectionGpu> s_ProjectionsGpuData;
        std::vector<uint32_t> s_LocalViewIds;
        std::vector<uint32_t> s_DirtyViewIds;

        struct DirectionalVsmLevelDepthState
        {
            float CenterZ = 0.0f;
            float RadiusZ = 0.0f;
            bool Valid = false;
        };

        struct DirectionalResidencySlot
        {
            uint32_t StableShadowMapId = 0;
            uint32_t AddressGeneration = 0;
            uint32_t LastUsedFrame = 0;
            bool Occupied = false;
            std::array<DirectionalVsmLevelDepthState, kMaxDirectionalClipmapLevels> LevelDepthStates;
        };

        std::array<DirectionalResidencySlot, kMaxDirectionalClipmaps> s_DirectionalResidencySlots;
        std::vector<uint32_t> s_ResidencyStatesToInitialize;
        uint32_t s_CurrentPageTableIndex = 0;
        uint32_t s_CommittedPhysicalHZBIndex = 0;
        uint32_t s_FrameNumber = 0;
        bool s_PhysicalPagePoolInitialized = false;

        constexpr uint32_t kPageStatisticsReadbackCount = 3;
        constexpr uint32_t kPageManagementThreadCount = 64;
        constexpr uint32_t kRequestWordGroupCount =
            (kRequestMaskWordCountPerView + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;
        constexpr uint32_t kPhysicalPageGroupCount =
            (kPhysicalPageCapacity + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;
        constexpr float kResidencyTargetPoolLoad = 0.85f;
        constexpr float kResidencyResolutionDownLerpFactor = 0.5f;
        constexpr float kResidencyResolutionUpLerpFactor = 0.1f;
        constexpr float kDirectionalClipmapZRangeScale = 1000.0f;
        constexpr float kDirectionalClipmapZGuardBand = 0.9f;
        constexpr uint32_t kResidencyRecoveryFrameCount = 10;
        constexpr size_t kManagementStatisticsReadbackOffset = 0;
        constexpr size_t kPhysicalPageRenderStatisticsReadbackOffset = VSM_PAGE_MANAGEMENT_COUNTERS_SIZE;
        constexpr size_t kRequestStatisticsReadbackOffset =
            kPhysicalPageRenderStatisticsReadbackOffset + VSM_PHYSICAL_PAGE_RENDER_COUNTERS_SIZE;
        constexpr size_t kCullQueueStateReadbackOffset =
            kRequestStatisticsReadbackOffset + kMaxShadowViews * VSM_REQUEST_STATISTICS_STRIDE;
        constexpr size_t kCullCountersReadbackOffset = kCullQueueStateReadbackOffset + sizeof(QueueState);
        constexpr size_t kPageStatisticsReadbackSize =
            kCullCountersReadbackOffset + VSM_CULL_COUNTERS_SIZE;

        struct PageStatisticsReadbackSlot
        {
            ReadbackBuffer Buffer;
            uint64_t FenceValue = 0;
            uint32_t ViewCount = 0;
            uint32_t RenderBudget = 0;
            uint32_t Generation = 0;
        };

        std::array<PageStatisticsReadbackSlot, kPageStatisticsReadbackCount> s_PageStatisticsReadbacks;
        PageStatistics s_PageStatistics;
        uint32_t s_PageStatisticsGeneration = 0;
        uint32_t s_PageStatisticsRenderBudget = 0;

        uint32_t GetPhysicalPageRenderBudget()
        {
            return static_cast<uint32_t>(s_PhysicalPageRenderBudget);
        }

        uint32_t AcquireDirectionalResidencyState(uint32_t stableShadowMapId)
        {
            for (uint32_t index = 0; index < s_DirectionalResidencySlots.size(); ++index)
            {
                DirectionalResidencySlot& slot = s_DirectionalResidencySlots[index];
                if (slot.Occupied && slot.StableShadowMapId == stableShadowMapId)
                {
                    slot.LastUsedFrame = s_FrameNumber;
                    return index;
                }
            }

            auto slotIt = std::find_if(
                s_DirectionalResidencySlots.begin(),
                s_DirectionalResidencySlots.end(),
                [](const DirectionalResidencySlot& slot) { return !slot.Occupied; });
            if (slotIt == s_DirectionalResidencySlots.end())
            {
                slotIt = std::min_element(
                    s_DirectionalResidencySlots.begin(),
                    s_DirectionalResidencySlots.end(),
                    [](const DirectionalResidencySlot& lhs, const DirectionalResidencySlot& rhs)
                    {
                        return lhs.LastUsedFrame < rhs.LastUsedFrame;
                    });
            }

            ASSERT(slotIt->LastUsedFrame != s_FrameNumber, "Exceeded the directional VSM residency-state limit.");
            const uint32_t slotIndex = static_cast<uint32_t>(slotIt - s_DirectionalResidencySlots.begin());
            *slotIt = {};
            slotIt->StableShadowMapId = stableShadowMapId;
            slotIt->LastUsedFrame = s_FrameNumber;
            slotIt->Occupied = true;
            s_ResidencyStatesToInitialize.push_back(slotIndex);
            return slotIndex;
        }

        bool UpdateDirectionalLevelDepthState(
            DirectionalVsmLevelDepthState& state,
            float desiredCenterZ,
            float levelRadius)
        {
            const float desiredRadiusZ = std::max(levelRadius * kDirectionalClipmapZRangeScale, levelRadius);
            if (!state.Valid)
            {
                state = { desiredCenterZ, desiredRadiusZ, true };
                return false;
            }

            const bool radiusChanged = state.RadiusZ != desiredRadiusZ;
            const bool outsideGuardBand =
                std::fabs(desiredCenterZ - state.CenterZ) + levelRadius > state.RadiusZ * kDirectionalClipmapZGuardBand;
            if (!radiusChanged && !outsideGuardBand)
                return false;

            state = { desiredCenterZ, desiredRadiusZ, true };
            return true;
        }

        void PublishPageStatistics(const PageStatistics& statistics)
        {
            s_PageStatistics = statistics;
            s_RequestedPageCount = static_cast<int32_t>(statistics.RequestedPages);
            s_ReusedPageCount = static_cast<int32_t>(statistics.ReusedPages);
            s_NewPageCount = static_cast<int32_t>(statistics.NewPages);
            s_OverflowPageCount = static_cast<int32_t>(statistics.OverflowPages);
            s_CoarseMappedPageCount = static_cast<int32_t>(statistics.CoarseMappedPages);
            s_CoarseOverflowPageCount = static_cast<int32_t>(statistics.CoarseOverflowPages);
            s_RenderRequestCount = static_cast<int32_t>(statistics.RenderRequests);
            s_RenderDataPageCount = static_cast<int32_t>(statistics.RenderDataPages);
            s_ActiveRenderViewCount = static_cast<int32_t>(statistics.ActiveRenderViews);
            s_RenderMaskPageCount = static_cast<int32_t>(statistics.RenderMaskPages);
            s_InvalidRenderRequestCount = static_cast<int32_t>(statistics.InvalidRenderRequests);
            s_RasterItemCount = static_cast<int32_t>(statistics.RasterItems);
            s_CullOverflowCount = static_cast<int32_t>(statistics.CullOverflow);
            s_RenderBacklogCount = static_cast<int32_t>(statistics.RenderBacklog);
            s_PhysicalPoolExhausted = statistics.PhysicalPoolExhausted;
            s_RenderBudgetExceeded = statistics.RenderBudgetExceeded;
            s_PageCountersValid = statistics.CountersValid;
        }

        PageStatistics DecodePageStatistics(PageStatisticsReadbackSlot& slot)
        {
            const uint32_t* data = static_cast<const uint32_t*>(slot.Buffer.Map());
            const auto readValue = [data](size_t byteOffset)
            {
                return data[byteOffset / sizeof(uint32_t)];
            };

            PageStatistics statistics;
            statistics.Ready = true;
            statistics.RenderBudget = slot.RenderBudget;
            statistics.ReusedPages = readValue(
                kManagementStatisticsReadbackOffset + VSM_REUSED_PAGE_COUNT_OFFSET);
            statistics.NewPages = readValue(
                kManagementStatisticsReadbackOffset + VSM_NEW_PAGE_COUNT_OFFSET);
            statistics.OverflowPages = readValue(
                kManagementStatisticsReadbackOffset + VSM_OVERFLOW_PAGE_COUNT_OFFSET);
            statistics.CoarseMappedPages = readValue(
                kManagementStatisticsReadbackOffset + VSM_COARSE_MAPPED_PAGE_COUNT_OFFSET);
            statistics.CoarseOverflowPages = readValue(
                kManagementStatisticsReadbackOffset + VSM_COARSE_OVERFLOW_PAGE_COUNT_OFFSET);
            statistics.RenderRequests = readValue(
                kManagementStatisticsReadbackOffset + VSM_RENDER_REQUEST_COUNT_OFFSET);
            statistics.FreePagesBeforeAllocation = readValue(
                kManagementStatisticsReadbackOffset + VSM_FREE_PAGE_COUNT_OFFSET);
            statistics.RenderDataPages = readValue(
                kPhysicalPageRenderStatisticsReadbackOffset + VSM_RENDER_PAGE_COUNT_OFFSET);
            statistics.ActiveRenderViews = readValue(
                kPhysicalPageRenderStatisticsReadbackOffset + VSM_ACTIVE_VIEW_COUNT_OFFSET);
            statistics.RenderMaskPages = readValue(
                kPhysicalPageRenderStatisticsReadbackOffset + VSM_RENDER_MASK_PAGE_COUNT_OFFSET);
            statistics.InvalidRenderRequests = readValue(
                kPhysicalPageRenderStatisticsReadbackOffset + VSM_INVALID_RENDER_REQUEST_COUNT_OFFSET);
            constexpr size_t kVisibleMeshletCountOffset = offsetof(QueuePassState, VisibleMeshletCount);
            const uint64_t rasterItemCount =
                readValue(kCullQueueStateReadbackOffset + kVisibleMeshletCountOffset) +
                static_cast<uint64_t>(readValue(
                    kCullQueueStateReadbackOffset + sizeof(QueuePassState) + kVisibleMeshletCountOffset));
            statistics.RasterItems = static_cast<uint32_t>(
                std::min(rasterItemCount, static_cast<uint64_t>(MAX_VISIBLE_MESHLETS)));
            statistics.CullOverflow =
                readValue(kCullCountersReadbackOffset + VSM_CULL_OVERFLOW_COUNT_OFFSET);

            for (uint32_t viewId = 0; viewId < slot.ViewCount; ++viewId)
            {
                statistics.RequestedPages += readValue(
                    kRequestStatisticsReadbackOffset +
                        static_cast<size_t>(viewId) * VSM_REQUEST_STATISTICS_STRIDE +
                        VSM_REQUESTED_PAGE_COUNT_OFFSET);
            }

            slot.Buffer.Unmap();

            const uint32_t mappedPages = statistics.ReusedPages + statistics.NewPages;
            statistics.RenderBacklog = statistics.RenderRequests > statistics.RenderBudget
                ? statistics.RenderRequests - statistics.RenderBudget
                : 0u;
            statistics.PhysicalPoolExhausted = statistics.OverflowPages != 0u;
            statistics.RenderBudgetExceeded = statistics.RenderBacklog != 0u;
            statistics.CountersValid =
                statistics.RequestedPages == mappedPages + statistics.OverflowPages &&
                statistics.ReusedPages + statistics.FreePagesBeforeAllocation == kPhysicalPageCapacity &&
                statistics.NewPages <= statistics.FreePagesBeforeAllocation &&
                (!statistics.PhysicalPoolExhausted ||
                    statistics.NewPages == statistics.FreePagesBeforeAllocation) &&
                statistics.CoarseMappedPages <= mappedPages &&
                statistics.CoarseOverflowPages <= statistics.OverflowPages &&
                statistics.RenderRequests <= mappedPages &&
                statistics.RenderRequests <= kPhysicalPageCapacity &&
                statistics.RenderDataPages + statistics.InvalidRenderRequests == statistics.RenderRequests &&
                statistics.RenderMaskPages == statistics.RenderDataPages &&
                statistics.ActiveRenderViews <= slot.ViewCount;
            return statistics;
        }

        void UpdatePageStatisticsReadback()
        {
            PageStatisticsReadbackSlot& slot =
                s_PageStatisticsReadbacks[s_FrameNumber % kPageStatisticsReadbackCount];

            if (slot.FenceValue != 0)
            {
                if (!Graphics::g_CommandManager.IsFenceComplete(slot.FenceValue))
                    return;

                slot.FenceValue = 0;
                if (slot.Generation == s_PageStatisticsGeneration)
                    PublishPageStatistics(DecodePageStatistics(slot));
            }

            if (s_Views.empty())
                return;

            slot.ViewCount = static_cast<uint32_t>(s_Views.size());
            slot.RenderBudget = s_PageStatisticsRenderBudget;
            slot.Generation = s_PageStatisticsGeneration;

            GraphicsContext& context = GraphicsContext::Begin(L"VSM Page Statistics Readback");
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.TransitionResource(s_PhysicalPageRenderCountersGpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.TransitionResource(s_CullResources.QueueStateGpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.CopyBufferRegion(
                slot.Buffer,
                kManagementStatisticsReadbackOffset,
                s_PageManagementCountersGpu,
                0,
                VSM_PAGE_MANAGEMENT_COUNTERS_SIZE);
            context.CopyBufferRegion(
                slot.Buffer,
                kPhysicalPageRenderStatisticsReadbackOffset,
                s_PhysicalPageRenderCountersGpu,
                0,
                VSM_PHYSICAL_PAGE_RENDER_COUNTERS_SIZE);
            context.CopyBufferRegion(
                slot.Buffer,
                kRequestStatisticsReadbackOffset,
                s_RequestStatisticsGpu,
                0,
                static_cast<size_t>(slot.ViewCount) * VSM_REQUEST_STATISTICS_STRIDE);
            context.CopyBufferRegion(
                slot.Buffer,
                kCullQueueStateReadbackOffset,
                s_CullResources.QueueStateGpu,
                0,
                sizeof(QueueState));
            context.CopyBufferRegion(
                slot.Buffer,
                kCullCountersReadbackOffset,
                s_CullResources.CountersGpu,
                0,
                VSM_CULL_COUNTERS_SIZE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                s_PhysicalPageRenderCountersGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.QueueStateGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            slot.FenceValue = context.Finish();
        }

        uint64_t GetRenderRequestPredicateOffset(uint32_t renderRequestIndex)
        {
            return static_cast<uint64_t>(renderRequestIndex) * VSM_RENDER_REQUEST_PREDICATE_STRIDE;
        }

        void AddPhysicalPagePassRootSRVs(ProgramDesc& desc)
        {
            desc.AddRootBufferSRV("g_VsmPageRenderRequests");
            desc.AddRootBufferSRV("g_VsmPageManagementCounters");
            desc.AddRootBufferSRV("g_VsmPhysicalPageViews");
        }

        void BindPhysicalPagePassResources(ProgramBinder& binder, uint32_t renderRequestIndex)
        {
            binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.SetRootBufferSRV("g_VsmPhysicalPageViews", s_PhysicalPageViewsGpu);
            binder["g_VsmPhysicalPagePass"]["RenderRequestIndex"].Set(renderRequestIndex);
        }

        void SetCommonResources(ProgramBinder& binder, const Renderer::FrameConstants& frame)
        {
            ProgramVar commonResources = binder["g_CommonResources"];
            commonResources["BindlessResourcesBaseIndex"].Set(frame.BindlessResourcesBaseIndex);
            commonResources["FrameIndexMod2"].Set(frame.FrameIndexMod2);
        }

        void SetMarkRequestedPagesConstants(ProgramBinder& binder, const Renderer::ViewConstants& view)
        {
            ProgramVar constants = binder["g_MarkVsmPages"];
            constants["InverseViewProjMatrix"].Set(view.InverseViewProjMatrix);
            constants["ViewportSize"].Set(DirectX::XMUINT2(view.ViewportWidth, view.ViewportHeight));

            ProgramVar commonResources = binder["g_CommonResources"];
            commonResources["BindlessResourcesBaseIndex"].Set(Renderer::GetBindlessResourcesBaseOffset());
            commonResources["FrameIndexMod2"].Set(TemporalEffects::GetFrameIndexMod2());
        }

        StructuredBuffer& GetCurrentPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex];
        }

        StructuredBuffer& GetPreviousPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex ^ 1u];
        }

        HierarchicalDepthBuffer& GetCommittedPhysicalHZB()
        {
            return s_PhysicalHZBs[s_CommittedPhysicalHZBIndex];
        }

        HierarchicalDepthBuffer& GetPendingPhysicalHZB()
        {
            return s_PhysicalHZBs[s_CommittedPhysicalHZBIndex ^ 1u];
        }

        void CommitPendingPhysicalHZB()
        {
            s_CommittedPhysicalHZBIndex ^= 1u;
        }

        Renderer::HZBResources GetPhysicalHZBResources()
        {
            const uint32_t descriptorBaseIndex = Renderer::GetBindlessResourcesBaseOffset();

            Renderer::HZBResources resources;
            resources.Previous = &GetCommittedPhysicalHZB();
            resources.Current = &GetPendingPhysicalHZB();
            resources.PreviousSRVIndex = descriptorBaseIndex + SRV_VSM_PHYSICAL_HZB0 + s_CommittedPhysicalHZBIndex;
            resources.CurrentSRVIndex = descriptorBaseIndex + SRV_VSM_PHYSICAL_HZB0 +
                (s_CommittedPhysicalHZBIndex ^ 1u);
            return resources;
        }

        void BindPageManagementConstants(ProgramBinder& binder)
        {
            binder["g_VsmPageManagement"]["FrameNumber"].Set(s_FrameNumber);
        }

        void BindPageManagementConstants(ProgramBinder& binder, uint32_t allocationClass)
        {
            BindPageManagementConstants(binder);
            binder["g_VsmPageManagement"]["AllocationClass"].Set(allocationClass);
        }

        void InsertPageMappingUAVBarriers(ComputeContext& context, bool includeUsedMask)
        {
            context.InsertUAVBarrier(s_PageTablesGpu[s_CurrentPageTableIndex]);
            context.InsertUAVBarrier(s_PhysicalPageMetadataGpu);
            context.InsertUAVBarrier(s_PageRenderRequestsGpu);
            context.InsertUAVBarrier(s_PageManagementCountersGpu);
            context.InsertUAVBarrier(s_RenderRequestPredicateGpu);

            if (includeUsedMask)
                context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        }

        void DispatchReuseRequestedPages(
            ComputeContext& context,
            StructuredBuffer& currentPageTable,
            StructuredBuffer& previousPageTable)
        {
            ProgramBinder binder(*s_ReuseRequestedPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_ReuseRequestedPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferSRV("g_VsmPreviousPageTable", previousPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmCurrentPageTable", currentPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV", s_PhysicalPageUsedMaskGpu);
            binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.SetRootBufferUAV("g_VsmRenderRequestPredicate", s_RenderRequestPredicateGpu);
            BindPageManagementConstants(binder);
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        void DispatchAllocateNewPages(
            ComputeContext& context,
            StructuredBuffer& currentPageTable,
            uint32_t allocationClass)
        {
            ProgramBinder binder(*s_AllocateNewPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_AllocateNewPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferSRV("g_VsmFreePhysicalPagesSRV", s_FreePhysicalPagesGpu);
            binder.SetRootBufferUAV("g_VsmCurrentPageTable", currentPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.SetRootBufferUAV("g_VsmRenderRequestPredicate", s_RenderRequestPredicateGpu);
            BindPageManagementConstants(binder, allocationClass);
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        void MarkPendingViewsDirty(ComputeContext& context)
        {
            if (s_DirtyViewIds.empty())
                return;

            ProgramBinder binder(*s_MarkViewDirtyProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_MarkViewDirtyPSO);
            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);

            for (uint32_t viewId : s_DirtyViewIds)
            {
                binder["g_MarkVsmViewDirty"]["ViewId"].Set(viewId);
                binder.Apply();
                context.Dispatch1D(kPhysicalPageCapacity);
            }

            context.InsertUAVBarrier(s_PhysicalPageMetadataGpu);
            s_DirtyViewIds.clear();
        }

        void GeneratePendingPhysicalHZB(GraphicsContext& gfxContext)
        {
            ScopedTimer timer(L"VSM: Generate Physical HZB", gfxContext);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GetPendingPhysicalHZB().GenerateHZB(gfxContext, s_PhysicalPagePool);
        }

        void BindPhysicalHZBConstants(ProgramVar constants, const Renderer::HZBResources& hzbResources)
        {
            const float width = static_cast<float>(hzbResources.Current->GetWidth());
            const float height = static_cast<float>(hzbResources.Current->GetHeight());
            constants["HZBSizeAndInv"].Set(DirectX::XMFLOAT4(width, height, 1.0f / width, 1.0f / height));
            constants["PreviousHZB"].Set(SlangDescriptorHandle{ hzbResources.PreviousSRVIndex, 0 });
            constants["CurrentHZB"].Set(SlangDescriptorHandle{ hzbResources.CurrentSRVIndex, 0 });
        }

        void MarkPhysicalPagesRendered(
            GraphicsContext& gfxContext,
            uint32_t firstRenderRequestIndex,
            uint32_t requestCount,
            uint32_t addressType,
            bool requireCullSuccess)
        {
            if (requestCount == 0u)
                return;

            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_MarkPhysicalPageRenderedProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_MarkPhysicalPageRenderedPSO);
            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_VsmPageRenderRequestsSRV", s_PageRenderRequestsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCountersSRV", s_PageManagementCountersGpu);
            binder.SetRootBufferSRV("g_VsmCullCounters", s_CullResources.CountersGpu);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmPhysicalPageViewsUAV", s_PhysicalPageViewsGpu);
            ProgramVar constants = binder["g_MarkPhysicalPageRendered"];
            constants["FirstRenderRequestIndex"].Set(firstRenderRequestIndex);
            constants["RenderRequestCount"].Set(requestCount);
            constants["AddressType"].Set(addressType);
            constants["RequireCullSuccess"].Set(requireCullSuccess ? 1u : 0u);
            binder.Apply();

            context.Dispatch1D(requestCount, 64u);
            context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }

        void MarkPhysicalPageRendered(GraphicsContext& gfxContext, uint32_t renderRequestIndex)
        {
            MarkPhysicalPagesRendered(
                gfxContext,
                renderRequestIndex,
                1u,
                VSM_ADDRESS_TYPE_INVALID,
                false);
        }

        void DispatchPhysicalPageCull(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            const Renderer::HZBResources& hzbResources,
            uint32_t renderRequestIndex,
            uint32_t passIndex)
        {
            const Program& instanceCullProgram = *s_PhysicalPageInstanceCullPrograms[passIndex];
            const ComputePSO& instanceCullPSO = s_PhysicalPageInstanceCullPSOs[passIndex];
            const Program& dagCullProgram = *s_PhysicalPageDAGCullPrograms[passIndex];
            const ComputePSO& dagCullPSO = s_PhysicalPageDAGCullPSOs[passIndex];

            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(
                DrawCommandManager::GetPotentialDrawItemsGPU(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                s_PhysicalPageCullResources.QueueStateGpu,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(GetCommittedPhysicalHZB(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(GetPendingPhysicalHZB(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();

            context.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());
            context.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                Renderer::s_SamplerHeap.GetHeapPointer());

            {
                ProgramBinder binder(instanceCullProgram, context);
                binder.SetRootSignature();
                context.SetPipelineState(instanceCullPSO);

                ProgramVar constants = binder["g_InstanceCull"];
                constants["ViewportWidth"].Set(kPageSize);
                constants["ViewportHeight"].Set(kPageSize);
                constants["MaxCommands"].Set(DrawCommandManager::GetNumPotentialDrawItems());
                BindPhysicalHZBConstants(constants, hzbResources);
                BindPhysicalPagePassResources(binder, renderRequestIndex);
                SetCommonResources(binder, frame);
                binder.SetRootBufferUAV("g_VsmTaskQueueStateUAV", s_PhysicalPageCullResources.QueueStateGpu);
                binder.Apply();

                context.Dispatch1D(DrawCommandManager::GetNumPotentialDrawItems());
            }

            context.TransitionResource(
                s_PhysicalPageCullResources.LegacyVisibleMeshletsGpu,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                DrawCommandManager::GetCandidateMeshletGPU(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                GeometryStreaming::m_GeometryStreamingRequestMaskGPU,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(dagCullProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(dagCullPSO);

            ProgramVar constants = binder["g_DAGCull"];
            constants["PixelErrorThreshold"].Set(Renderer::GetPixelErrorThreshold());
            constants["ViewportWidth"].Set(kPageSize);
            constants["ViewportHeight"].Set(kPageSize);
            BindPhysicalHZBConstants(constants, hzbResources);
            BindPhysicalPagePassResources(binder, renderRequestIndex);
            SetCommonResources(binder, frame);
            binder.SetRootBufferUAV("g_TaskQueueStateUAV", s_PhysicalPageCullResources.QueueStateGpu);
            binder.SetRootBufferUAV("g_TaskQueueUAV", DrawCommandManager::GetTaskQueueGPU());
            binder.SetRootBufferUAV("g_MeshletBatchUAV", DrawCommandManager::GetMeshletBatchGPU());
            binder.SetRootBufferUAV("g_CandidateMeshletUAV", DrawCommandManager::GetCandidateMeshletGPU());
            binder.SetRootBufferUAV(
                "g_VsmVisibleMeshletUAV",
                s_PhysicalPageCullResources.LegacyVisibleMeshletsGpu);
            binder.Apply();

            constexpr uint32_t kDAGCullGroupSize = DAG_CULL_GROUP_SIZE;
            context.Dispatch1D(Renderer::GetDAGCullGroupCount() * kDAGCullGroupSize, kDAGCullGroupSize);
        }

        void BuildPhysicalPageDrawCommand(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            uint32_t passIndex)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(
                s_PhysicalPageCullResources.QueueStateGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                s_PhysicalPageCullResources.IndirectDispatchMeshGpu,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_PhysicalPageMeshBufferGenProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_PhysicalPageMeshBufferGenPSO);
            binder["g_MeshBufferGen"]["PassIndex"].Set(passIndex);
            SetCommonResources(binder, frame);
            binder.SetRootBufferSRV("g_VsmTaskQueueStateSRV", s_PhysicalPageCullResources.QueueStateGpu);
            binder.SetRootBufferUAV(
                "g_VsmIndirectDispatchMeshUAV",
                s_PhysicalPageCullResources.IndirectDispatchMeshGpu);
            binder.Apply();
            context.Dispatch1D(1, 1);
        }

        void DrawPhysicalPageDepth(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            uint32_t renderRequestIndex,
            uint32_t passIndex)
        {
            constexpr D3D12_RESOURCE_STATES kGraphicsShaderResourceState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            gfxContext.TransitionResource(
                s_PhysicalPageCullResources.IndirectDispatchMeshGpu,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(
                s_PhysicalPageCullResources.LegacyVisibleMeshletsGpu,
                kGraphicsShaderResourceState);
            if (passIndex == 1u)
            {
                gfxContext.TransitionResource(
                    s_PhysicalPageCullResources.QueueStateGpu,
                    kGraphicsShaderResourceState);
            }
            gfxContext.TransitionResource(s_PageRenderRequestsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PageManagementCountersGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPageViewsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.FlushResourceBarriers();

            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                Renderer::s_SamplerHeap.GetHeapPointer());
            gfxContext.SetDepthStencilTarget(s_PhysicalPagePool.GetDSV());
            gfxContext.SetViewportAndScissor(0, 0, kPhysicalPoolResolution, kPhysicalPoolResolution);
            gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ProgramBinder binder(*s_PhysicalPageDepthPrograms[passIndex], gfxContext);
            binder.SetRootSignature();
            gfxContext.SetPipelineState(s_PhysicalPageDepthPSOs[passIndex]);
            binder["g_VBufferMesh"]["ViewportWidth"].Set(kPageSize);
            binder["g_VBufferMesh"]["ViewportHeight"].Set(kPageSize);
            BindPhysicalPagePassResources(binder, renderRequestIndex);
            SetCommonResources(binder, frame);
            if (passIndex == 1u)
                binder.SetRootBufferSRV("g_VsmTaskQueueStateSRV", s_PhysicalPageCullResources.QueueStateGpu);
            binder.SetRootBufferSRV("g_VsmVisibleMeshlets", s_PhysicalPageCullResources.LegacyVisibleMeshletsGpu);
            binder.Apply();

            gfxContext.ExecuteIndirect(
                Renderer::GPUDrivenDrawIndirectCommandSignature,
                s_PhysicalPageCullResources.IndirectDispatchMeshGpu,
                0,
                1);
        }

        uint32_t FindPreviousPageTableBase(uint32_t stableShadowMapId, uint32_t addressType, uint32_t layer)
        {
            const auto previousView = std::find_if(
                s_PreviousViews.begin(),
                s_PreviousViews.end(),
                [=](const VsmShadowView& view)
                {
                    return view.StableShadowMapId == stableShadowMapId && view.AddressType == addressType &&
                        view.Layer == layer;
                });

            return previousView != s_PreviousViews.end() ? previousView->PageTableBase : kInvalidPageTableBase;
        }

        DirectX::XMFLOAT4 PackFloat4(Math::Vector3 value, float w)
        {
            return DirectX::XMFLOAT4(
                static_cast<float>(value.GetX()),
                static_cast<float>(value.GetY()),
                static_cast<float>(value.GetZ()),
                w);
        }

        DirectX::XMFLOAT4 PackFloat4(Math::Vector4 value)
        {
            return DirectX::XMFLOAT4(
                static_cast<float>(value.GetX()),
                static_cast<float>(value.GetY()),
                static_cast<float>(value.GetZ()),
                static_cast<float>(value.GetW()));
        }

        int32_t CheckedInt32(int64_t value)
        {
            ASSERT(value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max(),
                   "VSM address exceeds the signed 32-bit shader address range.");

            return static_cast<int32_t>(std::clamp(value, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                                                   static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
        }

        int32_t FloorToInt32(float value)
        {
            ASSERT(std::isfinite(value), "VSM address must be finite.");
            if (!std::isfinite(value))
                return 0;

            const double flooredValue = std::floor(static_cast<double>(value));
            const double clampedValue = std::clamp(flooredValue, static_cast<double>(std::numeric_limits<int32_t>::min()),
                                                   static_cast<double>(std::numeric_limits<int32_t>::max()));

            ASSERT(flooredValue == clampedValue, "VSM address exceeds the signed 32-bit shader address range.");

            return static_cast<int32_t>(clampedValue);
        }

        DirectionalVsmAddressGpu BuildDirectionalVsmAddressGpu(
            const DirectionalVsmAddressConstants& addressConstants)
        {
            // Math matrices use DirectXMath's transposed storage while Slang consumes
            // explicit mathematical rows for the dot products below.
            const Math::Matrix3 worldToLightRows = Math::Transpose(addressConstants.WorldToLightRotation);

            DirectionalVsmAddressGpu addressData{};
            addressData.WorldToLightRow0 = PackFloat4(worldToLightRows.GetX(), 0.0f);
            addressData.WorldToLightRow1 = PackFloat4(worldToLightRows.GetY(), 0.0f);
            addressData.WorldToLightRow2 = PackFloat4(worldToLightRows.GetZ(), 0.0f);
            addressData.AddressOriginAndInvWorldUnitsPerPage =
                PackFloat4(addressConstants.AddressOriginWS, addressConstants.InvWorldUnitsPerPage);
            addressData.AddressAndWindowOriginPage = DirectX::XMINT4(
                addressConstants.AddressOriginPage.x,
                addressConstants.AddressOriginPage.y,
                addressConstants.WindowOriginPage.x,
                addressConstants.WindowOriginPage.y);
            return addressData;
        }

        DirectX::XMFLOAT4 BuildDirectionalVirtualProjectionRow(
            const DirectX::XMFLOAT4& worldToLightRow,
            const DirectX::XMFLOAT4& addressOriginAndInvWorldUnitsPerPage,
            float clipOrigin)
        {
            const float scale = 2.0f * addressOriginAndInvWorldUnitsPerPage.w / static_cast<float>(kPageTableDim);
            const DirectX::XMFLOAT4 row(
                worldToLightRow.x * scale,
                worldToLightRow.y * scale,
                worldToLightRow.z * scale,
                0.0f);
            const float translation =
                -(row.x * addressOriginAndInvWorldUnitsPerPage.x +
                  row.y * addressOriginAndInvWorldUnitsPerPage.y +
                  row.z * addressOriginAndInvWorldUnitsPerPage.z) +
                clipOrigin;
            return DirectX::XMFLOAT4(row.x, row.y, row.z, translation);
        }

        VsmProjectionGpu BuildVsmProjectionGpu(
            const Math::Matrix4& viewProjMatrix,
            Math::Vector3 viewerPosition,
            uint32_t projectionType)
        {
            const Math::Matrix4 viewProjRows = Math::Transpose(viewProjMatrix);

            VsmProjectionGpu projectionData{};
            projectionData.ViewProjRow0 = PackFloat4(viewProjRows.GetX());
            projectionData.ViewProjRow1 = PackFloat4(viewProjRows.GetY());
            projectionData.ViewProjRow2 = PackFloat4(viewProjRows.GetZ());
            projectionData.ViewProjRow3 = PackFloat4(viewProjRows.GetW());
            projectionData.ViewerPositionAndProjectionType =
                PackFloat4(viewerPosition, static_cast<float>(projectionType));
            return projectionData;
        }

        VsmProjectionGpu BuildDirectionalVsmProjectionGpu(
            const DirectionalVsmAddressDesc& desc,
            const DirectionalVsmAddressGpu& addressData,
            const DirectionalVsmLevelDepthState& depthState)
        {
            ASSERT(depthState.Valid && depthState.RadiusZ > 0.0f, "Directional VSM depth range must be valid.");

            VsmProjectionGpu projectionData{};
            projectionData.ViewProjRow0 = BuildDirectionalVirtualProjectionRow(
                addressData.WorldToLightRow0,
                addressData.AddressOriginAndInvWorldUnitsPerPage,
                -1.0f);
            projectionData.ViewProjRow1 = BuildDirectionalVirtualProjectionRow(
                addressData.WorldToLightRow1,
                addressData.AddressOriginAndInvWorldUnitsPerPage,
                1.0f);

            const float invDepthRange = 0.5f / depthState.RadiusZ;
            projectionData.ViewProjRow2 = DirectX::XMFLOAT4(
                addressData.WorldToLightRow2.x * invDepthRange,
                addressData.WorldToLightRow2.y * invDepthRange,
                addressData.WorldToLightRow2.z * invDepthRange,
                0.5f - depthState.CenterZ * invDepthRange);
            projectionData.ViewProjRow3 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            projectionData.ViewerPositionAndProjectionType =
                PackFloat4(desc.FocusPositionWS, static_cast<float>(VSM_PROJECTION_TYPE_ORTHOGRAPHIC));
            return projectionData;
        }

        float ComputePerspectiveVsmLodScale(const Math::Matrix4& projMatrix)
        {
            const float scaleX = std::fabs(static_cast<float>(projMatrix.GetX().GetX()));
            const float scaleY = std::fabs(static_cast<float>(projMatrix.GetY().GetY()));
            if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f)
                return 0.0f;

            const float virtualResolution = static_cast<float>(kVirtualResolution);
            return std::min(
                2.0f / (scaleX * virtualResolution),
                2.0f / (scaleY * virtualResolution));
        }

        void UploadFrameData(ComputeContext& context)
        {
            context.WriteBuffer(
                s_ShadowViewsGpu,
                0,
                s_Views.data(),
                s_Views.size() * sizeof(VsmShadowView));

            if (!s_DirectionalClipmapsGpuData.empty())
            {
                context.WriteBuffer(
                    s_DirectionalClipmapsGpu,
                    0,
                    s_DirectionalClipmapsGpuData.data(),
                    s_DirectionalClipmapsGpuData.size() * sizeof(DirectionalVsmClipmapGpu));
            }

            if (!s_DirectionalAddressesGpuData.empty())
            {
                context.WriteBuffer(
                    s_DirectionalAddressesGpu,
                    0,
                    s_DirectionalAddressesGpuData.data(),
                    s_DirectionalAddressesGpuData.size() * sizeof(DirectionalVsmAddressGpu));
            }

            if (!s_ProjectionsGpuData.empty())
            {
                context.WriteBuffer(
                    s_ProjectionsGpu,
                    0,
                    s_ProjectionsGpuData.data(),
                    s_ProjectionsGpuData.size() * sizeof(VsmProjectionGpu));
            }

            for (uint32_t stateIndex : s_ResidencyStatesToInitialize)
            {
                const VsmResidencyStateGpu initialState{};
                context.WriteBuffer(
                    s_ResidencyStatesGpu,
                    static_cast<size_t>(stateIndex) * sizeof(VsmResidencyStateGpu),
                    &initialState,
                    sizeof(initialState));
            }
            s_ResidencyStatesToInitialize.clear();

            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalClipmapsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        void CommitResidencyStates(ComputeContext& context)
        {
            const uint32_t clipmapCount = static_cast<uint32_t>(s_DirectionalClipmapsGpuData.size());
            if (clipmapCount == 0u)
                return;

            context.TransitionResource(s_DirectionalClipmapsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_CommitResidencyStatesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_CommitResidencyStatesPSO);
            binder.SetRootBufferSRV("g_DirectionalVsmClipmaps", s_DirectionalClipmapsGpu);
            binder.SetRootBufferUAV("g_VsmResidencyStates", s_ResidencyStatesGpu);
            binder["g_VsmResidency"]["ClipmapCount"].Set(clipmapCount);
            binder.Apply();

            context.Dispatch1D(clipmapCount);
            context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }

        void UpdateResidencyStates(ComputeContext& context)
        {
            const uint32_t clipmapCount = static_cast<uint32_t>(s_DirectionalClipmapsGpuData.size());
            if (clipmapCount == 0u)
                return;

            context.TransitionResource(s_DirectionalClipmapsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_UpdateResidencyStatesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_UpdateResidencyStatesPSO);
            binder.SetRootBufferSRV("g_DirectionalVsmClipmaps", s_DirectionalClipmapsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.SetRootBufferUAV("g_VsmResidencyStates", s_ResidencyStatesGpu);

            ProgramVar constants = binder["g_VsmResidency"];
            constants["ClipmapCount"].Set(clipmapCount);
            constants["PhysicalPageCapacity"].Set(kPhysicalPageCapacity);
            constants["TargetPoolLoad"].Set(kResidencyTargetPoolLoad);
            constants["ResolutionDownLerpFactor"].Set(kResidencyResolutionDownLerpFactor);
            constants["ResolutionUpLerpFactor"].Set(kResidencyResolutionUpLerpFactor);
            constants["RecoveryFrameCount"].Set(kResidencyRecoveryFrameCount);
            binder.Apply();

            context.Dispatch1D(clipmapCount);
            context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }
    } // namespace

    DirectionalVsmAddressConstants BuildDirectionalVsmAddressConstants(const DirectionalVsmAddressDesc& desc)
    {
        DirectionalVsmAddressConstants constants;
        constants.WorldToLightRotation = desc.WorldToLightRotation;
        constants.LightIndex = desc.LightIndex;
        constants.ClipmapLevel = desc.ClipmapLevel;
        constants.AddressGeneration = desc.AddressGeneration;

        const float worldUnitsPerPage = desc.LevelWorldExtent / static_cast<float>(kPageTableDim);
        ASSERT(std::isfinite(worldUnitsPerPage) && worldUnitsPerPage > 0.0f, "VSM world-units-per-page must be positive and finite.");
        if (!std::isfinite(worldUnitsPerPage) || worldUnitsPerPage <= 0.0f)
            return constants;

        constants.InvWorldUnitsPerPage = 1.0f / worldUnitsPerPage;

        const Math::Vector3 focusPositionLS = desc.WorldToLightRotation * desc.FocusPositionWS;
        const float focusPageX = static_cast<float>(focusPositionLS.GetX()) * constants.InvWorldUnitsPerPage;
        const float focusPageY = -static_cast<float>(focusPositionLS.GetY()) * constants.InvWorldUnitsPerPage;

        constants.AddressOriginPage = DirectX::XMINT2(FloorToInt32(focusPageX), FloorToInt32(focusPageY));
        constants.WindowOriginPage =
            DirectX::XMINT2(CheckedInt32(static_cast<int64_t>(constants.AddressOriginPage.x) - static_cast<int64_t>(kPageTableDim / 2u)),
                            CheckedInt32(static_cast<int64_t>(constants.AddressOriginPage.y) - static_cast<int64_t>(kPageTableDim / 2u)));

        // AddressOriginWS is the world-space point whose light-plane XY lies
        // exactly on the AddressOriginPage boundary. Keeping the focus Z makes
        // the relative world position small along the light direction too.
        const Math::Vector3 addressOriginLS(static_cast<float>(constants.AddressOriginPage.x) * worldUnitsPerPage,
                                            -static_cast<float>(constants.AddressOriginPage.y) * worldUnitsPerPage,
                                            static_cast<float>(focusPositionLS.GetZ()));
        constants.AddressOriginWS = Math::Transpose(desc.WorldToLightRotation) * addressOriginLS;

        return constants;
    }

    bool Initialize()
    {
        if (s_Initialized)
            return true;

        ProgramDesc markDirectionalPagesDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MarkVsmPages.slang"),
            "markDirectionalMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        markDirectionalPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        markDirectionalPagesDesc.AddRootBufferSRV("g_DirectionalVsmClipmaps");
        markDirectionalPagesDesc.AddRootBufferSRV("g_VsmResidencyStates");
        markDirectionalPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        markDirectionalPagesDesc.AddRootBufferUAV("g_VsmPageRequestMask");
        markDirectionalPagesDesc.AddRootBufferUAV("g_VsmRequestStatistics");
        s_MarkDirectionalPagesProgram = ProgramUtils::GetProgram(
            markDirectionalPagesDesc,
            "VSM: Mark Directional Pages");
        if (!s_MarkDirectionalPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkDirectionalPagesPSO, *s_MarkDirectionalPagesProgram);
        s_MarkDirectionalPagesPSO.Finalize();

        ProgramDesc markLocalPagesDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MarkVsmPages.slang"),
            "markLocalMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        markLocalPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        markLocalPagesDesc.AddRootBufferSRV("g_VsmProjections");
        markLocalPagesDesc.AddRootBufferUAV("g_VsmPageRequestMask");
        markLocalPagesDesc.AddRootBufferUAV("g_VsmRequestStatistics");
        s_MarkLocalPagesProgram = ProgramUtils::GetProgram(
            markLocalPagesDesc,
            "VSM: Mark Local Pages");
        if (!s_MarkLocalPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkLocalPagesPSO, *s_MarkLocalPagesProgram);
        s_MarkLocalPagesPSO.Finalize();

        const std::string residencyShaderPath = Renderer::GetModelShaderPath("VsmResidency.slang");

        ProgramDesc commitResidencyStatesDesc =
            ProgramUtils::MakeComputeDesc(residencyShaderPath, "commitResidencyStates");
        commitResidencyStatesDesc.AddRootBufferSRV("g_DirectionalVsmClipmaps");
        commitResidencyStatesDesc.AddRootBufferUAV("g_VsmResidencyStates");
        s_CommitResidencyStatesProgram =
            ProgramUtils::GetProgram(commitResidencyStatesDesc, "VSM: Commit Residency States");
        if (!s_CommitResidencyStatesProgram)
            return false;

        ProgramUtils::SetProgram(s_CommitResidencyStatesPSO, *s_CommitResidencyStatesProgram);
        s_CommitResidencyStatesPSO.Finalize();

        ProgramDesc updateResidencyStatesDesc =
            ProgramUtils::MakeComputeDesc(residencyShaderPath, "updateResidencyStates");
        updateResidencyStatesDesc.AddRootBufferSRV("g_DirectionalVsmClipmaps");
        updateResidencyStatesDesc.AddRootBufferSRV("g_VsmPageManagementCounters");
        updateResidencyStatesDesc.AddRootBufferUAV("g_VsmResidencyStates");
        s_UpdateResidencyStatesProgram =
            ProgramUtils::GetProgram(updateResidencyStatesDesc, "VSM: Update Residency States");
        if (!s_UpdateResidencyStatesProgram)
            return false;

        ProgramUtils::SetProgram(s_UpdateResidencyStatesPSO, *s_UpdateResidencyStatesProgram);
        s_UpdateResidencyStatesPSO.Finalize();

        const std::string pageManagementShaderPath = Renderer::GetModelShaderPath("VsmPageManagement.slang");

        ProgramDesc markViewDirtyDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "markViewDirty");
        markViewDirtyDesc.AddRootBufferSRV("g_VsmShadowViews");
        markViewDirtyDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        markViewDirtyDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        s_MarkViewDirtyProgram = ProgramUtils::GetProgram(markViewDirtyDesc, "VSM: Mark View Dirty");
        if (!s_MarkViewDirtyProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkViewDirtyPSO, *s_MarkViewDirtyProgram);
        s_MarkViewDirtyPSO.Finalize();

        ProgramDesc reuseRequestedPagesDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "reuseRequestedPages");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPreviousPageTable");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmCurrentPageTable");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmRenderRequestPredicate");
        s_ReuseRequestedPagesProgram = ProgramUtils::GetProgram(reuseRequestedPagesDesc, "VSM: Reuse Requested Pages");
        if (!s_ReuseRequestedPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_ReuseRequestedPagesPSO, *s_ReuseRequestedPagesProgram);
        s_ReuseRequestedPagesPSO.Finalize();

        ProgramDesc buildFreePhysicalPageListDesc =
            ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "buildFreePhysicalPageList");
        buildFreePhysicalPageListDesc.AddRootBufferSRV("g_VsmPhysicalPageUsedMaskSRV");
        buildFreePhysicalPageListDesc.AddRootBufferUAV("g_VsmFreePhysicalPagesUAV");
        buildFreePhysicalPageListDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        s_BuildFreePhysicalPageListProgram =
            ProgramUtils::GetProgram(buildFreePhysicalPageListDesc, "VSM: Build Free Physical Page List");
        if (!s_BuildFreePhysicalPageListProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildFreePhysicalPageListPSO, *s_BuildFreePhysicalPageListProgram);
        s_BuildFreePhysicalPageListPSO.Finalize();

        ProgramDesc allocateNewPagesDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "allocateNewPages");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        allocateNewPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmFreePhysicalPagesSRV");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmCurrentPageTable");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmRenderRequestPredicate");
        s_AllocateNewPagesProgram = ProgramUtils::GetProgram(allocateNewPagesDesc, "VSM: Allocate New Pages");
        if (!s_AllocateNewPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_AllocateNewPagesPSO, *s_AllocateNewPagesProgram);
        s_AllocateNewPagesPSO.Finalize();

        ProgramDesc markPhysicalPageRenderedDesc =
            ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "markPhysicalPageRendered");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmShadowViews");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmPageRenderRequestsSRV");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmPageManagementCountersSRV");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmCullCounters");
        markPhysicalPageRenderedDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        markPhysicalPageRenderedDesc.AddRootBufferUAV("g_VsmPhysicalPageViewsUAV");
        s_MarkPhysicalPageRenderedProgram =
            ProgramUtils::GetProgram(markPhysicalPageRenderedDesc, "VSM: Mark Physical Page Rendered");
        if (!s_MarkPhysicalPageRenderedProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkPhysicalPageRenderedPSO, *s_MarkPhysicalPageRenderedProgram);
        s_MarkPhysicalPageRenderedPSO.Finalize();

        ProgramDesc buildPhysicalPageRenderDataDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("BuildVsmPhysicalPageRenderData.slang"),
            "computeMain");
        buildPhysicalPageRenderDataDesc.AddRootBufferSRV("g_VsmShadowViews");
        buildPhysicalPageRenderDataDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        buildPhysicalPageRenderDataDesc.AddRootBufferSRV("g_VsmProjections");
        buildPhysicalPageRenderDataDesc.AddRootBufferSRV("g_VsmPageRenderRequests");
        buildPhysicalPageRenderDataDesc.AddRootBufferSRV("g_VsmPageManagementCounters");
        buildPhysicalPageRenderDataDesc.AddRootBufferUAV("g_VsmPhysicalPageViews");
        buildPhysicalPageRenderDataDesc.AddRootBufferUAV("g_VsmPageRenderMask");
        buildPhysicalPageRenderDataDesc.AddRootBufferUAV("g_VsmActiveViewMask");
        buildPhysicalPageRenderDataDesc.AddRootBufferUAV("g_VsmActiveViews");
        buildPhysicalPageRenderDataDesc.AddRootBufferUAV("g_VsmPhysicalPageRenderCounters");
        s_BuildPhysicalPageRenderDataProgram = ProgramUtils::GetProgram(
            buildPhysicalPageRenderDataDesc,
            "VSM: Build Physical Page Render Data");
        if (!s_BuildPhysicalPageRenderDataProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildPhysicalPageRenderDataPSO, *s_BuildPhysicalPageRenderDataProgram);
        s_BuildPhysicalPageRenderDataPSO.Finalize();

        const std::string pageFlagsShaderPath = Renderer::GetModelShaderPath("BuildVsmPageFlags.slang");

        ProgramDesc buildPageFlagsMip0Desc = ProgramUtils::MakeComputeDesc(pageFlagsShaderPath, "buildMip0");
        buildPageFlagsMip0Desc.AddRootBufferSRV("g_VsmCullShadowViews");
        buildPageFlagsMip0Desc.AddRootBufferSRV("g_VsmCullDirectionalAddresses");
        buildPageFlagsMip0Desc.AddRootBufferSRV("g_VsmActiveViews");
        buildPageFlagsMip0Desc.AddRootBufferSRV("g_VsmPhysicalPageRenderCounters");
        buildPageFlagsMip0Desc.AddRootBufferSRV("g_VsmPageRenderMask");
        buildPageFlagsMip0Desc.AddRootBufferUAV("g_VsmPageFlagsUAV");
        s_BuildPageFlagsMip0Program = ProgramUtils::GetProgram(buildPageFlagsMip0Desc, "VSM: Build Page Flags Mip 0");
        if (!s_BuildPageFlagsMip0Program)
            return false;

        ProgramUtils::SetProgram(s_BuildPageFlagsMip0PSO, *s_BuildPageFlagsMip0Program);
        s_BuildPageFlagsMip0PSO.Finalize();

        ProgramDesc buildPageFlagsMipDesc = ProgramUtils::MakeComputeDesc(pageFlagsShaderPath, "buildMip");
        buildPageFlagsMipDesc.AddRootBufferSRV("g_VsmCullShadowViews");
        buildPageFlagsMipDesc.AddRootBufferSRV("g_VsmActiveViews");
        buildPageFlagsMipDesc.AddRootBufferSRV("g_VsmPhysicalPageRenderCounters");
        buildPageFlagsMipDesc.AddRootBufferUAV("g_VsmPageFlagsUAV");
        s_BuildPageFlagsMipProgram = ProgramUtils::GetProgram(buildPageFlagsMipDesc, "VSM: Build Page Flags Mip");
        if (!s_BuildPageFlagsMipProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildPageFlagsMipPSO, *s_BuildPageFlagsMipProgram);
        s_BuildPageFlagsMipPSO.Finalize();

        ProgramDesc buildRasterWindowMasksDesc =
            ProgramUtils::MakeComputeDesc(pageFlagsShaderPath, "buildRasterWindowMasks");
        buildRasterWindowMasksDesc.AddRootBufferSRV("g_VsmCullShadowViews");
        buildRasterWindowMasksDesc.AddRootBufferSRV("g_VsmActiveViews");
        buildRasterWindowMasksDesc.AddRootBufferSRV("g_VsmPhysicalPageRenderCounters");
        buildRasterWindowMasksDesc.AddRootBufferUAV("g_VsmPageFlagsUAV");
        buildRasterWindowMasksDesc.AddRootBufferUAV("g_VsmRasterWindowPageMasksUAV");
        s_BuildRasterWindowMasksProgram =
            ProgramUtils::GetProgram(buildRasterWindowMasksDesc, "VSM: Build Raster Window Masks");
        if (!s_BuildRasterWindowMasksProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildRasterWindowMasksPSO, *s_BuildRasterWindowMasksProgram);
        s_BuildRasterWindowMasksPSO.Finalize();

        ProgramDesc buildCullDispatchArgsDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("BuildVsmCullDispatchArgs.slang"),
            "computeMain");
        buildCullDispatchArgsDesc.AddRootBufferSRV("g_VsmPhysicalPageRenderCounters");
        buildCullDispatchArgsDesc.AddRootBufferUAV("g_VsmCullDispatchArgs");
        s_BuildCullDispatchArgsProgram =
            ProgramUtils::GetProgram(buildCullDispatchArgsDesc, "VSM: Build Cull Dispatch Args");
        if (!s_BuildCullDispatchArgsProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildCullDispatchArgsPSO, *s_BuildCullDispatchArgsProgram);
        s_BuildCullDispatchArgsPSO.Finalize();

        ProgramDesc buildRasterDispatchArgsDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("BuildVsmRasterDispatchArgs.slang"),
            "computeMain");
        buildRasterDispatchArgsDesc.AddRootBufferSRV("g_VsmCullQueueState");
        buildRasterDispatchArgsDesc.AddRootBufferUAV("g_VsmRasterDispatchArgs");
        s_BuildRasterDispatchArgsProgram =
            ProgramUtils::GetProgram(buildRasterDispatchArgsDesc, "VSM: Build Raster Dispatch Args");
        if (!s_BuildRasterDispatchArgsProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildRasterDispatchArgsPSO, *s_BuildRasterDispatchArgsProgram);
        s_BuildRasterDispatchArgsPSO.Finalize();

        ProgramDesc multiViewInstanceCullDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("VsmInstanceCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        multiViewInstanceCullDesc.AddRootBufferSRV("g_VsmCullShadowViews");
        multiViewInstanceCullDesc.AddRootBufferSRV("g_VsmCullDirectionalAddresses");
        multiViewInstanceCullDesc.AddRootBufferSRV("g_VsmCullProjections");
        multiViewInstanceCullDesc.AddRootBufferSRV("g_VsmPageFlags");
        multiViewInstanceCullDesc.AddRootBufferSRV("g_VsmActiveViews");
        multiViewInstanceCullDesc.AddRootBufferUAV("g_VsmCullQueueState");
        multiViewInstanceCullDesc.AddRootBufferUAV("g_VsmNodeTasks");
        multiViewInstanceCullDesc.AddRootBufferUAV("g_VsmCullCounters");
        s_MultiViewInstanceCullProgram =
            ProgramUtils::GetProgram(multiViewInstanceCullDesc, "VSM: Multi-View Instance Cull");
        if (!s_MultiViewInstanceCullProgram)
            return false;

        ProgramUtils::SetProgram(s_MultiViewInstanceCullPSO, *s_MultiViewInstanceCullProgram);
        s_MultiViewInstanceCullPSO.Finalize();

        ProgramDesc multiViewDAGCullDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("VsmDAGCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        multiViewDAGCullDesc.AddDefine("DAG_CULL_PASS_INDEX", "0");
        multiViewDAGCullDesc.AddDefine("DISABLE_HZB_CULL");
        multiViewDAGCullDesc.AddRootBufferSRV("g_VsmCullShadowViews");
        multiViewDAGCullDesc.AddRootBufferSRV("g_VsmCullDirectionalAddresses");
        multiViewDAGCullDesc.AddRootBufferSRV("g_VsmCullProjections");
        multiViewDAGCullDesc.AddRootBufferSRV("g_VsmPageFlags");
        multiViewDAGCullDesc.AddRootBufferSRV("g_VsmRasterWindowPageMasks");
        multiViewDAGCullDesc.AddRootBufferUAV("g_TaskQueueStateUAV");
        multiViewDAGCullDesc.AddRootBufferUAV("g_VsmNodeTasksUAV");
        multiViewDAGCullDesc.AddRootBufferUAV("g_MeshletBatchUAV");
        multiViewDAGCullDesc.AddRootBufferUAV("g_VsmCandidateMeshletsUAV");
        multiViewDAGCullDesc.AddRootBufferUAV("g_VsmRasterItemsUAV");
        multiViewDAGCullDesc.AddRootBufferUAV("g_VsmCullCounters");
        s_MultiViewDAGCullProgram = ProgramUtils::GetProgram(multiViewDAGCullDesc, "VSM: Multi-View DAG Cull");
        if (!s_MultiViewDAGCullProgram)
            return false;

        ProgramUtils::SetProgram(s_MultiViewDAGCullPSO, *s_MultiViewDAGCullProgram);
        s_MultiViewDAGCullPSO.Finalize();

        ProgramDesc directionalRasterDepthDesc = ProgramUtils::MakeGraphicsDesc(
            Renderer::GetModelShaderPath("VsmRaster.slang"),
            "",
            "pixelMain",
            "meshMain",
            ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
        directionalRasterDepthDesc.SetGenerateDebugInfo(false);
        directionalRasterDepthDesc.AddEntryPoint(ShaderStage::Amplification, "amplificationMain");
        directionalRasterDepthDesc.AddRootBufferSRV("g_VsmCullQueueState");
        directionalRasterDepthDesc.AddRootBufferSRV("g_VsmRasterItems");
        directionalRasterDepthDesc.AddRootBufferSRV("g_VsmShadowViews");
        directionalRasterDepthDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        directionalRasterDepthDesc.AddRootBufferSRV("g_VsmPageTable");
        directionalRasterDepthDesc.AddRootBufferSRV("g_VsmPhysicalPageViews");
        s_DirectionalRasterDepthProgram =
            ProgramUtils::GetProgram(directionalRasterDepthDesc, "VSM: Directional Raster Depth");
        if (!s_DirectionalRasterDepthProgram)
            return false;

        s_DirectionalRasterDepthPSO.SetRasterizerState(RasterizerShadowTwoSided);
        s_DirectionalRasterDepthPSO.SetDepthStencilState(DepthStateReadWrite);
        s_DirectionalRasterDepthPSO.SetBlendState(BlendDisable);
        s_DirectionalRasterDepthPSO.SetRenderTargetFormats(0, {}, DXGI_FORMAT_D32_FLOAT);
        ProgramUtils::SetProgram(s_DirectionalRasterDepthPSO, *s_DirectionalRasterDepthProgram);
        s_DirectionalRasterDepthPSO.Finalize();

        ProgramDesc clearRequestedPhysicalPageDesc = ProgramUtils::MakeGraphicsDesc(
            Renderer::GetModelShaderPath("ClearVsmPhysicalPage.slang"),
            "vertexMain",
            "");
        clearRequestedPhysicalPageDesc.AddRootBufferSRV("g_VsmShadowViews");
        clearRequestedPhysicalPageDesc.AddRootBufferSRV("g_VsmPageRenderRequests");
        clearRequestedPhysicalPageDesc.AddRootBufferSRV("g_VsmPageManagementCounters");
        s_ClearRequestedPhysicalPageProgram =
            ProgramUtils::GetProgram(clearRequestedPhysicalPageDesc, "VSM: Clear Requested Physical Page");
        if (!s_ClearRequestedPhysicalPageProgram)
            return false;

        D3D12_DEPTH_STENCIL_DESC clearDepthState = DepthStateReadWrite;
        clearDepthState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        s_ClearRequestedPhysicalPagePSO.SetRasterizerState(RasterizerTwoSided);
        s_ClearRequestedPhysicalPagePSO.SetDepthStencilState(clearDepthState);
        s_ClearRequestedPhysicalPagePSO.SetBlendState(BlendDisable);
        s_ClearRequestedPhysicalPagePSO.SetInputLayout(0, nullptr);
        s_ClearRequestedPhysicalPagePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        s_ClearRequestedPhysicalPagePSO.SetDepthTargetFormat(DXGI_FORMAT_D32_FLOAT);
        ProgramUtils::SetProgram(s_ClearRequestedPhysicalPagePSO, *s_ClearRequestedPhysicalPageProgram);
        s_ClearRequestedPhysicalPagePSO.Finalize();

        SamplerDesc physicalPageHZBSampler;
        physicalPageHZBSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        physicalPageHZBSampler.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            const std::string passIndexString = std::to_string(passIndex);
            const std::string instanceCullName = "VSM: Physical Page Instance Cull Pass " + passIndexString;
            const std::string dagCullName = "VSM: Physical Page DAG Cull Pass " + passIndexString;

            ProgramDesc instanceCullDesc = ProgramUtils::MakeComputeDesc(
                Renderer::GetModelShaderPath("InstanceCull.slang"),
                "computeMain",
                ProgramUtils::BindlessMode::ResourceHeap);
            instanceCullDesc.AddDefine("INSTANCE_CULL_PASS_INDEX", passIndexString);
            instanceCullDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            instanceCullDesc.AddStaticSampler("g_HZBSampler", physicalPageHZBSampler);
            AddPhysicalPagePassRootSRVs(instanceCullDesc);
            instanceCullDesc.AddRootBufferUAV("g_VsmTaskQueueStateUAV");
            s_PhysicalPageInstanceCullPrograms[passIndex] =
                ProgramUtils::GetProgram(instanceCullDesc, instanceCullName.c_str());
            if (!s_PhysicalPageInstanceCullPrograms[passIndex])
                return false;

            ProgramUtils::SetProgram(
                s_PhysicalPageInstanceCullPSOs[passIndex],
                *s_PhysicalPageInstanceCullPrograms[passIndex]);
            s_PhysicalPageInstanceCullPSOs[passIndex].Finalize();

            ProgramDesc dagCullDesc = ProgramUtils::MakeComputeDesc(
                Renderer::GetModelShaderPath("DAGCull.slang"),
                "computeMain",
                ProgramUtils::BindlessMode::ResourceHeap);
            dagCullDesc.AddDefine("DAG_CULL_PASS_INDEX", passIndexString);
            dagCullDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            dagCullDesc.AddStaticSampler("g_HZBSampler", physicalPageHZBSampler);
            AddPhysicalPagePassRootSRVs(dagCullDesc);
            dagCullDesc.AddRootBufferUAV("g_TaskQueueStateUAV");
            dagCullDesc.AddRootBufferUAV("g_TaskQueueUAV");
            dagCullDesc.AddRootBufferUAV("g_MeshletBatchUAV");
            dagCullDesc.AddRootBufferUAV("g_CandidateMeshletUAV");
            dagCullDesc.AddRootBufferUAV("g_VsmVisibleMeshletUAV");
            s_PhysicalPageDAGCullPrograms[passIndex] = ProgramUtils::GetProgram(dagCullDesc, dagCullName.c_str());
            if (!s_PhysicalPageDAGCullPrograms[passIndex])
                return false;

            ProgramUtils::SetProgram(
                s_PhysicalPageDAGCullPSOs[passIndex],
                *s_PhysicalPageDAGCullPrograms[passIndex]);
            s_PhysicalPageDAGCullPSOs[passIndex].Finalize();
        }

        ProgramDesc physicalPageMeshBufferGenDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MeshBufferGen.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        physicalPageMeshBufferGenDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
        physicalPageMeshBufferGenDesc.AddRootBufferSRV("g_VsmTaskQueueStateSRV");
        physicalPageMeshBufferGenDesc.AddRootBufferUAV("g_VsmIndirectDispatchMeshUAV");
        s_PhysicalPageMeshBufferGenProgram =
            ProgramUtils::GetProgram(physicalPageMeshBufferGenDesc, "VSM: Physical Page Mesh Buffer Gen");
        if (!s_PhysicalPageMeshBufferGenProgram)
            return false;

        ProgramUtils::SetProgram(s_PhysicalPageMeshBufferGenPSO, *s_PhysicalPageMeshBufferGenProgram);
        s_PhysicalPageMeshBufferGenPSO.Finalize();

        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            const std::string passIndexString = std::to_string(passIndex);
            ProgramDesc depthDesc = ProgramUtils::MakeGraphicsDesc(
                Renderer::GetModelShaderPath("VBufferMesh.slang"),
                "",
                "pixelMain",
                "meshMain",
                ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
            depthDesc.AddDefine("VBUFFER_MESH_PASS_INDEX", passIndexString);
            depthDesc.AddDefine("DEPTH_ONLY");
            depthDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            AddPhysicalPagePassRootSRVs(depthDesc);
            if (passIndex == 1u)
                depthDesc.AddRootBufferSRV("g_VsmTaskQueueStateSRV");
            depthDesc.AddRootBufferSRV("g_VsmVisibleMeshlets");
            const std::string depthName = "VSM: Physical Page Depth Pass " + passIndexString;
            s_PhysicalPageDepthPrograms[passIndex] = ProgramUtils::GetProgram(
                depthDesc,
                depthName.c_str());
            if (!s_PhysicalPageDepthPrograms[passIndex])
                return false;

            MeshShaderPSO& depthPSO = s_PhysicalPageDepthPSOs[passIndex];
            depthPSO.SetRasterizerState(RasterizerShadowTwoSided);
            depthPSO.SetDepthStencilState(DepthStateReadWrite);
            depthPSO.SetBlendState(BlendDisable);
            depthPSO.SetRenderTargetFormats(0, {}, DXGI_FORMAT_D32_FLOAT);
            ProgramUtils::SetProgram(depthPSO, *s_PhysicalPageDepthPrograms[passIndex]);
            depthPSO.Finalize();
        }

        s_ShadowViewsGpu.Create(
            L"VSM Shadow Views",
            kMaxShadowViews,
            sizeof(VsmShadowView));
        s_DirectionalClipmapsGpu.Create(
            L"VSM Directional Clipmaps",
            kMaxDirectionalClipmaps,
            sizeof(DirectionalVsmClipmapGpu));
        s_ResidencyStatesGpu.Create(
            L"VSM Residency States",
            kMaxDirectionalClipmaps,
            sizeof(VsmResidencyStateGpu));
        s_DirectionalAddressesGpu.Create(
            L"VSM Directional Addresses",
            kMaxShadowViews,
            sizeof(DirectionalVsmAddressGpu));
        s_ProjectionsGpu.Create(
            L"VSM Projections",
            kMaxShadowViews,
            sizeof(VsmProjectionGpu));
        s_PageRequestMaskGpu.Create(
            L"VSM Page Request Mask",
            kMaxShadowViews * kRequestMaskWordCountPerView,
            sizeof(uint32_t));
        s_RequestStatisticsGpu.Create(
            L"VSM Request Statistics",
            kMaxShadowViews,
            VSM_REQUEST_STATISTICS_STRIDE);
        s_PageTablesGpu[0].Create(L"VSM Page Table 0", kMaxShadowViews * kPagesPerView, sizeof(uint32_t));
        s_PageTablesGpu[1].Create(L"VSM Page Table 1", kMaxShadowViews * kPagesPerView, sizeof(uint32_t));
        s_PageRenderRequestsGpu.Create(
            L"VSM Page Render Requests",
            kPhysicalPageCapacity,
            sizeof(VsmPageRenderRequest));
        s_PhysicalPageMetadataGpu.Create(
            L"VSM Physical Page Metadata",
            kPhysicalPageCapacity,
            sizeof(VsmPhysicalPageMetadata));
        s_FreePhysicalPagesGpu.Create(L"VSM Free Physical Pages", kPhysicalPageCapacity, sizeof(uint32_t));
        s_PhysicalPageUsedMaskGpu.Create(
            L"VSM Physical Page Used Mask",
            kPhysicalPageCapacity / kRequestMaskWordBits,
            sizeof(uint32_t));
        s_PageManagementCountersGpu.Create(L"VSM Page Management Counters", 1, VSM_PAGE_MANAGEMENT_COUNTERS_SIZE);
        s_RenderRequestPredicateGpu.Create(
            L"VSM Render Request Predicates",
            kPhysicalPageCapacity,
            VSM_RENDER_REQUEST_PREDICATE_STRIDE);
        s_PhysicalPageViewsGpu.Create(
            L"VSM Physical Page Views",
            kPhysicalPageCapacity,
            sizeof(VsmPhysicalPageView));
        s_PageRenderMaskGpu.Create(
            L"VSM Page Render Mask",
            kMaxShadowViews * kRequestMaskWordCountPerView,
            sizeof(uint32_t));
        static_assert(kMaxShadowViews % kRequestMaskWordBits == 0u);
        s_ActiveViewMaskGpu.Create(
            L"VSM Active View Mask",
            kMaxShadowViews / kRequestMaskWordBits,
            sizeof(uint32_t));
        s_ActiveViewsGpu.Create(L"VSM Active Views", kMaxShadowViews, sizeof(uint32_t));
        s_PhysicalPageRenderCountersGpu.Create(
            L"VSM Physical Page Render Counters",
            1,
            VSM_PHYSICAL_PAGE_RENDER_COUNTERS_SIZE);
        static_assert(kPageStatisticsReadbackSize % sizeof(uint32_t) == 0);
        for (PageStatisticsReadbackSlot& slot : s_PageStatisticsReadbacks)
        {
            slot.Buffer.Create(
                L"VSM Page Statistics Readback",
                static_cast<uint32_t>(kPageStatisticsReadbackSize / sizeof(uint32_t)),
                sizeof(uint32_t));
        }
        s_PhysicalPageCullResources.Create();
        s_CullResources.Create();
        s_PhysicalPagePool.Create(
            L"VSM Physical Page Pool",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_D32_FLOAT);
        s_PhysicalHZBs[0].Create(
            L"VSM Physical HZB 0",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_R32_FLOAT);
        s_PhysicalHZBs[1].Create(
            L"VSM Physical HZB 1",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_R32_FLOAT);
        Renderer::SetBindlessResourceDescriptor(SRV_VSM_PHYSICAL_PAGE_POOL, s_PhysicalPagePool.GetDepthSRV());
        Renderer::SetBindlessResourceDescriptor(SRV_VSM_PHYSICAL_HZB0, s_PhysicalHZBs[0].GetSRV());
        Renderer::SetBindlessResourceDescriptor(SRV_VSM_PHYSICAL_HZB1, s_PhysicalHZBs[1].GetSRV());

        s_Views.reserve(kMaxShadowViews);
        s_PreviousViews.reserve(kMaxShadowViews);
        s_DirectionalClipmapsGpuData.reserve(kMaxDirectionalClipmaps);
        s_DirectionalAddressesGpuData.reserve(kMaxShadowViews);
        s_ProjectionsGpuData.reserve(kMaxShadowViews);
        s_LocalViewIds.reserve(kMaxShadowViews);
        s_DirtyViewIds.reserve(kMaxShadowViews);
        s_ResidencyStatesToInitialize.reserve(kMaxDirectionalClipmaps);
        s_DirectionalResidencySlots = {};
        s_CurrentPageTableIndex = 0;
        s_CommittedPhysicalHZBIndex = 0;
        s_FrameNumber = 0;
        s_PhysicalPagePoolInitialized = false;
        s_PageStatistics = {};
        s_PageStatisticsGeneration = 0;
        s_PageStatisticsRenderBudget = 0;
        s_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        s_Views.clear();
        s_PreviousViews.clear();
        s_DirectionalClipmapsGpuData.clear();
        s_DirectionalAddressesGpuData.clear();
        s_ProjectionsGpuData.clear();
        s_LocalViewIds.clear();
        s_DirtyViewIds.clear();
        s_ResidencyStatesToInitialize.clear();
        s_DirectionalResidencySlots = {};

        s_ShadowViewsGpu.Destroy();
        s_DirectionalClipmapsGpu.Destroy();
        s_ResidencyStatesGpu.Destroy();
        s_DirectionalAddressesGpu.Destroy();
        s_ProjectionsGpu.Destroy();
        s_PageRequestMaskGpu.Destroy();
        s_RequestStatisticsGpu.Destroy();
        s_PageTablesGpu[0].Destroy();
        s_PageTablesGpu[1].Destroy();
        s_PageRenderRequestsGpu.Destroy();
        s_PhysicalPageMetadataGpu.Destroy();
        s_FreePhysicalPagesGpu.Destroy();
        s_PhysicalPageUsedMaskGpu.Destroy();
        s_PageManagementCountersGpu.Destroy();
        s_RenderRequestPredicateGpu.Destroy();
        s_PhysicalPageViewsGpu.Destroy();
        s_PageRenderMaskGpu.Destroy();
        s_ActiveViewMaskGpu.Destroy();
        s_ActiveViewsGpu.Destroy();
        s_PhysicalPageRenderCountersGpu.Destroy();
        for (PageStatisticsReadbackSlot& slot : s_PageStatisticsReadbacks)
        {
            slot.Buffer.Destroy();
            slot.FenceValue = 0;
        }
        s_PhysicalPageCullResources.Destroy();
        s_CullResources.Destroy();
        s_PhysicalPagePool.Destroy();
        s_PhysicalHZBs[0].Destroy();
        s_PhysicalHZBs[1].Destroy();
        s_MarkDirectionalPagesProgram.reset();
        s_MarkLocalPagesProgram.reset();
        s_CommitResidencyStatesProgram.reset();
        s_UpdateResidencyStatesProgram.reset();
        s_MarkViewDirtyProgram.reset();
        s_ReuseRequestedPagesProgram.reset();
        s_BuildFreePhysicalPageListProgram.reset();
        s_AllocateNewPagesProgram.reset();
        s_MarkPhysicalPageRenderedProgram.reset();
        s_BuildPhysicalPageRenderDataProgram.reset();
        s_BuildPageFlagsMip0Program.reset();
        s_BuildPageFlagsMipProgram.reset();
        s_BuildRasterWindowMasksProgram.reset();
        s_BuildCullDispatchArgsProgram.reset();
        s_BuildRasterDispatchArgsProgram.reset();
        s_MultiViewInstanceCullProgram.reset();
        s_MultiViewDAGCullProgram.reset();
        s_DirectionalRasterDepthProgram.reset();
        s_ClearRequestedPhysicalPageProgram.reset();
        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            s_PhysicalPageInstanceCullPrograms[passIndex].reset();
            s_PhysicalPageDAGCullPrograms[passIndex].reset();
            s_PhysicalPageDepthPrograms[passIndex].reset();
        }
        s_PhysicalPageMeshBufferGenProgram.reset();
        s_CurrentPageTableIndex = 0;
        s_CommittedPhysicalHZBIndex = 0;
        s_FrameNumber = 0;
        s_PhysicalPagePoolInitialized = false;
        s_PageStatistics = {};
        s_PageStatisticsGeneration = 0;
        s_PageStatisticsRenderBudget = 0;
        s_Initialized = false;
    }

    void Reset(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before Reset.");

        s_Views.clear();
        s_PreviousViews.clear();
        s_DirectionalClipmapsGpuData.clear();
        s_DirectionalAddressesGpuData.clear();
        s_ProjectionsGpuData.clear();
        s_LocalViewIds.clear();
        s_DirtyViewIds.clear();
        s_ResidencyStatesToInitialize.clear();
        s_DirectionalResidencySlots = {};
        s_CurrentPageTableIndex = 0;
        s_CommittedPhysicalHZBIndex = 0;
        s_FrameNumber = 0;
        ++s_PageStatisticsGeneration;
        s_PageStatisticsRenderBudget = 0;
        PublishPageStatistics({});

        ComputeContext& context = gfxContext.GetComputeContext();
        const auto clearBuffer = [&context](GpuBuffer& buffer, uint32_t value = 0u)
        {
            context.TransitionResource(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.ClearUAV(buffer, value);
            context.InsertUAVBarrier(buffer, true);
        };

        clearBuffer(s_ShadowViewsGpu);
        clearBuffer(s_DirectionalClipmapsGpu);
        clearBuffer(s_DirectionalAddressesGpu);
        clearBuffer(s_ProjectionsGpu);
        clearBuffer(s_PageRequestMaskGpu);
        clearBuffer(s_RequestStatisticsGpu);
        clearBuffer(s_PageTablesGpu[0], kInvalidPageTableEntry);
        clearBuffer(s_PageTablesGpu[1], kInvalidPageTableEntry);
        clearBuffer(s_PageRenderRequestsGpu);
        clearBuffer(s_PhysicalPageMetadataGpu);
        clearBuffer(s_FreePhysicalPagesGpu);
        clearBuffer(s_PhysicalPageUsedMaskGpu);
        clearBuffer(s_PageManagementCountersGpu);
        clearBuffer(s_RenderRequestPredicateGpu);
        clearBuffer(s_PhysicalPageViewsGpu);
        clearBuffer(s_PageRenderMaskGpu);
        clearBuffer(s_ActiveViewMaskGpu);
        clearBuffer(s_ActiveViewsGpu);
        clearBuffer(s_PhysicalPageRenderCountersGpu);
        clearBuffer(s_PhysicalPageCullResources.QueueStateGpu);
        clearBuffer(s_PhysicalPageCullResources.LegacyVisibleMeshletsGpu);
        clearBuffer(s_PhysicalPageCullResources.IndirectDispatchMeshGpu);
        clearBuffer(s_CullResources.PageFlagsGpu);
        clearBuffer(s_CullResources.RasterWindowPageMasksGpu);
        clearBuffer(s_CullResources.QueueStateGpu);
        clearBuffer(s_CullResources.NodeTasksGpu, INVALID_ID);
        clearBuffer(s_CullResources.MeshletBatchGpu);
        clearBuffer(s_CullResources.CandidateMeshletsGpu);
        clearBuffer(s_CullResources.RasterItemsGpu);
        clearBuffer(s_CullResources.CullDispatchArgsGpu);
        clearBuffer(s_CullResources.RasterDispatchArgsGpu);
        clearBuffer(s_CullResources.CountersGpu);

        gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        gfxContext.ClearDepth(s_PhysicalPagePool);
        gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s_PhysicalHZBs[0].GenerateHZB(gfxContext, s_PhysicalPagePool);
        s_PhysicalHZBs[1].GenerateHZB(gfxContext, s_PhysicalPagePool);
        s_PhysicalPagePoolInitialized = true;
    }

    void BeginFrame()
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before BeginFrame.");

        UpdatePageStatisticsReadback();
        s_PreviousViews.swap(s_Views);
        s_Views.clear();
        s_DirectionalClipmapsGpuData.clear();
        s_DirectionalAddressesGpuData.clear();
        s_ProjectionsGpuData.clear();
        s_LocalViewIds.clear();
        s_DirtyViewIds.clear();
        s_CurrentPageTableIndex ^= 1u;
        ++s_FrameNumber;
    }

    namespace
    {
        uint32_t AddDirectionalLevelView(
            const DirectionalVsmAddressDesc& desc,
            const DirectionalVsmLevelDepthState& depthState)
        {
            const DirectionalVsmAddressConstants addressConstants = BuildDirectionalVsmAddressConstants(desc);
            const DirectionalVsmAddressGpu addressData = BuildDirectionalVsmAddressGpu(addressConstants);
            const uint32_t addressDataIndex = static_cast<uint32_t>(s_DirectionalAddressesGpuData.size());
            const uint32_t projectionDataIndex = static_cast<uint32_t>(s_ProjectionsGpuData.size());
            const uint32_t viewId = static_cast<uint32_t>(s_Views.size());

            VsmShadowView view{};
            view.StableShadowMapId = desc.StableShadowMapId;
            view.AddressGeneration = desc.AddressGeneration;
            view.AddressType = VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP;
            view.AddressDataIndex = addressDataIndex;
            view.RequestMaskWordBase = viewId * kRequestMaskWordCountPerView;
            view.PageTableBase = viewId * kPagesPerView;
            view.PreviousPageTableBase = FindPreviousPageTableBase(
                desc.StableShadowMapId,
                VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP,
                desc.ClipmapLevel);
            view.Layer = desc.ClipmapLevel;
            view.LightIndex = desc.LightIndex;
            view.ProjectionDataIndex = projectionDataIndex;
            view.LodScale = addressConstants.InvWorldUnitsPerPage > 0.0f
                ? 1.0f / (addressConstants.InvWorldUnitsPerPage * static_cast<float>(kPageSize))
                : 0.0f;

            s_Views.push_back(view);
            s_DirectionalAddressesGpuData.push_back(addressData);
            s_ProjectionsGpuData.push_back(BuildDirectionalVsmProjectionGpu(desc, addressData, depthState));
            return viewId;
        }
    } // namespace

    uint32_t AddDirectionalClipmap(const DirectionalVsmClipmapDesc& desc)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before adding clipmaps.");
        ASSERT(desc.LevelCount > 0u, "A directional VSM clipmap must contain at least one level.");
        ASSERT(desc.LevelCount <= kMaxDirectionalClipmapLevels, "Exceeded the directional VSM clipmap level limit.");
        ASSERT(std::isfinite(desc.FirstLevelExtent) && desc.FirstLevelExtent > 0.0f,
               "The directional VSM first-level extent must be positive and finite.");
        if (s_DirectionalClipmapsGpuData.size() >= kMaxDirectionalClipmaps ||
            s_Views.size() + desc.LevelCount > kMaxShadowViews)
        {
            return kInvalidViewId;
        }

        const uint32_t clipmapId = static_cast<uint32_t>(s_DirectionalClipmapsGpuData.size());
        const uint32_t firstViewId = static_cast<uint32_t>(s_Views.size());
        const uint32_t residencyStateIndex = AcquireDirectionalResidencyState(desc.StableShadowMapId);
        DirectionalResidencySlot& residencySlot = s_DirectionalResidencySlots[residencyStateIndex];
        if (residencySlot.AddressGeneration != desc.AddressGeneration)
        {
            residencySlot.AddressGeneration = desc.AddressGeneration;
            residencySlot.LevelDepthStates = {};
        }

        const Math::Vector3 originLS = desc.WorldToLightRotation * desc.OriginWS;
        const float desiredCenterZ = static_cast<float>(originLS.GetZ());
        for (uint32_t level = 0; level < desc.LevelCount; ++level)
        {
            DirectionalVsmAddressDesc levelDesc;
            levelDesc.WorldToLightRotation = desc.WorldToLightRotation;
            levelDesc.FocusPositionWS = desc.OriginWS;
            levelDesc.LevelWorldExtent = std::ldexp(desc.FirstLevelExtent, static_cast<int>(level));
            levelDesc.LightIndex = desc.LightIndex;
            levelDesc.StableShadowMapId = desc.StableShadowMapId;
            levelDesc.ClipmapLevel = level;
            levelDesc.AddressGeneration = desc.AddressGeneration;

            DirectionalVsmLevelDepthState& depthState = residencySlot.LevelDepthStates[level];
            const bool depthRangeChanged =
                UpdateDirectionalLevelDepthState(depthState, desiredCenterZ, levelDesc.LevelWorldExtent * 0.5f);
            const uint32_t viewId = AddDirectionalLevelView(levelDesc, depthState);
            ASSERT(viewId == firstViewId + level, "Directional VSM clipmap views must be contiguous.");
            if (depthRangeChanged)
                MarkViewDirty(viewId);
        }

        s_Views[firstViewId + desc.LevelCount - 1u].Flags |= VSM_SHADOW_VIEW_FLAG_COARSE_FALLBACK;

        DirectionalVsmClipmapGpu clipmap{};
        clipmap.OriginAndFirstLevelRadius = PackFloat4(
            desc.OriginWS,
            desc.FirstLevelExtent * kDirectionalClipmapSelectionRadiusScale);
        clipmap.FirstViewId = firstViewId;
        clipmap.LevelCount = desc.LevelCount;
        clipmap.ResidencyStateIndex = residencyStateIndex;
        s_DirectionalClipmapsGpuData.push_back(clipmap);
        return clipmapId;
    }

    uint32_t AddLocalView(const LocalVsmViewDesc& desc)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before adding views.");
        if (s_Views.size() >= kMaxShadowViews)
            return kInvalidViewId;

        const uint32_t projectionDataIndex = static_cast<uint32_t>(s_ProjectionsGpuData.size());
        const uint32_t viewId = static_cast<uint32_t>(s_Views.size());

        VsmShadowView view{};
        view.StableShadowMapId = desc.StableShadowMapId;
        view.AddressGeneration = desc.AddressGeneration;
        view.AddressType = VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE;
        view.AddressDataIndex = kInvalidViewId;
        view.RequestMaskWordBase = viewId * kRequestMaskWordCountPerView;
        view.PageTableBase = viewId * kPagesPerView;
        view.PreviousPageTableBase = FindPreviousPageTableBase(
            desc.StableShadowMapId,
            VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE,
            desc.Layer);
        view.Layer = desc.Layer;
        view.LightIndex = desc.LightIndex;
        view.ProjectionDataIndex = projectionDataIndex;
        view.LodScale = ComputePerspectiveVsmLodScale(desc.ProjMatrix);

        s_Views.push_back(view);
        s_ProjectionsGpuData.push_back(BuildVsmProjectionGpu(
            desc.ViewProjMatrix,
            desc.ViewerPositionWS,
            VSM_PROJECTION_TYPE_PERSPECTIVE));
        s_LocalViewIds.push_back(viewId);
        return viewId;
    }

    void MarkViewDirty(uint32_t viewId)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before marking a view dirty.");
        ASSERT(viewId < s_Views.size(), "Invalid VSM shadow-view index.");

        if (std::find(s_DirtyViewIds.begin(), s_DirtyViewIds.end(), viewId) == s_DirtyViewIds.end())
            s_DirtyViewIds.push_back(viewId);
    }

    void MarkClipmapDirty(uint32_t clipmapId)
    {
        ASSERT(clipmapId < s_DirectionalClipmapsGpuData.size(), "Invalid directional VSM clipmap index.");

        const DirectionalVsmClipmapGpu& clipmap = s_DirectionalClipmapsGpuData[clipmapId];
        for (uint32_t level = 0; level < clipmap.LevelCount; ++level)
            MarkViewDirty(clipmap.FirstViewId + level);
    }

    const VsmShadowView& GetView(uint32_t viewId)
    {
        ASSERT(viewId < s_Views.size(), "Invalid VSM shadow-view index.");
        return s_Views[viewId];
    }

    bool IsInitialized()
    {
        return s_Initialized;
    }

    uint32_t GetViewCount()
    {
        return static_cast<uint32_t>(s_Views.size());
    }

    const PageStatistics& GetPageStatistics()
    {
        return s_PageStatistics;
    }

    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before marking pages.");
        if (s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Mark Requested Pages", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();
        UploadFrameData(context);
        CommitResidencyStates(context);

        const size_t requestMaskBytes = s_Views.size() * kRequestMaskWordCountPerView * sizeof(uint32_t);
        const size_t statisticsBytes = s_Views.size() * VSM_REQUEST_STATISTICS_STRIDE;
        context.ClearBufferUAV(s_PageRequestMaskGpu, requestMaskBytes, 0);
        context.ClearBufferUAV(s_RequestStatisticsGpu, statisticsBytes, 0);
        context.InsertUAVBarrier(s_PageRequestMaskGpu);
        context.InsertUAVBarrier(s_RequestStatisticsGpu);

        context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalClipmapsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
        const Renderer::ViewConstants& viewConstants = receiverView.GetConstants();

        if (!s_DirectionalClipmapsGpuData.empty())
        {
            ProgramBinder binder(*s_MarkDirectionalPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_MarkDirectionalPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmClipmaps", s_DirectionalClipmapsGpu);
            binder.SetRootBufferSRV("g_VsmResidencyStates", s_ResidencyStatesGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferUAV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferUAV("g_VsmRequestStatistics", s_RequestStatisticsGpu);

            SetMarkRequestedPagesConstants(binder, viewConstants);
            ProgramVar constants = binder["g_MarkVsmPages"];

            for (uint32_t clipmapId = 0; clipmapId < s_DirectionalClipmapsGpuData.size(); ++clipmapId)
            {
                constants["TargetId"].Set(clipmapId);
                binder.Apply();
                context.Dispatch2D(viewConstants.ViewportWidth, viewConstants.ViewportHeight);
            }
        }

        if (!s_LocalViewIds.empty())
        {
            ProgramBinder binder(*s_MarkLocalPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_MarkLocalPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_VsmProjections", s_ProjectionsGpu);
            binder.SetRootBufferUAV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferUAV("g_VsmRequestStatistics", s_RequestStatisticsGpu);

            SetMarkRequestedPagesConstants(binder, viewConstants);
            ProgramVar constants = binder["g_MarkVsmPages"];

            for (uint32_t viewId : s_LocalViewIds)
            {
                constants["TargetId"].Set(viewId);
                binder.Apply();
                context.Dispatch2D(viewConstants.ViewportWidth, viewConstants.ViewportHeight);
            }
        }

        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void AllocateRequestedPages(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before allocating pages.");
        if (s_Views.empty())
            return;

        s_PageStatisticsRenderBudget = GetPhysicalPageRenderBudget();

        ScopedTimer timer(L"VSM: Manage Physical Pages", gfxContext);

        // Cached pages must survive between frames. Only initialize the pool once;
        // the future page renderer will clear individual physical pages before writing them.
        if (!s_PhysicalPagePoolInitialized)
        {
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.ClearDepth(s_PhysicalPagePool);
            s_PhysicalPagePoolInitialized = true;
        }

        ComputeContext& context = gfxContext.GetComputeContext();
        StructuredBuffer& currentPageTable = GetCurrentPageTable();
        StructuredBuffer& previousPageTable = GetPreviousPageTable();
        const size_t pageTableBytes = s_Views.size() * kPagesPerView * sizeof(uint32_t);
        const size_t physicalPageUsedMaskBytes = kPhysicalPageCapacity / kRequestMaskWordBits * sizeof(uint32_t);
        context.ClearBufferUAV(currentPageTable, pageTableBytes, kInvalidPageTableEntry);
        context.ClearBufferUAV(s_PhysicalPageUsedMaskGpu, physicalPageUsedMaskBytes, 0);
        context.ClearBufferUAV(s_PageManagementCountersGpu, VSM_PAGE_MANAGEMENT_COUNTERS_SIZE, 0);
        context.ClearBufferUAV(
            s_RenderRequestPredicateGpu,
            s_RenderRequestPredicateGpu.GetBufferSize(),
            0);
        context.InsertUAVBarrier(currentPageTable);
        context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);
        context.InsertUAVBarrier(s_RenderRequestPredicateGpu);

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(previousPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PhysicalPageUsedMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        MarkPendingViewsDirty(context);

        DispatchReuseRequestedPages(
            context,
            currentPageTable,
            previousPageTable);
        InsertPageMappingUAVBarriers(context, true);

        context.TransitionResource(s_PhysicalPageUsedMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_FreePhysicalPagesGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        {
            ProgramBinder binder(*s_BuildFreePhysicalPageListProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_BuildFreePhysicalPageListPSO);

            binder.SetRootBufferSRV("g_VsmPhysicalPageUsedMaskSRV", s_PhysicalPageUsedMaskGpu);
            binder.SetRootBufferUAV("g_VsmFreePhysicalPagesUAV", s_FreePhysicalPagesGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.Apply();

            context.Dispatch(kPhysicalPageGroupCount, 1, 1);
        }

        context.InsertUAVBarrier(s_FreePhysicalPagesGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);
        context.TransitionResource(s_FreePhysicalPagesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        DispatchAllocateNewPages(context, currentPageTable, VSM_PAGE_ALLOCATION_CLASS_COARSE);
        InsertPageMappingUAVBarriers(context, false);

        DispatchAllocateNewPages(context, currentPageTable, VSM_PAGE_ALLOCATION_CLASS_DETAIL);

        UpdateResidencyStates(context);

        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.FlushResourceBarriers();
    }

    void BuildPhysicalPageRenderData(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before building physical page render data.");
        if (s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Build Physical Page Render Data", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();
        const size_t renderMaskBytes =
            s_Views.size() * kRequestMaskWordCountPerView * sizeof(uint32_t);
        const size_t activeViewMaskBytes =
            (s_Views.size() + kRequestMaskWordBits - 1u) / kRequestMaskWordBits * sizeof(uint32_t);

        context.ClearBufferUAV(s_PageRenderMaskGpu, renderMaskBytes, 0u);
        context.ClearBufferUAV(s_ActiveViewMaskGpu, activeViewMaskBytes, 0u);
        context.ClearBufferUAV(
            s_PhysicalPageRenderCountersGpu,
            VSM_PHYSICAL_PAGE_RENDER_COUNTERS_SIZE,
            0u);
        context.InsertUAVBarrier(s_PageRenderMaskGpu);
        context.InsertUAVBarrier(s_ActiveViewMaskGpu);
        context.InsertUAVBarrier(s_PhysicalPageRenderCountersGpu);

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageRenderMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_ActiveViewMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_ActiveViewsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PhysicalPageRenderCountersGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        ProgramBinder binder(*s_BuildPhysicalPageRenderDataProgram, context);
        binder.SetRootSignature();
        context.SetPipelineState(s_BuildPhysicalPageRenderDataPSO);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmProjections", s_ProjectionsGpu);
        binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
        binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
        binder.SetRootBufferUAV("g_VsmPhysicalPageViews", s_PhysicalPageViewsGpu);
        binder.SetRootBufferUAV("g_VsmPageRenderMask", s_PageRenderMaskGpu);
        binder.SetRootBufferUAV("g_VsmActiveViewMask", s_ActiveViewMaskGpu);
        binder.SetRootBufferUAV("g_VsmActiveViews", s_ActiveViewsGpu);
        binder.SetRootBufferUAV("g_VsmPhysicalPageRenderCounters", s_PhysicalPageRenderCountersGpu);
        binder["g_BuildVsmPhysicalPageRenderData"]["ViewCount"].Set(static_cast<uint32_t>(s_Views.size()));
        binder.Apply();

        context.Dispatch1D(kPhysicalPageCapacity);
        context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ActiveViewMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ActiveViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(
            s_PhysicalPageRenderCountersGpu,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.FlushResourceBarriers();
    }

    namespace
    {
        void PreparePhysicalPageRenderResources(GraphicsContext& gfxContext)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            for (GpuBuffer& geometryChunk : GeometryStreaming::m_GeometryChunksGPU)
                context.TransitionResource(geometryChunk, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            context.TransitionResource(
                GeometryStreaming::m_GroupDataLocationGPU,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }

        void BuildDirectionalPageFlags(ComputeContext& context)
        {
            const uint32_t viewCount = static_cast<uint32_t>(s_Views.size());

            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ActiveViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                s_PhysicalPageRenderCountersGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageRenderMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.PageFlagsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                s_CullResources.RasterWindowPageMasksGpu,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            {
                ProgramBinder binder(*s_BuildPageFlagsMip0Program, context);
                binder.SetRootSignature();
                context.SetPipelineState(s_BuildPageFlagsMip0PSO);
                binder.SetRootBufferSRV("g_VsmCullShadowViews", s_ShadowViewsGpu);
                binder.SetRootBufferSRV("g_VsmCullDirectionalAddresses", s_DirectionalAddressesGpu);
                binder.SetRootBufferSRV("g_VsmActiveViews", s_ActiveViewsGpu);
                binder.SetRootBufferSRV("g_VsmPhysicalPageRenderCounters", s_PhysicalPageRenderCountersGpu);
                binder.SetRootBufferSRV("g_VsmPageRenderMask", s_PageRenderMaskGpu);
                binder.SetRootBufferUAV("g_VsmPageFlagsUAV", s_CullResources.PageFlagsGpu);
                binder.Apply();
                context.Dispatch3D(VSM_PAGE_TABLE_DIM, VSM_PAGE_TABLE_DIM, viewCount, 8u, 8u, 1u);
            }

            context.InsertUAVBarrier(s_CullResources.PageFlagsGpu);

            {
                ProgramBinder binder(*s_BuildPageFlagsMipProgram, context);
                binder.SetRootSignature();
                context.SetPipelineState(s_BuildPageFlagsMipPSO);
                binder.SetRootBufferSRV("g_VsmCullShadowViews", s_ShadowViewsGpu);
                binder.SetRootBufferSRV("g_VsmActiveViews", s_ActiveViewsGpu);
                binder.SetRootBufferSRV("g_VsmPhysicalPageRenderCounters", s_PhysicalPageRenderCountersGpu);
                binder.SetRootBufferUAV("g_VsmPageFlagsUAV", s_CullResources.PageFlagsGpu);

                for (uint32_t mipLevel = 1u; mipLevel < kPageFlagsMipCount; ++mipLevel)
                {
                    binder["g_BuildVsmPageFlags"]["MipLevel"].Set(mipLevel);
                    binder.Apply();
                    const uint32_t mipDimension = kPageTableDim >> mipLevel;
                    context.Dispatch3D(mipDimension, mipDimension, viewCount, 8u, 8u, 1u);
                    context.InsertUAVBarrier(s_CullResources.PageFlagsGpu);
                }
            }

            {
                ProgramBinder binder(*s_BuildRasterWindowMasksProgram, context);
                binder.SetRootSignature();
                context.SetPipelineState(s_BuildRasterWindowMasksPSO);
                binder.SetRootBufferSRV("g_VsmCullShadowViews", s_ShadowViewsGpu);
                binder.SetRootBufferSRV("g_VsmActiveViews", s_ActiveViewsGpu);
                binder.SetRootBufferSRV("g_VsmPhysicalPageRenderCounters", s_PhysicalPageRenderCountersGpu);
                binder.SetRootBufferUAV("g_VsmPageFlagsUAV", s_CullResources.PageFlagsGpu);
                binder.SetRootBufferUAV(
                    "g_VsmRasterWindowPageMasksUAV",
                    s_CullResources.RasterWindowPageMasksGpu);
                binder.Apply();
                context.Dispatch3D(kRasterWindowDim, kRasterWindowDim, viewCount, 8u, 8u, 1u);
            }

            context.TransitionResource(s_CullResources.PageFlagsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                s_CullResources.RasterWindowPageMasksGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }

        void BuildCullDispatchArgs(ComputeContext& context)
        {
            context.TransitionResource(s_PhysicalPageRenderCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.CullDispatchArgsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_BuildCullDispatchArgsProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_BuildCullDispatchArgsPSO);
            binder.SetRootBufferSRV("g_VsmPhysicalPageRenderCounters", s_PhysicalPageRenderCountersGpu);
            binder.SetRootBufferUAV("g_VsmCullDispatchArgs", s_CullResources.CullDispatchArgsGpu);
            ProgramVar constants = binder["g_BuildVsmCullDispatchArgs"];
            constants["InstanceGroupCount"].Set(
                Math::DivideByMultiple(DrawCommandManager::GetNumPotentialDrawItems(), 64u));
            constants["DagGroupCount"].Set(Renderer::GetDAGCullGroupCount());
            binder.Apply();
            context.Dispatch(1u, 1u, 1u);

            context.TransitionResource(s_CullResources.CullDispatchArgsGpu, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            context.FlushResourceBarriers();
        }

        void BuildRasterDispatchArgs(ComputeContext& context)
        {
            context.TransitionResource(s_CullResources.QueueStateGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.RasterDispatchArgsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_BuildRasterDispatchArgsProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_BuildRasterDispatchArgsPSO);
            binder.SetRootBufferSRV("g_VsmCullQueueState", s_CullResources.QueueStateGpu);
            binder.SetRootBufferUAV("g_VsmRasterDispatchArgs", s_CullResources.RasterDispatchArgsGpu);
            binder.Apply();
            context.Dispatch(1u, 1u, 1u);

            context.TransitionResource(s_CullResources.RasterDispatchArgsGpu, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            context.FlushResourceBarriers();
        }

        void ResetMultiViewCullResources(ComputeContext& context)
        {
            context.ClearBufferUAV(
                s_CullResources.QueueStateGpu,
                s_CullResources.QueueStateGpu.GetBufferSize(),
                0u);
            context.ClearBufferUAV(s_CullResources.CountersGpu, VSM_CULL_COUNTERS_SIZE, 0u);
            context.InsertUAVBarrier(s_CullResources.QueueStateGpu);
            context.InsertUAVBarrier(s_CullResources.NodeTasksGpu);
            context.InsertUAVBarrier(s_CullResources.MeshletBatchGpu);
            context.InsertUAVBarrier(s_CullResources.CountersGpu);
        }

        void DispatchMultiViewCull(ComputeContext& context, const Renderer::FrameConstants& frame)
        {
            context.TransitionResource(
                DrawCommandManager::GetPotentialDrawItemsGPU(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ActiveViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.PageFlagsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.QueueStateGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(s_CullResources.NodeTasksGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            context.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());

            {
                ProgramBinder binder(*s_MultiViewInstanceCullProgram, context);
                binder.SetRootSignature();
                context.SetPipelineState(s_MultiViewInstanceCullPSO);
                SetCommonResources(binder, frame);
                binder["g_VsmInstanceCull"]["MaxCommands"].Set(
                    DrawCommandManager::GetNumPotentialDrawItems());
                binder.SetRootBufferSRV("g_VsmCullShadowViews", s_ShadowViewsGpu);
                binder.SetRootBufferSRV("g_VsmCullDirectionalAddresses", s_DirectionalAddressesGpu);
                binder.SetRootBufferSRV("g_VsmCullProjections", s_ProjectionsGpu);
                binder.SetRootBufferSRV("g_VsmPageFlags", s_CullResources.PageFlagsGpu);
                binder.SetRootBufferSRV("g_VsmActiveViews", s_ActiveViewsGpu);
                binder.SetRootBufferUAV("g_VsmCullQueueState", s_CullResources.QueueStateGpu);
                binder.SetRootBufferUAV("g_VsmNodeTasks", s_CullResources.NodeTasksGpu);
                binder.SetRootBufferUAV("g_VsmCullCounters", s_CullResources.CountersGpu);
                binder.Apply();
                context.DispatchIndirect(s_CullResources.CullDispatchArgsGpu, 0u);
            }

            context.InsertUAVBarrier(s_CullResources.QueueStateGpu);
            context.InsertUAVBarrier(s_CullResources.NodeTasksGpu);
            context.TransitionResource(s_CullResources.MeshletBatchGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(s_CullResources.CandidateMeshletsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(s_CullResources.RasterItemsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                s_CullResources.RasterWindowPageMasksGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                GeometryStreaming::m_GeometryStreamingRequestMaskGPU,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            {
                ProgramBinder binder(*s_MultiViewDAGCullProgram, context);
                binder.SetRootSignature();
                context.SetPipelineState(s_MultiViewDAGCullPSO);
                SetCommonResources(binder, frame);
                ProgramVar constants = binder["g_DAGCull"];
                constants["PixelErrorThreshold"].Set(Renderer::GetPixelErrorThreshold());
                constants["ViewportWidth"].Set(kVirtualResolution);
                constants["ViewportHeight"].Set(kVirtualResolution);
                binder.SetRootBufferSRV("g_VsmCullShadowViews", s_ShadowViewsGpu);
                binder.SetRootBufferSRV("g_VsmCullDirectionalAddresses", s_DirectionalAddressesGpu);
                binder.SetRootBufferSRV("g_VsmCullProjections", s_ProjectionsGpu);
                binder.SetRootBufferSRV("g_VsmPageFlags", s_CullResources.PageFlagsGpu);
                binder.SetRootBufferSRV(
                    "g_VsmRasterWindowPageMasks",
                    s_CullResources.RasterWindowPageMasksGpu);
                binder.SetRootBufferUAV("g_TaskQueueStateUAV", s_CullResources.QueueStateGpu);
                binder.SetRootBufferUAV("g_VsmNodeTasksUAV", s_CullResources.NodeTasksGpu);
                binder.SetRootBufferUAV("g_MeshletBatchUAV", s_CullResources.MeshletBatchGpu);
                binder.SetRootBufferUAV("g_VsmCandidateMeshletsUAV", s_CullResources.CandidateMeshletsGpu);
                binder.SetRootBufferUAV("g_VsmRasterItemsUAV", s_CullResources.RasterItemsGpu);
                binder.SetRootBufferUAV("g_VsmCullCounters", s_CullResources.CountersGpu);
                binder.Apply();
                context.DispatchIndirect(s_CullResources.CullDispatchArgsGpu, sizeof(D3D12_DISPATCH_ARGUMENTS));
            }

            context.TransitionResource(s_CullResources.RasterItemsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }

        void ResetPhysicalPageCullBuffers(GraphicsContext& gfxContext)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            context.ClearBufferUAV(
                s_PhysicalPageCullResources.QueueStateGpu,
                s_PhysicalPageCullResources.QueueStateGpu.GetBufferSize(),
                0);
            gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ClearUAV(DrawCommandManager::GetTaskQueueGPU(), 0xffffffffu);
            gfxContext.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ClearUAV(DrawCommandManager::GetMeshletBatchGPU(), 0);
            context.InsertUAVBarrier(s_PhysicalPageCullResources.QueueStateGpu);
            context.InsertUAVBarrier(DrawCommandManager::GetTaskQueueGPU());
            context.InsertUAVBarrier(DrawCommandManager::GetMeshletBatchGPU());
        }

        void ClearRequestedPhysicalPageRange(
            GraphicsContext& gfxContext,
            uint32_t firstRenderRequestIndex,
            uint32_t instanceCount,
            uint32_t addressType = VSM_ADDRESS_TYPE_INVALID)
        {
            ASSERT(s_Initialized, "VirtualShadowMap must be initialized before clearing physical pages.");
            if (s_Views.empty() || instanceCount == 0u)
                return;

            ScopedTimer timer(L"VSM: Clear Requested Physical Pages", gfxContext);

            constexpr D3D12_RESOURCE_STATES kGraphicsShaderResourceState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            gfxContext.TransitionResource(s_ShadowViewsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PageRenderRequestsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PageManagementCountersGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.FlushResourceBarriers();

            ProgramBinder binder(*s_ClearRequestedPhysicalPageProgram, gfxContext);
            binder.SetRootSignature();
            gfxContext.SetPipelineState(s_ClearRequestedPhysicalPagePSO);
            gfxContext.SetDepthStencilTarget(s_PhysicalPagePool.GetDSV());
            gfxContext.SetViewportAndScissor(0, 0, kPhysicalPoolResolution, kPhysicalPoolResolution);
            gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            ProgramVar constants = binder["g_ClearVsmPhysicalPage"];
            constants["FirstRenderRequestIndex"].Set(firstRenderRequestIndex);
            constants["AddressType"].Set(addressType);
            binder.Apply();

            gfxContext.DrawInstanced(6, instanceCount, 0, 0);
        }

        void DrawDirectionalPhysicalPagesDepth(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame)
        {
            constexpr D3D12_RESOURCE_STATES kGraphicsShaderResourceState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            gfxContext.TransitionResource(
                s_CullResources.RasterDispatchArgsGpu,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(s_CullResources.QueueStateGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_CullResources.RasterItemsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_ShadowViewsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_DirectionalAddressesGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(GetCurrentPageTable(), kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPageViewsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.FlushResourceBarriers();

            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                Renderer::s_SamplerHeap.GetHeapPointer());
            gfxContext.SetDepthStencilTarget(s_PhysicalPagePool.GetDSV());
            gfxContext.SetViewportAndScissor(0, 0, kPhysicalPoolResolution, kPhysicalPoolResolution);
            gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ProgramBinder binder(*s_DirectionalRasterDepthProgram, gfxContext);
            binder.SetRootSignature();
            gfxContext.SetPipelineState(s_DirectionalRasterDepthPSO);
            SetCommonResources(binder, frame);
            binder.SetRootBufferSRV("g_VsmCullQueueState", s_CullResources.QueueStateGpu);
            binder.SetRootBufferSRV("g_VsmRasterItems", s_CullResources.RasterItemsGpu);
            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageTable", GetCurrentPageTable());
            binder.SetRootBufferSRV("g_VsmPhysicalPageViews", s_PhysicalPageViewsGpu);
            binder.Apply();

            gfxContext.ExecuteIndirect(
                Renderer::GPUDrivenDrawIndirectCommandSignature,
                s_CullResources.RasterDispatchArgsGpu,
                0u,
                1u);
        }
    } // namespace

    void BuildDirectionalRasterItems(GraphicsContext& gfxContext, const Renderer::FrameConstants& frame)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before building raster items.");

        ComputeContext& context = gfxContext.GetComputeContext();
        if (s_DirectionalClipmapsGpuData.empty() || DrawCommandManager::GetNumPotentialDrawItems() == 0u)
        {
            context.ClearBufferUAV(
                s_CullResources.QueueStateGpu,
                s_CullResources.QueueStateGpu.GetBufferSize(),
                0u);
            context.ClearBufferUAV(s_CullResources.CountersGpu, VSM_CULL_COUNTERS_SIZE, 0u);
            context.TransitionResource(
                s_CullResources.QueueStateGpu,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_CullResources.CountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            BuildRasterDispatchArgs(context);
            return;
        }

        ScopedTimer timer(L"VSM: Build Directional Raster Items", gfxContext);
        PreparePhysicalPageRenderResources(gfxContext);
        BuildDirectionalPageFlags(context);
        ResetMultiViewCullResources(context);
        BuildCullDispatchArgs(context);
        DispatchMultiViewCull(context, frame);
        BuildRasterDispatchArgs(context);
    }

    void ClearRequestedPhysicalPages(GraphicsContext& gfxContext)
    {
        ClearRequestedPhysicalPageRange(gfxContext, 0u, kPhysicalPageCapacity);
    }

    void ClearRequestedPhysicalPage(GraphicsContext& gfxContext, uint32_t renderRequestIndex)
    {
        ClearRequestedPhysicalPageRange(gfxContext, renderRequestIndex, 1u);
    }

    void RenderDirectionalPhysicalPagesDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before rendering physical pages.");
        ASSERT(s_PhysicalPagePoolInitialized, "VSM physical page pool must be initialized before rendering.");
        if (s_DirectionalClipmapsGpuData.empty())
            return;

        s_PageStatisticsRenderBudget = kPhysicalPageCapacity;
        ScopedTimer timer(L"VSM: Render Directional Physical Pages Depth", gfxContext);
        PreparePhysicalPageRenderResources(gfxContext);
        ClearRequestedPhysicalPageRange(
            gfxContext,
            0u,
            kPhysicalPageCapacity,
            VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP);
        DrawDirectionalPhysicalPagesDepth(gfxContext, frame);
        GeneratePendingPhysicalHZB(gfxContext);
        MarkPhysicalPagesRendered(
            gfxContext,
            0u,
            kPhysicalPageCapacity,
            VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP,
            true);
        CommitPendingPhysicalHZB();
    }

    void RenderRequestedPhysicalPagesDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before rendering physical pages.");
        ASSERT(s_PhysicalPagePoolInitialized, "VSM physical page pool must be initialized before rendering.");
        if (s_Views.empty() || DrawCommandManager::GetNumPotentialDrawItems() == 0)
        {
            return;
        }

        ScopedTimer timer(L"VSM: Render Requested Physical Pages Depth", gfxContext);
        PreparePhysicalPageRenderResources(gfxContext);

        const Renderer::HZBResources hzbResources = GetPhysicalHZBResources();
        const uint32_t renderBudget = GetPhysicalPageRenderBudget();

        gfxContext.TransitionResource(s_RenderRequestPredicateGpu, D3D12_RESOURCE_STATE_PREDICATION);
        gfxContext.FlushResourceBarriers();
        gfxContext.SetPredication(
            s_RenderRequestPredicateGpu.GetResource(),
            GetRenderRequestPredicateOffset(0u),
            D3D12_PREDICATION_OP_EQUAL_ZERO);

        ClearRequestedPhysicalPageRange(gfxContext, 0u, renderBudget);

        for (uint32_t renderRequestIndex = 0; renderRequestIndex < renderBudget; ++renderRequestIndex)
        {
            gfxContext.SetPredication(
                s_RenderRequestPredicateGpu.GetResource(),
                GetRenderRequestPredicateOffset(renderRequestIndex),
                D3D12_PREDICATION_OP_EQUAL_ZERO);

            ResetPhysicalPageCullBuffers(gfxContext);

            DispatchPhysicalPageCull(gfxContext, frame, hzbResources, renderRequestIndex, 0u);
            BuildPhysicalPageDrawCommand(gfxContext, frame, 0u);
            DrawPhysicalPageDepth(gfxContext, frame, renderRequestIndex, 0u);

            GeneratePendingPhysicalHZB(gfxContext);

            DispatchPhysicalPageCull(gfxContext, frame, hzbResources, renderRequestIndex, 1u);
            BuildPhysicalPageDrawCommand(gfxContext, frame, 1u);
            DrawPhysicalPageDepth(gfxContext, frame, renderRequestIndex, 1u);

            MarkPhysicalPageRendered(gfxContext, renderRequestIndex);
        }

        gfxContext.SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        GeneratePendingPhysicalHZB(gfxContext);
        CommitPendingPhysicalHZB();
    }

    void RenderRequestedPhysicalPageDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame,
        uint32_t renderRequestIndex)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before rendering a physical page.");
        ASSERT(s_PhysicalPagePoolInitialized, "VSM physical page pool must be initialized before rendering.");
        if (s_Views.empty() || DrawCommandManager::GetNumPotentialDrawItems() == 0)
        {
            return;
        }

        ScopedTimer timer(L"VSM: Render Requested Physical Page Depth", gfxContext);
        PreparePhysicalPageRenderResources(gfxContext);
        ResetPhysicalPageCullBuffers(gfxContext);

        const Renderer::HZBResources hzbResources = GetPhysicalHZBResources();
        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            DispatchPhysicalPageCull(gfxContext, frame, hzbResources, renderRequestIndex, passIndex);
            BuildPhysicalPageDrawCommand(gfxContext, frame, passIndex);
            DrawPhysicalPageDepth(gfxContext, frame, renderRequestIndex, passIndex);
            GeneratePendingPhysicalHZB(gfxContext);
        }

        MarkPhysicalPageRendered(gfxContext, renderRequestIndex);
        CommitPendingPhysicalHZB();
    }

    void BindSamplingResources(ComputeContext& context, ProgramBinder& binder)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before binding sampling resources.");

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalClipmapsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ResidencyStatesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(GetCurrentPageTable(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmClipmaps", s_DirectionalClipmapsGpu);
        binder.SetRootBufferSRV("g_VsmResidencyStates", s_ResidencyStatesGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmProjections", s_ProjectionsGpu);
        binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferSRV("g_VsmPageTable", GetCurrentPageTable());
        binder.SetRootBufferSRV("g_VsmPhysicalPageMetadata", s_PhysicalPageMetadataGpu);
    }
} // namespace Renderer::VirtualShadowMap

#include "VirtualShadowMap.h"

#include "Renderer.h"
#include "TemporalEffects.h"
#include "../Core/BufferManager.h"
#include "../Core/DepthBuffer.h"
#include "../Core/EngineProfiling.h"
#include "../Core/GpuBuffer.h"
#include "../Core/PipelineState.h"
#include "../Core/ProgramBinder.h"
#include "../Core/ProgramUtils.h"
#include "../Core/Utility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace Graphics;

namespace Renderer::VirtualShadowMap
{
    namespace
    {
        bool s_Initialized = false;

        ComputePSO s_MarkRequestedPagesPSO(L"VSM: Mark Requested Pages");
        std::shared_ptr<Program> s_MarkRequestedPagesProgram;
        ComputePSO s_ReuseRequestedPagesPSO(L"VSM: Reuse Requested Pages");
        ComputePSO s_BuildFreePhysicalPageListPSO(L"VSM: Build Free Physical Page List");
        ComputePSO s_AllocateNewPagesPSO(L"VSM: Allocate New Pages");
        std::shared_ptr<Program> s_ReuseRequestedPagesProgram;
        std::shared_ptr<Program> s_BuildFreePhysicalPageListProgram;
        std::shared_ptr<Program> s_AllocateNewPagesProgram;

        StructuredBuffer s_ShadowViewsGpu;
        StructuredBuffer s_DirectionalAddressesGpu;
        ByteAddressBuffer s_PageRequestMaskGpu;
        ByteAddressBuffer s_RequestStatisticsGpu;
        std::array<StructuredBuffer, 2> s_PageTablesGpu;
        StructuredBuffer s_PageRenderRequestsGpu;
        StructuredBuffer s_PhysicalPageMetadataGpu;
        StructuredBuffer s_FreePhysicalPagesGpu;
        ByteAddressBuffer s_PhysicalPageUsedMaskGpu;
        ByteAddressBuffer s_PageManagementCountersGpu;
        DepthBuffer s_PhysicalPagePool;

        std::vector<VsmShadowView> s_Views;
        std::vector<VsmShadowView> s_PreviousViews;
        std::vector<DirectionalVsmAddressGpu> s_DirectionalAddressesGpuData;
        uint32_t s_CurrentPageTableIndex = 0;
        bool s_PhysicalPagePoolInitialized = false;

        StructuredBuffer& GetCurrentPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex];
        }

        StructuredBuffer& GetPreviousPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex ^ 1u];
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

        void UploadFrameData(ComputeContext& context)
        {
            ASSERT(!s_Views.empty());
            if (s_Views.empty())
                return;

            context.WriteBuffer(
                s_ShadowViewsGpu,
                0,
                s_Views.data(),
                s_Views.size() * sizeof(VsmShadowView));
            if (!s_DirectionalAddressesGpuData.empty())
            {
                context.WriteBuffer(
                    s_DirectionalAddressesGpu,
                    0,
                    s_DirectionalAddressesGpuData.data(),
                    s_DirectionalAddressesGpuData.size() * sizeof(DirectionalVsmAddressGpu));
            }

            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

        ProgramDesc markRequestedPagesDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MarkVsmPages.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        markRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        markRequestedPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        markRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRequestMask");
        markRequestedPagesDesc.AddRootBufferUAV("g_VsmRequestStatistics");
        s_MarkRequestedPagesProgram = ProgramUtils::GetProgram(
            markRequestedPagesDesc,
            "VSM: Mark Requested Pages");
        if (!s_MarkRequestedPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkRequestedPagesPSO, *s_MarkRequestedPagesProgram);
        s_MarkRequestedPagesPSO.Finalize();

        const std::string pageManagementShaderPath = Renderer::GetModelShaderPath("VsmPageManagement.slang");

        ProgramDesc reuseRequestedPagesDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "reuseRequestedPages");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPreviousPageTable");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPhysicalPageMetadataSRV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmCurrentPageTable");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
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
        s_AllocateNewPagesProgram = ProgramUtils::GetProgram(allocateNewPagesDesc, "VSM: Allocate New Pages");
        if (!s_AllocateNewPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_AllocateNewPagesPSO, *s_AllocateNewPagesProgram);
        s_AllocateNewPagesPSO.Finalize();

        s_ShadowViewsGpu.Create(
            L"VSM Shadow Views",
            kMaxShadowViews,
            sizeof(VsmShadowView));
        s_DirectionalAddressesGpu.Create(
            L"VSM Directional Addresses",
            kMaxShadowViews,
            sizeof(DirectionalVsmAddressGpu));
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
        s_PhysicalPagePool.Create(
            L"VSM Physical Page Pool",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_D32_FLOAT);

        s_Views.reserve(kMaxShadowViews);
        s_PreviousViews.reserve(kMaxShadowViews);
        s_DirectionalAddressesGpuData.reserve(kMaxShadowViews);
        s_CurrentPageTableIndex = 0;
        s_PhysicalPagePoolInitialized = false;
        s_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        s_Views.clear();
        s_PreviousViews.clear();
        s_DirectionalAddressesGpuData.clear();

        s_ShadowViewsGpu.Destroy();
        s_DirectionalAddressesGpu.Destroy();
        s_PageRequestMaskGpu.Destroy();
        s_RequestStatisticsGpu.Destroy();
        s_PageTablesGpu[0].Destroy();
        s_PageTablesGpu[1].Destroy();
        s_PageRenderRequestsGpu.Destroy();
        s_PhysicalPageMetadataGpu.Destroy();
        s_FreePhysicalPagesGpu.Destroy();
        s_PhysicalPageUsedMaskGpu.Destroy();
        s_PageManagementCountersGpu.Destroy();
        s_PhysicalPagePool.Destroy();
        s_MarkRequestedPagesProgram.reset();
        s_ReuseRequestedPagesProgram.reset();
        s_BuildFreePhysicalPageListProgram.reset();
        s_AllocateNewPagesProgram.reset();
        s_CurrentPageTableIndex = 0;
        s_PhysicalPagePoolInitialized = false;
        s_Initialized = false;
    }

    void BeginFrame()
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before BeginFrame.");
        if (!s_Initialized)
            return;

        s_PreviousViews.swap(s_Views);
        s_Views.clear();
        s_DirectionalAddressesGpuData.clear();
        s_CurrentPageTableIndex ^= 1u;
    }

    uint32_t AddDirectionalView(const DirectionalVsmAddressDesc& desc)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before adding views.");
        ASSERT(s_Views.size() < kMaxShadowViews, "Exceeded the VSM shadow-view capacity.");
        ASSERT(s_DirectionalAddressesGpuData.size() < kMaxShadowViews, "Exceeded the directional VSM address capacity.");
        if (!s_Initialized ||
            s_Views.size() >= kMaxShadowViews ||
            s_DirectionalAddressesGpuData.size() >= kMaxShadowViews)
        {
            return kInvalidViewId;
        }

        const DirectionalVsmAddressConstants addressConstants = BuildDirectionalVsmAddressConstants(desc);
        const uint32_t addressDataIndex = static_cast<uint32_t>(s_DirectionalAddressesGpuData.size());
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

        s_Views.push_back(view);
        s_DirectionalAddressesGpuData.push_back(BuildDirectionalVsmAddressGpu(addressConstants));
        return viewId;
    }

    const VsmShadowView& GetView(uint32_t viewId)
    {
        ASSERT(viewId < s_Views.size(), "Invalid VSM shadow-view index.");
        if (viewId >= s_Views.size())
        {
            static const VsmShadowView invalidView{};
            return invalidView;
        }

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

    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before marking pages.");
        ASSERT(s_MarkRequestedPagesProgram != nullptr);
        if (!s_Initialized || !s_MarkRequestedPagesProgram || s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Mark Requested Pages", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();
        UploadFrameData(context);

        const size_t requestMaskBytes = s_Views.size() * kRequestMaskWordCountPerView * sizeof(uint32_t);
        const size_t statisticsBytes = s_Views.size() * VSM_REQUEST_STATISTICS_STRIDE;
        context.ClearBufferUAV(s_PageRequestMaskGpu, requestMaskBytes, 0);
        context.ClearBufferUAV(s_RequestStatisticsGpu, statisticsBytes, 0);
        context.InsertUAVBarrier(s_PageRequestMaskGpu);
        context.InsertUAVBarrier(s_RequestStatisticsGpu);

        context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
        ProgramBinder binder(*s_MarkRequestedPagesProgram, context);
        binder.SetRootSignature();
        context.SetPipelineState(s_MarkRequestedPagesPSO);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferUAV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferUAV("g_VsmRequestStatistics", s_RequestStatisticsGpu);

        const Renderer::ViewConstants& viewConstants = receiverView.GetConstants();
        ProgramVar constants = binder["g_MarkVsmPages"];
        constants["InverseViewProjMatrix"].Set(viewConstants.InverseViewProjMatrix);
        constants["ViewportSize"].Set(
            DirectX::XMUINT2(viewConstants.ViewportWidth, viewConstants.ViewportHeight));

        ProgramVar commonResources = binder["g_CommonResources"];
        commonResources["BindlessResourcesBaseIndex"].Set(Renderer::GetBindlessResourcesBaseOffset());
        commonResources["FrameIndexMod2"].Set(TemporalEffects::GetFrameIndexMod2());

        for (uint32_t viewId = 0; viewId < s_Views.size(); ++viewId)
        {
            constants["ViewId"].Set(viewId);
            binder.Apply();
            context.Dispatch2D(viewConstants.ViewportWidth, viewConstants.ViewportHeight);
        }

        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void AllocateRequestedPages(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before allocating pages.");
        ASSERT(s_ReuseRequestedPagesProgram != nullptr);
        ASSERT(s_BuildFreePhysicalPageListProgram != nullptr);
        ASSERT(s_AllocateNewPagesProgram != nullptr);
        if (!s_Initialized || !s_ReuseRequestedPagesProgram || !s_BuildFreePhysicalPageListProgram ||
            !s_AllocateNewPagesProgram || s_Views.empty())
        {
            return;
        }

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
        context.ClearBufferUAV(currentPageTable, pageTableBytes, kInvalidPhysicalPage);
        context.ClearBufferUAV(s_PhysicalPageUsedMaskGpu, physicalPageUsedMaskBytes, 0);
        context.ClearBufferUAV(s_PageManagementCountersGpu, VSM_PAGE_MANAGEMENT_COUNTERS_SIZE, 0);
        context.InsertUAVBarrier(currentPageTable);
        context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(previousPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PhysicalPageUsedMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        constexpr uint32_t kPageManagementThreadCount = 64;
        constexpr uint32_t kRequestWordGroupCount =
            (kRequestMaskWordCountPerView + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;
        constexpr uint32_t kPhysicalPageGroupCount =
            (kPhysicalPageCapacity + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;

        {
            ProgramBinder binder(*s_ReuseRequestedPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_ReuseRequestedPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferSRV("g_VsmPreviousPageTable", previousPageTable);
            binder.SetRootBufferSRV("g_VsmPhysicalPageMetadataSRV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmCurrentPageTable", currentPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV", s_PhysicalPageUsedMaskGpu);
            binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        context.InsertUAVBarrier(currentPageTable);
        context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        context.InsertUAVBarrier(s_PageRenderRequestsGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);

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
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.FlushResourceBarriers();
    }

    void BindPageRequestDebugResources(ProgramBinder& binder)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before binding request resources.");
        if (!s_Initialized)
            return;

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferSRV("g_VsmPageTable", GetCurrentPageTable());
    }
} // namespace Renderer::VirtualShadowMap

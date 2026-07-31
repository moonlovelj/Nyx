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
        ComputePSO s_AllocateRequestedPagesPSO(L"VSM: Allocate Requested Pages");
        std::shared_ptr<Program> s_AllocateRequestedPagesProgram;

        StructuredBuffer s_ShadowViewsGpu;
        StructuredBuffer s_DirectionalAddressesGpu;
        ByteAddressBuffer s_PageRequestMaskGpu;
        ByteAddressBuffer s_RequestStatisticsGpu;
        StructuredBuffer s_PageTableGpu;
        StructuredBuffer s_PageRenderRequestsGpu;
        ByteAddressBuffer s_AllocationStatisticsGpu;
        DepthBuffer s_PhysicalPagePool;

        std::vector<VsmShadowView> s_Views;
        std::vector<DirectionalVsmAddressGpu> s_DirectionalAddressesGpuData;

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

        ProgramDesc allocateRequestedPagesDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("AllocateVsmPages.slang"),
            "computeMain");
        allocateRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        allocateRequestedPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        allocateRequestedPagesDesc.AddRootBufferUAV("g_VsmPageTable");
        allocateRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        allocateRequestedPagesDesc.AddRootBufferUAV("g_VsmAllocationStatistics");
        s_AllocateRequestedPagesProgram = ProgramUtils::GetProgram(
            allocateRequestedPagesDesc,
            "VSM: Allocate Requested Pages");
        if (!s_AllocateRequestedPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_AllocateRequestedPagesPSO, *s_AllocateRequestedPagesProgram);
        s_AllocateRequestedPagesPSO.Finalize();

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
        s_PageTableGpu.Create(
            L"VSM Page Table",
            kMaxShadowViews * kPagesPerView,
            sizeof(uint32_t));
        s_PageRenderRequestsGpu.Create(
            L"VSM Page Render Requests",
            kPhysicalPageCapacity,
            sizeof(VsmPageRenderRequest));
        s_AllocationStatisticsGpu.Create(
            L"VSM Allocation Statistics",
            1,
            VSM_ALLOCATION_STATISTICS_STRIDE);
        s_PhysicalPagePool.Create(
            L"VSM Physical Page Pool",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_D32_FLOAT);

        s_Views.reserve(kMaxShadowViews);
        s_DirectionalAddressesGpuData.reserve(kMaxShadowViews);
        s_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        s_Views.clear();
        s_DirectionalAddressesGpuData.clear();

        s_ShadowViewsGpu.Destroy();
        s_DirectionalAddressesGpu.Destroy();
        s_PageRequestMaskGpu.Destroy();
        s_RequestStatisticsGpu.Destroy();
        s_PageTableGpu.Destroy();
        s_PageRenderRequestsGpu.Destroy();
        s_AllocationStatisticsGpu.Destroy();
        s_PhysicalPagePool.Destroy();
        s_MarkRequestedPagesProgram.reset();
        s_AllocateRequestedPagesProgram.reset();
        s_Initialized = false;
    }

    void BeginFrame()
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before BeginFrame.");
        if (!s_Initialized)
            return;

        s_Views.clear();
        s_DirectionalAddressesGpuData.clear();
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
        ASSERT(s_AllocateRequestedPagesProgram != nullptr);
        if (!s_Initialized || !s_AllocateRequestedPagesProgram || s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Allocate Requested Pages", gfxContext);

        // The temporary allocator has no cross-frame cache, so every physical page starts empty.
        gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        gfxContext.ClearDepth(s_PhysicalPagePool);

        ComputeContext& context = gfxContext.GetComputeContext();
        const size_t pageTableBytes = s_Views.size() * kPagesPerView * sizeof(uint32_t);
        context.ClearBufferUAV(s_PageTableGpu, pageTableBytes, kInvalidPhysicalPage);
        context.ClearBufferUAV(s_AllocationStatisticsGpu, VSM_ALLOCATION_STATISTICS_STRIDE, 0);
        context.InsertUAVBarrier(s_PageTableGpu);
        context.InsertUAVBarrier(s_AllocationStatisticsGpu);

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageTableGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_AllocationStatisticsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ProgramBinder binder(*s_AllocateRequestedPagesProgram, context);
        binder.SetRootSignature();
        context.SetPipelineState(s_AllocateRequestedPagesPSO);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferUAV("g_VsmPageTable", s_PageTableGpu);
        binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
        binder.SetRootBufferUAV("g_VsmAllocationStatistics", s_AllocationStatisticsGpu);
        binder.Apply();

        constexpr uint32_t kAllocationThreadCount = 64;
        constexpr uint32_t kAllocationGroupCount =
            (kRequestMaskWordCountPerView + kAllocationThreadCount - 1u) / kAllocationThreadCount;
        context.Dispatch(kAllocationGroupCount, static_cast<uint32_t>(s_Views.size()), 1);

        context.TransitionResource(s_PageTableGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_AllocationStatisticsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void BindPageRequestDebugResources(ProgramBinder& binder)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before binding request resources.");
        if (!s_Initialized)
            return;

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferSRV("g_VsmPageTable", s_PageTableGpu);
    }
} // namespace Renderer::VirtualShadowMap

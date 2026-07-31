#include "CommonResources.slang"
#include "ViewCommon.h.slang"
#include "VirtualShadowMap.h.slang"

struct MarkVsmPagesConstants
{
    float4x4 InverseViewProjMatrix;
    uint2 ViewportSize;
    uint ViewId;
    uint Padding;
};

ConstantBuffer<MarkVsmPagesConstants> g_MarkVsmPages;

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
RWByteAddressBuffer g_VsmPageRequestMask;
RWByteAddressBuffer g_VsmRequestStatistics;

void MarkRequestedPage(uint viewId, VsmShadowView shadowView, VsmVirtualAddress address)
{
    uint statisticsBase = viewId * VSM_REQUEST_STATISTICS_STRIDE;
    if (address.InsideWindow == 0u)
    {
        uint originalValue;
        g_VsmRequestStatistics.InterlockedAdd(
            statisticsBase + VSM_OUT_OF_WINDOW_PIXEL_COUNT_OFFSET,
            1u,
            originalValue);
        return;
    }

    if (MarkVsmPageRequested(g_VsmPageRequestMask, shadowView, address.PageTableCoord))
    {
        uint originalValue;
        g_VsmRequestStatistics.InterlockedAdd(
            statisticsBase + VSM_REQUESTED_PAGE_COUNT_OFFSET,
            1u,
            originalValue);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixelPosition = dispatchThreadId.xy;
    if (any(pixelPosition >= g_MarkVsmPages.ViewportSize))
        return;

    Texture2D<float> sceneDepthTexture = GetSceneDepthSRV();
    float sceneDepth = sceneDepthTexture[pixelPosition];

    // The main camera uses reverse Z and clears depth to zero.
    if (sceneDepth == 0.0f)
        return;

    VsmShadowView shadowView = g_VsmShadowViews[g_MarkVsmPages.ViewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return;

    float3 worldPosition = ReconstructWorldPosition(
        pixelPosition,
        sceneDepth,
        g_MarkVsmPages.ViewportSize,
        g_MarkVsmPages.InverseViewProjMatrix);
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    MarkRequestedPage(g_MarkVsmPages.ViewId, shadowView, address);
}

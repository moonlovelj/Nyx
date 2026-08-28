#include "CommonResources.slang"
#include "ViewCommon.h.slang"
#include "VirtualShadowMap.h.slang"

struct MarkVsmPagesConstants
{
    float4x4 InverseViewProjMatrix;
    uint2 ViewportSize;
    uint TargetId;
    uint Padding;
};

ConstantBuffer<MarkVsmPagesConstants> g_MarkVsmPages;

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmClipmapGpu> g_DirectionalVsmClipmaps;
StructuredBuffer<VsmResidencyStateGpu> g_VsmResidencyStates;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<VsmProjectionGpu> g_VsmProjections;
RWByteAddressBuffer g_VsmPageRequestMask;
RWByteAddressBuffer g_VsmRequestStatistics;

void MarkRequestedPage(uint viewId, VsmShadowView shadowView, VsmVirtualAddress address)
{
    uint statisticsBase = viewId * VSM_REQUEST_STATISTICS_STRIDE;
    if (address.Valid == 0u)
    {
        if (shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        {
            uint originalValue;
            g_VsmRequestStatistics.InterlockedAdd(
                statisticsBase + VSM_INVALID_ADDRESS_PIXEL_COUNT_OFFSET,
                1u,
                originalValue);
        }
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

bool TryGetReceiverWorldPosition(uint2 pixelPosition, out float3 worldPosition)
{
    worldPosition = float3(0.0f);
    if (any(pixelPosition >= g_MarkVsmPages.ViewportSize))
        return false;

    Texture2D<float> sceneDepthTexture = GetSceneDepthSRV();
    float sceneDepth = sceneDepthTexture[pixelPosition];

    // The main camera uses reverse Z and clears depth to zero.
    if (sceneDepth == 0.0f)
        return false;

    worldPosition = ReconstructWorldPosition(
        pixelPosition,
        sceneDepth,
        g_MarkVsmPages.ViewportSize,
        g_MarkVsmPages.InverseViewProjMatrix);
    return true;
}

bool TryMarkDirectionalPage(uint viewId, float3 worldPosition)
{
    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    if (address.Valid == 0u)
        return false;

    MarkRequestedPage(viewId, shadowView, address);
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void markDirectionalMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float3 worldPosition;
    if (!TryGetReceiverWorldPosition(dispatchThreadId.xy, worldPosition))
        return;

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[g_MarkVsmPages.TargetId];
    VsmResidencyStateGpu residencyState = g_VsmResidencyStates[clipmap.ResidencyStateIndex];
    uint firstLevel = SelectDirectionalVsmClipmapLevel(worldPosition, clipmap, residencyState.CurrentLodBias);
    uint selectedViewId = clipmap.FirstViewId + firstLevel;
    bool anyPageRequested = TryMarkDirectionalPage(selectedViewId, worldPosition);

    uint coarseViewId = clipmap.FirstViewId + clipmap.LevelCount - 1u;
    if (coarseViewId != selectedViewId)
        anyPageRequested = TryMarkDirectionalPage(coarseViewId, worldPosition) || anyPageRequested;

    if (anyPageRequested)
        return;

    uint statisticsBase = selectedViewId * VSM_REQUEST_STATISTICS_STRIDE;
    uint originalValue;
    g_VsmRequestStatistics.InterlockedAdd(
        statisticsBase + VSM_INVALID_ADDRESS_PIXEL_COUNT_OFFSET,
        1u,
        originalValue);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void markLocalMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float3 worldPosition;
    if (!TryGetReceiverWorldPosition(dispatchThreadId.xy, worldPosition))
        return;

    uint viewId = g_MarkVsmPages.TargetId;
    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE)
        return;

    VsmProjectionGpu projectionData = g_VsmProjections[shadowView.ProjectionDataIndex];
    VsmVirtualAddress address = GetLocalVsmVirtualAddress(worldPosition, projectionData);
    MarkRequestedPage(viewId, shadowView, address);
}

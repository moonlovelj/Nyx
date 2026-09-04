#ifndef NYX_VSM_CULL_H_SLANG
#define NYX_VSM_CULL_H_SLANG

#include "CullCommon.slang"
#include "VirtualShadowMap.h.slang"
#include "../VirtualShadowMapCullShared.h"

StructuredBuffer<VsmShadowView> g_VsmCullShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_VsmCullDirectionalAddresses;
StructuredBuffer<VsmProjectionGpu> g_VsmCullProjections;
StructuredBuffer<uint> g_VsmPageFlags;
StructuredBuffer<uint> g_VsmRasterWindowPageMasks;
StructuredBuffer<uint> g_VsmCullPageTable;
StructuredBuffer<VsmPhysicalPageView> g_VsmCullPhysicalPageViews;

struct VsmCullPageBounds
{
    int2 Min;
    int2 Max;
    bool Valid;
};

uint GetVsmPageFlagsMipDimension(uint mipLevel)
{
    return VSM_PAGE_TABLE_DIM >> mipLevel;
}

uint GetVsmPageFlagsMipOffset(uint mipLevel)
{
    uint offset = 0u;
    uint dimension = VSM_PAGE_TABLE_DIM;

    [unroll]
    for (uint mip = 0u; mip < VSM_PAGE_FLAGS_MIP_COUNT - 1u; ++mip)
    {
        if (mip >= mipLevel)
            break;

        offset += dimension * dimension;
        dimension >>= 1u;
    }

    return offset;
}

uint GetVsmPageFlagsIndex(uint viewId, uint mipLevel, uint2 pageCoord)
{
    uint dimension = GetVsmPageFlagsMipDimension(mipLevel);
    return viewId * VSM_PAGE_FLAGS_NODES_PER_VIEW + GetVsmPageFlagsMipOffset(mipLevel) +
        pageCoord.y * dimension + pageCoord.x;
}

VsmCullPageBounds GetDirectionalVsmCullPageBounds(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    VsmShadowView shadowView)
{
    DirectionalVsmAddressGpu addressData = g_VsmCullDirectionalAddresses[shadowView.AddressDataIndex];
    VsmProjectionGpu projectionData = g_VsmCullProjections[shadowView.ProjectionDataIndex];
    int2 addressOriginPage = addressData.AddressAndWindowOriginPage.xy;
    int2 windowOriginPage = addressData.AddressAndWindowOriginPage.zw;
    float2 addressOriginInWindow = float2(addressOriginPage - windowOriginPage);
    float invWorldUnitsPerPage = addressData.AddressOriginAndInvWorldUnitsPerPage.w;

    float2 pageMin = float2(1e30f);
    float2 pageMax = float2(-1e30f);
    float depthMin = 1e30f;
    float depthMax = -1e30f;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        float3 worldPosition = mul(worldMatrix, BuildAabbCorner(bboxMin, bboxMax, cornerIndex)).xyz;
        float3 relativePosition = worldPosition - addressData.AddressOriginAndInvWorldUnitsPerPage.xyz;
        float2 pagePosition = addressOriginInWindow + float2(
            dot(addressData.WorldToLightRow0.xyz, relativePosition),
            -dot(addressData.WorldToLightRow1.xyz, relativePosition)) * invWorldUnitsPerPage;
        float4 homogeneousPosition = float4(worldPosition, 1.0f);
        float depthW = dot(projectionData.ViewProjRow3, homogeneousPosition);
        float depth = dot(projectionData.ViewProjRow2, homogeneousPosition) / depthW;

        pageMin = min(pageMin, pagePosition);
        pageMax = max(pageMax, pagePosition);
        depthMin = min(depthMin, depth);
        depthMax = max(depthMax, depth);
    }

    const float pageEpsilon = 1e-4f;
    int2 localPageMin = int2(floor(pageMin - pageEpsilon));
    int2 localPageMax = int2(floor(pageMax + pageEpsilon));

    VsmCullPageBounds bounds;
    bounds.Valid = depthMax >= 0.0f && depthMin <= 1.0f &&
        all(localPageMax >= int2(0)) && all(localPageMin < int2(VSM_PAGE_TABLE_DIM));
    bounds.Min = clamp(localPageMin, int2(0), int2(VSM_PAGE_TABLE_DIM - 1u));
    bounds.Max = clamp(localPageMax, int2(0), int2(VSM_PAGE_TABLE_DIM - 1u));
    return bounds;
}

bool DoesVsmPageBoundsContainRequestedPage(uint viewId, VsmCullPageBounds bounds)
{
    if (!bounds.Valid)
        return false;

    uint2 pageCount = uint2(bounds.Max - bounds.Min + 1);
    uint maxPageCount = max(pageCount.x, pageCount.y);
    uint mipLevel = min(uint(firstbithigh(maxPageCount - 1u) + 1), VSM_PAGE_FLAGS_MIP_COUNT - 1u);
    uint2 mipMin = uint2(bounds.Min) >> mipLevel;
    uint2 mipMax = uint2(bounds.Max) >> mipLevel;

    for (uint y = mipMin.y; y <= mipMax.y; ++y)
    {
        for (uint x = mipMin.x; x <= mipMax.x; ++x)
        {
            if (g_VsmPageFlags[GetVsmPageFlagsIndex(viewId, mipLevel, uint2(x, y))] != 0u)
                return true;
        }
    }

    return false;
}

uint GetVsmRasterWindowIndex(uint viewId, uint2 windowCoord)
{
    return viewId * VSM_RASTER_WINDOWS_PER_VIEW + windowCoord.y * VSM_RASTER_WINDOW_DIM + windowCoord.x;
}

uint GetVsmRasterWindowCoverageMask(VsmCullPageBounds bounds, uint2 windowCoord)
{
    int2 windowPageMin = int2(windowCoord << VSM_RASTER_WINDOW_PAGES_LOG2);
    int2 coveredMin = max(bounds.Min - windowPageMin, int2(0));
    int2 coveredMax = min(bounds.Max - windowPageMin, int2(VSM_RASTER_WINDOW_PAGES - 1u));
    uint mask = 0u;

    for (int y = coveredMin.y; y <= coveredMax.y; ++y)
    {
        for (int x = coveredMin.x; x <= coveredMax.x; ++x)
            mask |= 1u << (uint(y) * VSM_RASTER_WINDOW_PAGES + uint(x));
    }

    return mask;
}

uint PackVsmRasterWindow(uint2 windowCoord, uint pageMask)
{
    return ((windowCoord.x & VSM_RASTER_WINDOW_COORD_MASK) << VSM_RASTER_WINDOW_X_SHIFT) |
        ((windowCoord.y & VSM_RASTER_WINDOW_COORD_MASK) << VSM_RASTER_WINDOW_Y_SHIFT) |
        ((pageMask & VSM_RASTER_WINDOW_PAGE_MASK_MASK) << VSM_RASTER_WINDOW_PAGE_MASK_SHIFT);
}

float4x4 GetVsmCullPageViewProj(VsmPhysicalPageView pageView)
{
    return float4x4(
        pageView.ViewProjRow0,
        pageView.ViewProjRow1,
        pageView.ViewProjRow2,
        pageView.ViewProjRow3);
}

float4x4 GetVsmCullPagePrevViewProj(VsmPhysicalPageView pageView)
{
    return float4x4(
        pageView.PrevViewProjRow0,
        pageView.PrevViewProjRow1,
        pageView.PrevViewProjRow2,
        pageView.PrevViewProjRow3);
}

bool HasVsmCullPageHistory(VsmPhysicalPageView pageView)
{
    return (pageView.Flags & VSM_PAGE_RENDER_REQUEST_HISTORY_VALID) != 0u;
}

bool TryGetDirectionalVsmCullPageView(
    uint viewId,
    uint2 localPage,
    out VsmPhysicalPageView pageView)
{
    pageView = {};
    VsmShadowView shadowView = g_VsmCullShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return false;

    DirectionalVsmAddressGpu addressData = g_VsmCullDirectionalAddresses[shadowView.AddressDataIndex];
    int2 globalPage = addressData.AddressAndWindowOriginPage.zw + int2(localPage);
    uint pageTableIndex = shadowView.PageTableBase + GetVsmPageTableIndex(GetVsmPageTableCoord(globalPage));
    uint pageTableEntry = g_VsmCullPageTable[pageTableIndex];
    if (!IsVsmPageTableEntryExact(pageTableEntry))
        return false;

    uint physicalPageIndex = GetVsmPageTablePhysicalPageIndex(pageTableEntry);
    if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return false;

    pageView = g_VsmCullPhysicalPageViews[physicalPageIndex];
    return pageView.PhysicalPageIndex == physicalPageIndex &&
        pageView.ViewId == viewId &&
        all(pageView.VirtualPage == globalPage);
}

bool EvaluateVsmCullPageHzb(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    VsmPhysicalPageView pageView,
    Texture2D<float> hzb,
    SamplerState hzbSampler,
    CullCommonContext context,
    bool usePreviousProjection)
{
    context.HZBUvScaleBias = pageView.PhysicalAtlasUvScaleBias;
    float4x4 viewProjMatrix = usePreviousProjection
        ? GetVsmCullPagePrevViewProj(pageView)
        : GetVsmCullPageViewProj(pageView);

    return EvaluateBoundsVisibility(
        bboxMin,
        bboxMax,
        worldMatrix,
        viewProjMatrix,
        hzb,
        hzbSampler,
        context,
        VSM_PHYSICAL_HZB_MAX_PAGE_MIP);
}

bool IsVsmCullPageVisible(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    uint viewId,
    uint2 localPage,
    uint passIndex,
    Texture2D<float> previousHzb,
    Texture2D<float> currentHzb,
    SamplerState hzbSampler,
    CullCommonContext context,
    bool requirePreviousOcclusion)
{
    VsmPhysicalPageView pageView;
    if (!TryGetDirectionalVsmCullPageView(viewId, localPage, pageView))
        return false;

    if (!HasVsmCullPageHistory(pageView))
        return passIndex == 0u;

    // Pass 1 nodes only test current depth. Final meshlets additionally prove
    // that the page was hidden in the previous HZB to avoid duplicate draws.
    if (passIndex == 0u || requirePreviousOcclusion)
    {
        bool previousVisible = EvaluateVsmCullPageHzb(
            bboxMin,
            bboxMax,
            worldMatrix,
            pageView,
            previousHzb,
            hzbSampler,
            context,
            true);
        if (passIndex == 0u || previousVisible)
            return passIndex == 0u && previousVisible;
    }

    return EvaluateVsmCullPageHzb(
        bboxMin,
        bboxMax,
        worldMatrix,
        pageView,
        currentHzb,
        hzbSampler,
        context,
        false);
}

bool DoesVsmPageBoundsNeedTraversal(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    uint viewId,
    VsmCullPageBounds bounds,
    uint passIndex,
    Texture2D<float> previousHzb,
    Texture2D<float> currentHzb,
    SamplerState hzbSampler,
    CullCommonContext context)
{
    if (!DoesVsmPageBoundsContainRequestedPage(viewId, bounds))
        return false;

    uint2 windowMin = uint2(bounds.Min) >> VSM_RASTER_WINDOW_PAGES_LOG2;
    uint2 windowMax = uint2(bounds.Max) >> VSM_RASTER_WINDOW_PAGES_LOG2;
    // Physical atlas pages are discontinuous. Keep large nodes conservative
    // instead of turning one node test into an unbounded page-table walk.
    if (any(windowMin != windowMax))
        return true;

    uint pageMask = g_VsmRasterWindowPageMasks[GetVsmRasterWindowIndex(viewId, windowMin)] &
        GetVsmRasterWindowCoverageMask(bounds, windowMin);
    while (pageMask != 0u)
    {
        uint pageBitIndex = uint(firstbitlow(pageMask));
        uint2 pageInWindow = uint2(
            pageBitIndex & (VSM_RASTER_WINDOW_PAGES - 1u),
            pageBitIndex >> VSM_RASTER_WINDOW_PAGES_LOG2);
        uint2 localPage = (windowMin << VSM_RASTER_WINDOW_PAGES_LOG2) + pageInWindow;
        if (IsVsmCullPageVisible(
                bboxMin,
                bboxMax,
                worldMatrix,
                viewId,
                localPage,
                passIndex,
                previousHzb,
                currentHzb,
                hzbSampler,
                context,
                false))
        {
            return true;
        }

        pageMask &= pageMask - 1u;
    }

    return false;
}

uint FilterVsmRasterPageMaskByHzb(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    uint viewId,
    uint2 windowCoord,
    uint pageMask,
    uint passIndex,
    Texture2D<float> previousHzb,
    Texture2D<float> currentHzb,
    SamplerState hzbSampler,
    CullCommonContext context)
{
    uint visiblePageMask = 0u;
    while (pageMask != 0u)
    {
        uint pageBitIndex = uint(firstbitlow(pageMask));
        uint pageBit = 1u << pageBitIndex;
        uint2 pageInWindow = uint2(
            pageBitIndex & (VSM_RASTER_WINDOW_PAGES - 1u),
            pageBitIndex >> VSM_RASTER_WINDOW_PAGES_LOG2);
        uint2 localPage = (windowCoord << VSM_RASTER_WINDOW_PAGES_LOG2) + pageInWindow;
        if (IsVsmCullPageVisible(
                bboxMin,
                bboxMax,
                worldMatrix,
                viewId,
                localPage,
                passIndex,
                previousHzb,
                currentHzb,
                hzbSampler,
                context,
                true))
        {
            visiblePageMask |= pageBit;
        }

        pageMask &= pageMask - 1u;
    }

    return visiblePageMask;
}

#endif

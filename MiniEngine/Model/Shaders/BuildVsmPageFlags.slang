#include "VsmCull.h.slang"

struct BuildVsmPageFlagsConstants
{
    uint MipLevel;
};

ConstantBuffer<BuildVsmPageFlagsConstants> g_BuildVsmPageFlags;

StructuredBuffer<uint> g_VsmActiveViews;
ByteAddressBuffer g_VsmPhysicalPageRenderCounters;
ByteAddressBuffer g_VsmPageRenderMask;
RWStructuredBuffer<uint> g_VsmPageFlagsUAV;
RWStructuredBuffer<uint> g_VsmRasterWindowPageMasksUAV;

bool TryGetActiveDirectionalView(uint activeViewIndex, out uint viewId, out VsmShadowView shadowView)
{
    uint activeViewCount = g_VsmPhysicalPageRenderCounters.Load(VSM_ACTIVE_VIEW_COUNT_OFFSET);
    if (activeViewIndex >= activeViewCount)
    {
        viewId = 0u;
        shadowView = {};
        return false;
    }

    viewId = g_VsmActiveViews[activeViewIndex];
    shadowView = g_VsmCullShadowViews[viewId];
    return shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void buildMip0(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 localPage = dispatchThreadId.xy;
    if (any(localPage >= uint2(VSM_PAGE_TABLE_DIM)))
        return;

    uint viewId;
    VsmShadowView shadowView;
    if (!TryGetActiveDirectionalView(dispatchThreadId.z, viewId, shadowView))
        return;

    DirectionalVsmAddressGpu addressData = g_VsmCullDirectionalAddresses[shadowView.AddressDataIndex];
    int2 globalPage = addressData.AddressAndWindowOriginPage.zw + int2(localPage);
    uint requested = IsVsmPageInMask(g_VsmPageRenderMask, shadowView, GetVsmPageTableCoord(globalPage)) ? 1u : 0u;
    g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, 0u, localPage)] = requested;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void buildMip(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint mipLevel = g_BuildVsmPageFlags.MipLevel;
    uint dimension = GetVsmPageFlagsMipDimension(mipLevel);
    uint2 pageCoord = dispatchThreadId.xy;
    if (any(pageCoord >= uint2(dimension)))
        return;

    uint viewId;
    VsmShadowView shadowView;
    if (!TryGetActiveDirectionalView(dispatchThreadId.z, viewId, shadowView))
        return;

    uint2 childCoord = pageCoord << 1u;
    uint childMip = mipLevel - 1u;
    uint flags = 0u;
    flags |= g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, childMip, childCoord)];
    flags |= g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, childMip, childCoord + uint2(1u, 0u))];
    flags |= g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, childMip, childCoord + uint2(0u, 1u))];
    flags |= g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, childMip, childCoord + uint2(1u, 1u))];
    g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, mipLevel, pageCoord)] = flags;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void buildRasterWindowMasks(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 windowCoord = dispatchThreadId.xy;
    if (any(windowCoord >= uint2(VSM_RASTER_WINDOW_DIM)))
        return;

    uint viewId;
    VsmShadowView shadowView;
    if (!TryGetActiveDirectionalView(dispatchThreadId.z, viewId, shadowView))
        return;

    uint2 pageBase = windowCoord << VSM_RASTER_WINDOW_PAGES_LOG2;
    uint requestedPageMask = 0u;

    [unroll]
    for (uint y = 0u; y < VSM_RASTER_WINDOW_PAGES; ++y)
    {
        [unroll]
        for (uint x = 0u; x < VSM_RASTER_WINDOW_PAGES; ++x)
        {
            uint pageFlag = g_VsmPageFlagsUAV[GetVsmPageFlagsIndex(viewId, 0u, pageBase + uint2(x, y))];
            requestedPageMask |= (pageFlag & 1u) << (y * VSM_RASTER_WINDOW_PAGES + x);
        }
    }

    g_VsmRasterWindowPageMasksUAV[GetVsmRasterWindowIndex(viewId, windowCoord)] = requestedPageMask;
}

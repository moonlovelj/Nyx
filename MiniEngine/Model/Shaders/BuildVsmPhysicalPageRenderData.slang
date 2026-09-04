#include "VirtualShadowMap.h.slang"
#include "../VirtualShadowMapCullShared.h"

struct BuildVsmPhysicalPageRenderDataConstants
{
    uint ViewCount;
};

ConstantBuffer<BuildVsmPhysicalPageRenderDataConstants> g_BuildVsmPhysicalPageRenderData;

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<VsmProjectionGpu> g_VsmProjections;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
ByteAddressBuffer g_VsmPageManagementCounters;
RWStructuredBuffer<VsmPhysicalPageView> g_VsmPhysicalPageViews;
RWByteAddressBuffer g_VsmPageRenderMask;
RWByteAddressBuffer g_VsmActiveViewMask;
RWStructuredBuffer<uint> g_VsmActiveViews;
RWByteAddressBuffer g_VsmPhysicalPageRenderCounters;

void CountInvalidRenderRequest()
{
    uint unused;
    g_VsmPhysicalPageRenderCounters.InterlockedAdd(VSM_INVALID_RENDER_REQUEST_COUNT_OFFSET, 1u, unused);
}

void AddActiveView(uint viewId)
{
    uint wordByteAddress = (viewId >> VSM_REQUEST_MASK_WORD_LOG2) * sizeof(uint);
    uint viewBit = 1u << (viewId & VSM_REQUEST_MASK_WORD_BIT_MASK);

    uint previousMask;
    g_VsmActiveViewMask.InterlockedOr(wordByteAddress, viewBit, previousMask);
    if ((previousMask & viewBit) != 0u)
        return;

    uint activeViewIndex;
    g_VsmPhysicalPageRenderCounters.InterlockedAdd(VSM_ACTIVE_VIEW_COUNT_OFFSET, 1u, activeViewIndex);
    g_VsmActiveViews[activeViewIndex] = viewId;
}

bool ResolveVirtualPage(
    VsmShadowView shadowView,
    uint pageTableIndex,
    out int2 virtualPage,
    out int2 projectionPageCoord)
{
    if (shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
    {
        DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
        virtualPage = GetDirectionalVsmGlobalPage(pageTableIndex, addressData);
        projectionPageCoord = virtualPage - addressData.AddressAndWindowOriginPage.xy;
        return true;
    }

    if (shadowView.AddressType == VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE)
    {
        virtualPage = int2(GetVsmPageTableCoord(pageTableIndex));
        projectionPageCoord = virtualPage;
        return true;
    }

    virtualPage = int2(0);
    projectionPageCoord = int2(0);
    return false;
}

VsmPhysicalPageView BuildPhysicalPageView(
    VsmPageRenderRequest request,
    VsmShadowView shadowView,
    int2 virtualPage,
    int2 projectionPageCoord)
{
    VsmProjectionGpu projectionData = g_VsmProjections[shadowView.ProjectionDataIndex];
    uint2 physicalPageCoord = GetVsmPhysicalPageCoord(request.PhysicalPageIndex);
    VsmProjectionRows pageProjection =
        BuildVsmPageProjection(GetVsmProjectionRows(projectionData), projectionPageCoord);

    VsmPhysicalPageView pageView;
    pageView.ViewProjRow0 = pageProjection.Row0;
    pageView.ViewProjRow1 = pageProjection.Row1;
    pageView.ViewProjRow2 = pageProjection.Row2;
    pageView.ViewProjRow3 = pageProjection.Row3;

    if ((request.Flags & VSM_PAGE_RENDER_REQUEST_HISTORY_VALID) != 0u)
    {
        VsmPhysicalPageView history = g_VsmPhysicalPageViews[request.PhysicalPageIndex];
        pageView.PrevViewProjRow0 = history.PrevViewProjRow0;
        pageView.PrevViewProjRow1 = history.PrevViewProjRow1;
        pageView.PrevViewProjRow2 = history.PrevViewProjRow2;
        pageView.PrevViewProjRow3 = history.PrevViewProjRow3;
    }
    else
    {
        pageView.PrevViewProjRow0 = pageProjection.Row0;
        pageView.PrevViewProjRow1 = pageProjection.Row1;
        pageView.PrevViewProjRow2 = pageProjection.Row2;
        pageView.PrevViewProjRow3 = pageProjection.Row3;
    }

    float physicalAtlasUvScale = 1.0f / float(VSM_PHYSICAL_POOL_DIM_PAGES);
    pageView.PhysicalAtlasUvScaleBias =
        float4(float2(physicalAtlasUvScale), float2(physicalPageCoord) * physicalAtlasUvScale);
    pageView.ViewerPositionAndProjectionType = projectionData.ViewerPositionAndProjectionType;
    pageView.VirtualPage = virtualPage;
    pageView.PhysicalPageIndex = request.PhysicalPageIndex;
    pageView.Flags = request.Flags;
    pageView.PhysicalPageCoord = physicalPageCoord;
    pageView.ViewId = request.ViewId;
    pageView.LodScale = shadowView.LodScale;
    return pageView;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint renderRequestIndex = dispatchThreadId.x;
    uint renderRequestCount = g_VsmPageManagementCounters.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (renderRequestIndex >= renderRequestCount)
        return;

    VsmPageRenderRequest request = g_VsmPageRenderRequests[renderRequestIndex];
    if (request.ViewId >= g_BuildVsmPhysicalPageRenderData.ViewCount ||
        request.PageTableIndex >= VSM_PAGES_PER_VIEW ||
        request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
    {
        CountInvalidRenderRequest();
        return;
    }

    VsmShadowView shadowView = g_VsmShadowViews[request.ViewId];
    int2 virtualPage;
    int2 projectionPageCoord;
    if (!ResolveVirtualPage(shadowView, request.PageTableIndex, virtualPage, projectionPageCoord))
    {
        CountInvalidRenderRequest();
        return;
    }

    g_VsmPhysicalPageViews[request.PhysicalPageIndex] =
        BuildPhysicalPageView(request, shadowView, virtualPage, projectionPageCoord);

    uint unused;
    g_VsmPhysicalPageRenderCounters.InterlockedAdd(VSM_RENDER_PAGE_COUNT_OFFSET, 1u, unused);
    if (MarkVsmPageInMask(g_VsmPageRenderMask, shadowView, GetVsmPageTableCoord(request.PageTableIndex)))
    {
        g_VsmPhysicalPageRenderCounters.InterlockedAdd(VSM_RENDER_MASK_PAGE_COUNT_OFFSET, 1u, unused);
    }
    AddActiveView(request.ViewId);
}

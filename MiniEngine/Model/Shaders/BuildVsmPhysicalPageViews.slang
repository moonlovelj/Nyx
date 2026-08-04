#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<VsmProjectionGpu> g_VsmProjections;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
ByteAddressBuffer g_VsmPageManagementCounters;
RWStructuredBuffer<VsmPhysicalPageView> g_VsmPhysicalPageViews;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint renderRequestIndex = dispatchThreadId.x;
    uint renderRequestCount = g_VsmPageManagementCounters.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (renderRequestIndex >= renderRequestCount)
        return;

    VsmPageRenderRequest request = g_VsmPageRenderRequests[renderRequestIndex];
    VsmShadowView shadowView = g_VsmShadowViews[request.ViewId];
    VsmProjectionGpu projectionData = g_VsmProjections[shadowView.ProjectionDataIndex];

    int2 virtualPage;
    int2 projectionPageCoord;
    if (shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
    {
        DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
        virtualPage = GetDirectionalVsmGlobalPage(request.PageTableIndex, addressData);
        projectionPageCoord = virtualPage - addressData.AddressAndWindowOriginPage.xy;
    }
    else if (shadowView.AddressType == VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE)
    {
        virtualPage = int2(GetVsmPageTableCoord(request.PageTableIndex));
        projectionPageCoord = virtualPage;
    }
    else
    {
        return;
    }

    uint2 physicalPageCoord = GetVsmPhysicalPageCoord(request.PhysicalPageIndex);

    VsmProjectionRows pageProjection =
        BuildVsmPageProjection(GetVsmProjectionRows(projectionData), projectionPageCoord);
    VsmProjectionRows previousPageProjection =
        BuildVsmPageProjection(GetPreviousVsmProjectionRows(projectionData), projectionPageCoord);

    VsmPhysicalPageView pageView;
    pageView.ViewProjRow0 = pageProjection.Row0;
    pageView.ViewProjRow1 = pageProjection.Row1;
    pageView.ViewProjRow2 = pageProjection.Row2;
    pageView.ViewProjRow3 = pageProjection.Row3;

    pageView.PrevViewProjRow0 = previousPageProjection.Row0;
    pageView.PrevViewProjRow1 = previousPageProjection.Row1;
    pageView.PrevViewProjRow2 = previousPageProjection.Row2;
    pageView.PrevViewProjRow3 = previousPageProjection.Row3;

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

    g_VsmPhysicalPageViews[request.PhysicalPageIndex] = pageView;
}

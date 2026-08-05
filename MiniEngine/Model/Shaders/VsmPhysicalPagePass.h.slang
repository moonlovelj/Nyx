#ifndef NYX_VSM_PHYSICAL_PAGE_PASS_H_SLANG
#define NYX_VSM_PHYSICAL_PAGE_PASS_H_SLANG

#include "VirtualShadowMap.h.slang"

struct VsmPhysicalPagePassConstants
{
    uint RenderRequestIndex;
};

ConstantBuffer<VsmPhysicalPagePassConstants> g_VsmPhysicalPagePass;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
ByteAddressBuffer g_VsmPageManagementCounters;
StructuredBuffer<VsmPhysicalPageView> g_VsmPhysicalPageViews;

bool TryGetVsmPhysicalPageView(out VsmPhysicalPageView pageView)
{
    pageView = {};
    uint renderRequestCount = g_VsmPageManagementCounters.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (g_VsmPhysicalPagePass.RenderRequestIndex >= renderRequestCount)
        return false;

    VsmPageRenderRequest request = g_VsmPageRenderRequests[g_VsmPhysicalPagePass.RenderRequestIndex];
    if (request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return false;

    pageView = g_VsmPhysicalPageViews[request.PhysicalPageIndex];
    return pageView.PhysicalPageIndex == request.PhysicalPageIndex;
}

float4x4 GetVsmPhysicalPageViewProj(VsmPhysicalPageView pageView)
{
    return float4x4(
        pageView.ViewProjRow0,
        pageView.ViewProjRow1,
        pageView.ViewProjRow2,
        pageView.ViewProjRow3);
}

float4x4 GetVsmPhysicalPagePrevViewProj(VsmPhysicalPageView pageView)
{
    return float4x4(
        pageView.PrevViewProjRow0,
        pageView.PrevViewProjRow1,
        pageView.PrevViewProjRow2,
        pageView.PrevViewProjRow3);
}

bool HasVsmPhysicalPageHistory(VsmPhysicalPageView pageView)
{
    return (pageView.Flags & VSM_PAGE_RENDER_REQUEST_HISTORY_VALID) != 0u;
}

float4 TransformVsmPageClipToPhysicalAtlas(VsmPhysicalPageView pageView, float4 pageClipPosition)
{
    float2 pageUvTimesW = pageClipPosition.xy * float2(0.5f, -0.5f) + 0.5f * pageClipPosition.w;
    float2 atlasUvTimesW = pageUvTimesW * pageView.PhysicalAtlasUvScaleBias.xy +
        pageView.PhysicalAtlasUvScaleBias.zw * pageClipPosition.w;

    return float4(
        atlasUvTimesW * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f) * pageClipPosition.w,
        pageClipPosition.z,
        pageClipPosition.w);
}

float4 GetVsmPageClipDistances(float4 pageClipPosition)
{
    return pageClipPosition.w + float4(
        pageClipPosition.x,
        -pageClipPosition.x,
        pageClipPosition.y,
        -pageClipPosition.y);
}

#endif

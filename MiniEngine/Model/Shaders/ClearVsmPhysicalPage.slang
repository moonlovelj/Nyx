#include "VirtualShadowMap.h.slang"

struct ClearVsmPhysicalPageConstants
{
    uint RenderRequestIndex;
};

ConstantBuffer<ClearVsmPhysicalPageConstants> g_ClearVsmPhysicalPage;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
ByteAddressBuffer g_VsmPageManagementCounters;

struct VertexOutput
{
    float4 Position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 quadPositions[6] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f)
    };

    VertexOutput output;
    uint renderRequestCount = g_VsmPageManagementCounters.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (g_ClearVsmPhysicalPage.RenderRequestIndex >= renderRequestCount)
    {
        output.Position = float4(-2.0f, -2.0f, 0.0f, 1.0f);
        return output;
    }

    VsmPageRenderRequest request = g_VsmPageRenderRequests[g_ClearVsmPhysicalPage.RenderRequestIndex];
    if (request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
    {
        output.Position = float4(-2.0f, -2.0f, 0.0f, 1.0f);
        return output;
    }

    float2 pagePosition = float2(GetVsmPhysicalPageCoord(request.PhysicalPageIndex)) + quadPositions[vertexId];
    float2 atlasUv = pagePosition / float(VSM_PHYSICAL_POOL_DIM_PAGES);
    output.Position = float4(atlasUv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

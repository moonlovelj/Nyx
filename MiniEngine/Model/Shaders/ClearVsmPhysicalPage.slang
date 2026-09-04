#include "VirtualShadowMap.h.slang"

struct ClearVsmPhysicalPageConstants
{
    uint FirstRenderRequestIndex;
    uint AddressType;
};

ConstantBuffer<ClearVsmPhysicalPageConstants> g_ClearVsmPhysicalPage;
StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
ByteAddressBuffer g_VsmPageManagementCounters;

struct VertexOutput
{
    float4 Position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
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
    uint renderRequestIndex = g_ClearVsmPhysicalPage.FirstRenderRequestIndex + instanceId;
    uint renderRequestCount = g_VsmPageManagementCounters.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (renderRequestIndex >= renderRequestCount)
    {
        output.Position = float4(-2.0f, -2.0f, 0.0f, 1.0f);
        return output;
    }

    VsmPageRenderRequest request = g_VsmPageRenderRequests[renderRequestIndex];
    bool addressTypeMatches = g_ClearVsmPhysicalPage.AddressType == VSM_ADDRESS_TYPE_INVALID ||
        g_VsmShadowViews[request.ViewId].AddressType == g_ClearVsmPhysicalPage.AddressType;
    if (request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY || !addressTypeMatches)
    {
        output.Position = float4(-2.0f, -2.0f, 0.0f, 1.0f);
        return output;
    }

    float2 pagePosition = float2(GetVsmPhysicalPageCoord(request.PhysicalPageIndex)) + quadPositions[vertexId];
    float2 atlasUv = pagePosition / float(VSM_PHYSICAL_POOL_DIM_PAGES);
    output.Position = float4(atlasUv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

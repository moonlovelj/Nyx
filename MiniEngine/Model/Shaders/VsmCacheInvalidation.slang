#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<VsmInvalidationBoundsGpu> g_VsmInvalidationBounds;
RWStructuredBuffer<VsmInvalidationPageRange> g_VsmInvalidationPageRangesUAV;

struct VsmInvalidationConstants
{
    uint BoundsCount;
};

ConstantBuffer<VsmInvalidationConstants> g_VsmInvalidation;

[shader("compute")]
[numthreads(64, 1, 1)]
void buildInvalidationPageRanges(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_VsmInvalidation.BoundsCount)
        return;

    VsmShadowView view = g_VsmShadowViews[id.y];
    VsmInvalidationPageRange range;
    // Until local-light coverage is implemented, conservatively invalidate that view.
    range.MinVirtualPage = int2(-2147483647);
    range.MaxVirtualPage = int2(2147483647);
    if (view.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
    {
        VsmInvalidationBoundsGpu bounds = g_VsmInvalidationBounds[id.x];
        DirectionalVsmAddressGpu address = g_DirectionalVsmAddresses[view.AddressDataIndex];
        float3 center = bounds.MinWS.xyz * 0.5f + bounds.MaxWS.xyz * 0.5f;
        float3 extent = (bounds.MaxWS.xyz - bounds.MinWS.xyz) * 0.5f;
        float3 relativeCenter = center - address.AddressOriginAndInvWorldUnitsPerPage.xyz;
        float scale = address.AddressOriginAndInvWorldUnitsPerPage.w;
        float2 pageCenter = float2(dot(address.WorldToLightRow0.xyz, relativeCenter),
            -dot(address.WorldToLightRow1.xyz, relativeCenter)) * scale;
        float2 pageExtent = float2(dot(abs(address.WorldToLightRow0.xyz), extent),
            dot(abs(address.WorldToLightRow1.xyz), extent)) * scale;

        // Keep global identity: a cached page can be outside the current window.
        // Include touching boundaries conservatively and do not reject along light Z.
        int2 originPage = address.AddressAndWindowOriginPage.xy;
        range.MinVirtualPage = originPage + int2(floor(pageCenter - pageExtent - 1e-4f));
        range.MaxVirtualPage = originPage + int2(floor(pageCenter + pageExtent + 1e-4f));
    }
    g_VsmInvalidationPageRangesUAV[id.y * g_VsmInvalidation.BoundsCount + id.x] = range;
}

#ifndef NYX_VSM_SAMPLING_H_SLANG
#define NYX_VSM_SAMPLING_H_SLANG

#include "CommonResources.slang"
#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmClipmapGpu> g_DirectionalVsmClipmaps;
StructuredBuffer<VsmResidencyStateGpu> g_VsmResidencyStates;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<VsmProjectionGpu> g_VsmProjections;
StructuredBuffer<uint> g_VsmPageTable;
StructuredBuffer<VsmPhysicalPageMetadata> g_VsmPhysicalPageMetadata;

struct VsmSamplingConstants
{
    uint MinReceiverBiasUlp;
    float ReceiverPlaneBiasScale;
    float MaxReceiverBiasTexels;
    uint Padding;
};

ConstantBuffer<VsmSamplingConstants> g_VsmSampling;

float GetVsmDepthUlp(float depth)
{
    return asfloat(asuint(depth) + 1u) - depth;
}

float GetDirectionalVsmReceiverBias(
    float receiverDepth,
    float3 geometricNormalWS,
    float2 texelPositionInPage,
    DirectionalVsmAddressGpu addressData,
    VsmProjectionGpu projection)
{
    float2 offsetTexel = floor(texelPositionInPage) + 0.5f - texelPositionInPage;
    float worldUnitsPerTexel = rcp(
        addressData.AddressOriginAndInvWorldUnitsPerPage.w * float(VSM_PAGE_SIZE));

    float3 normalLS = float3(
        dot(addressData.WorldToLightRow0.xyz, geometricNormalWS),
        dot(addressData.WorldToLightRow1.xyz, geometricNormalWS),
        dot(addressData.WorldToLightRow2.xyz, geometricNormalWS));
    float safeNormalZ = (normalLS.z < 0.0f ? -1.0f : 1.0f) * max(abs(normalLS.z), 1e-3f);
    float deltaLightZ = worldUnitsPerTexel *
        (-normalLS.x * offsetTexel.x + normalLS.y * offsetTexel.y) / safeNormalZ;

    float depthPerWorldUnit = length(projection.ViewProjRow2.xyz);
    float maxSlopeBiasDepth = worldUnitsPerTexel *
        g_VsmSampling.MaxReceiverBiasTexels * depthPerWorldUnit;
    float slopeBiasDepth = min(
        max(deltaLightZ, 0.0f) * g_VsmSampling.ReceiverPlaneBiasScale * depthPerWorldUnit,
        maxSlopeBiasDepth);
    float precisionBiasDepth = GetVsmDepthUlp(receiverDepth) *
        float(g_VsmSampling.MinReceiverBiasUlp);
    return max(precisionBiasDepth, slopeBiasDepth);
}

bool IsVsmPhysicalPageReady(
    VsmPhysicalPageMetadata metadata,
    VsmShadowView shadowView,
    int2 virtualPage)
{
    bool isRendered = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_RENDERED) != 0u;
    bool isDirty = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_DIRTY) != 0u;
    return IsVsmPhysicalPageMetadataMatch(metadata, shadowView, virtualPage) && isRendered && !isDirty;
}

uint SelectResidentDirectionalVsmClipmapLevel(float3 worldPosition, DirectionalVsmClipmapGpu clipmap)
{
    float lodBias = g_VsmResidencyStates[clipmap.ResidencyStateIndex].CurrentLodBias;
    return SelectDirectionalVsmClipmapLevel(worldPosition, clipmap, lodBias);
}

bool TrySampleDirectionalVsmView(
    float3 worldPosition,
    float3 geometricNormalWS,
    uint viewId,
    out float shadow)
{
    shadow = 1.0f;
    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return false;

    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    float2 texelPositionInPage;
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(
        worldPosition,
        addressData,
        texelPositionInPage);
    if (address.Valid == 0u)
        return false;

    uint physicalPageIndex = GetVsmPhysicalPageIndex(g_VsmPageTable, shadowView, address.PageTableCoord);
    if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return false;

    VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadata[physicalPageIndex];
    if (!IsVsmPhysicalPageReady(metadata, shadowView, address.VirtualPage))
        return false;

    VsmProjectionGpu projection = g_VsmProjections[shadowView.ProjectionDataIndex];
    float4 clipPosition = ProjectWorldToVsmClip(GetVsmProjectionRows(projection), worldPosition);
    if (clipPosition.w <= 0.0f)
        return false;

    float receiverDepth = clipPosition.z / clipPosition.w;
    if (receiverDepth < 0.0f || receiverDepth > 1.0f)
        return false;

    float receiverBias = GetDirectionalVsmReceiverBias(
        receiverDepth,
        geometricNormalWS,
        texelPositionInPage,
        addressData,
        projection);

    uint2 atlasTexel = GetVsmPhysicalPageCoord(physicalPageIndex) * VSM_PAGE_SIZE + address.TexelInPage;
    float shadowDepth = GetVsmPhysicalPagePoolSRV().Load(int3(atlasTexel, 0));

    // The VSM uses reverse Z: larger depth values are closer to the light.
    shadow = receiverDepth + receiverBias >= shadowDepth ? 1.0f : 0.0f;
    return true;
}

float SampleDirectionalVsmClipmapHard(
    float3 worldPosition,
    float3 geometricNormalWS,
    uint clipmapId)
{
    if (clipmapId == VSM_INVALID_VIEW_ID)
        return 1.0f;

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    uint firstLevel = SelectResidentDirectionalVsmClipmapLevel(worldPosition, clipmap);

    float shadow;
    if (TrySampleDirectionalVsmView(
            worldPosition,
            geometricNormalWS,
            clipmap.FirstViewId + firstLevel,
            shadow))
    {
        return shadow;
    }

    uint coarseLevel = clipmap.LevelCount - 1u;
    if (firstLevel != coarseLevel && TrySampleDirectionalVsmView(
            worldPosition,
            geometricNormalWS,
            clipmap.FirstViewId + coarseLevel,
            shadow))
    {
        return shadow;
    }

    return 1.0f;
}

#endif

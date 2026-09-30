#ifndef NYX_VSM_SAMPLING_H_SLANG
#define NYX_VSM_SAMPLING_H_SLANG

#include "CommonResources.slang"
#include "VirtualShadowMap.h.slang"
#include "VsmScreenRayTrace.h.slang"

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
    uint EnableScreenRayTrace;

    float ScreenRayLengthScale;
    uint EnableDirectionalSmrt;
    uint SmrtRayCount;
    uint SmrtSamplesPerRay;

    float SmrtRayLengthScale;
    uint SmrtFrameIndex;
    uint SmrtFreezeNoise;
    float NormalBiasTexels;
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
    VsmProjectionGpu projection,
    float depthPerWorldUnit)
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

struct DirectionalVsmDepthQuery
{
    float ReferenceDepth;
    float ShadowDepth;
    float DepthPerWorldUnit;
    float ReferenceRayDistance;
    float ShadowRayDistance;
    float2 TexelPositionInPage;
    uint ViewId;
};

bool TryQueryDirectionalVsmView(
    float3 worldPosition,
    float3 rayOriginWS,
    uint viewId,
    out DirectionalVsmDepthQuery query)
{
    query.ReferenceDepth = 0.0f;
    query.ShadowDepth = 0.0f;
    query.DepthPerWorldUnit = 0.0f;
    query.ReferenceRayDistance = 0.0f;
    query.ShadowRayDistance = 0.0f;
    query.TexelPositionInPage = float2(0.0f);
    query.ViewId = viewId;
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

    query.DepthPerWorldUnit = length(projection.ViewProjRow2.xyz);
    if (query.DepthPerWorldUnit <= 0.0f)
        return false;

    query.ReferenceDepth = receiverDepth;
    query.TexelPositionInPage = texelPositionInPage;
    uint2 atlasTexel = GetVsmPhysicalPageCoord(physicalPageIndex) * VSM_PAGE_SIZE + address.TexelInPage;
    query.ShadowDepth = GetVsmPhysicalPagePoolSRV().Load(int3(atlasTexel, 0));
    float3 lightAxis = projection.ViewProjRow2.xyz / query.DepthPerWorldUnit;
    query.ReferenceRayDistance = dot(lightAxis, worldPosition - rayOriginWS);
    query.ShadowRayDistance = query.ReferenceRayDistance +
        (query.ShadowDepth - query.ReferenceDepth) / query.DepthPerWorldUnit;
    return true;
}

bool TryQueryDirectionalVsmReceiverView(
    float3 worldPosition,
    float3 geometricNormalWS,
    uint viewId,
    out DirectionalVsmDepthQuery query,
    out float3 queryPositionWS)
{
    queryPositionWS = worldPosition;
    if (g_VsmSampling.NormalBiasTexels > 0.0f)
    {
        VsmShadowView shadowView = g_VsmShadowViews[viewId];
        DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
        float worldUnitsPerTexel = rcp(
            addressData.AddressOriginAndInvWorldUnitsPerPage.w * float(VSM_PAGE_SIZE));
        queryPositionWS += geometricNormalWS * (g_VsmSampling.NormalBiasTexels * worldUnitsPerTexel);

        if (TryQueryDirectionalVsmView(queryPositionWS, queryPositionWS, viewId, query))
            return true;
    }

    // The shifted position may cross into a page that has not been requested.
    queryPositionWS = worldPosition;
    return TryQueryDirectionalVsmView(worldPosition, worldPosition, viewId, query);
}

bool TryQueryDirectionalVsmClipmap(
    float3 worldPosition,
    float3 rayOriginWS,
    DirectionalVsmClipmapGpu clipmap,
    uint preferredLevel,
    out DirectionalVsmDepthQuery query)
{
    if (TryQueryDirectionalVsmView(
            worldPosition,
            rayOriginWS,
            clipmap.FirstViewId + preferredLevel,
            query))
        return true;

    uint coarseLevel = clipmap.LevelCount - 1u;
    return preferredLevel != coarseLevel &&
        TryQueryDirectionalVsmView(
            worldPosition,
            rayOriginWS,
            clipmap.FirstViewId + coarseLevel,
            query);
}

bool TryQueryDirectionalVsmReceiverClipmap(
    float3 worldPosition,
    float3 geometricNormalWS,
    DirectionalVsmClipmapGpu clipmap,
    uint preferredLevel,
    out DirectionalVsmDepthQuery query,
    out float3 queryPositionWS)
{
    if (TryQueryDirectionalVsmReceiverView(
            worldPosition,
            geometricNormalWS,
            clipmap.FirstViewId + preferredLevel,
            query,
            queryPositionWS))
        return true;

    uint coarseLevel = clipmap.LevelCount - 1u;
    return preferredLevel != coarseLevel &&
        TryQueryDirectionalVsmReceiverView(
            worldPosition,
            geometricNormalWS,
            clipmap.FirstViewId + coarseLevel,
            query,
            queryPositionWS);
}

float GetDirectionalVsmQueryReceiverBias(
    DirectionalVsmDepthQuery query,
    float3 geometricNormalWS)
{
    VsmShadowView shadowView = g_VsmShadowViews[query.ViewId];
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    float receiverBias = GetDirectionalVsmReceiverBias(
        query.ReferenceDepth,
        geometricNormalWS,
        query.TexelPositionInPage,
        addressData,
        g_VsmProjections[shadowView.ProjectionDataIndex],
        query.DepthPerWorldUnit);
    return receiverBias;
}

bool TrySampleDirectionalVsmView(
    float3 worldPosition,
    float3 geometricNormalWS,
    uint viewId,
    VsmScreenRayTraceContext screenRayTraceContext,
    out float shadow)
{
    shadow = 1.0f;
    DirectionalVsmDepthQuery query;
    float3 queryPositionWS;
    if (!TryQueryDirectionalVsmReceiverView(worldPosition, geometricNormalWS, viewId, query, queryPositionWS))
        return false;

    float depthPerWorldUnit = query.DepthPerWorldUnit;
    float receiverBias = GetDirectionalVsmQueryReceiverBias(query, geometricNormalWS);

    // The VSM uses reverse Z: larger depth values are closer to the light.
    bool isLit = query.ReferenceDepth + receiverBias >= query.ShadowDepth;
    if (!isLit && g_VsmSampling.EnableScreenRayTrace != 0u)
    {
        float safeDistance = TraceVsmScreenRay(
            worldPosition,
            g_VsmSampling.ScreenRayLengthScale,
            screenRayTraceContext);
        float traceBias = safeDistance * depthPerWorldUnit;
        isLit = query.ReferenceDepth + max(receiverBias, traceBias) >= query.ShadowDepth;
    }
    shadow = isLit ? 1.0f : 0.0f;
    return true;
}

float SampleDirectionalVsmClipmapHard(
    float3 worldPosition,
    float3 geometricNormalWS,
    uint clipmapId,
    VsmScreenRayTraceContext screenRayTraceContext)
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
            screenRayTraceContext,
            shadow))
    {
        return shadow;
    }

    uint coarseLevel = clipmap.LevelCount - 1u;
    if (firstLevel != coarseLevel && TrySampleDirectionalVsmView(
            worldPosition,
            geometricNormalWS,
            clipmap.FirstViewId + coarseLevel,
            screenRayTraceContext,
            shadow))
    {
        return shadow;
    }

    return 1.0f;
}

#endif

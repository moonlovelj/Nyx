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

bool TrySampleDirectionalVsmView(float3 worldPosition, uint viewId, out float shadow)
{
    shadow = 1.0f;
    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return false;

    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
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

    uint2 atlasTexel = GetVsmPhysicalPageCoord(physicalPageIndex) * VSM_PAGE_SIZE + address.TexelInPage;
    float shadowDepth = GetVsmPhysicalPagePoolSRV().Load(int3(atlasTexel, 0));

    // The VSM uses reverse Z: larger depth values are closer to the light.
    shadow = receiverDepth + 1e-6 >= shadowDepth ? 1.0f : 0.0f;
    return true;
}

float SampleDirectionalVsmClipmapHard(float3 worldPosition, uint clipmapId)
{
    if (clipmapId == VSM_INVALID_VIEW_ID)
        return 1.0f;

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    uint firstLevel = SelectResidentDirectionalVsmClipmapLevel(worldPosition, clipmap);

    float shadow;
    if (TrySampleDirectionalVsmView(worldPosition, clipmap.FirstViewId + firstLevel, shadow))
    {
        return shadow;
    }

    uint coarseLevel = clipmap.LevelCount - 1u;
    if (firstLevel != coarseLevel && TrySampleDirectionalVsmView(worldPosition, clipmap.FirstViewId + coarseLevel, shadow))
    {
        return shadow;
    }

    return 1.0f;
}

#endif

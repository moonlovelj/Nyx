#ifndef NYX_VSM_SAMPLING_H_SLANG
#define NYX_VSM_SAMPLING_H_SLANG

#include "CommonResources.slang"
#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
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

float SampleDirectionalVsmHard(float3 worldPosition, uint viewId)
{
    if (viewId == VSM_INVALID_VIEW_ID)
        return 1.0f;

    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return 1.0f;

    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    if (address.Valid == 0u)
        return 1.0f;

    uint physicalPageIndex = GetVsmPhysicalPageIndex(g_VsmPageTable, shadowView, address.PageTableCoord);
    if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return 1.0f;

    VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadata[physicalPageIndex];
    if (!IsVsmPhysicalPageReady(metadata, shadowView, address.VirtualPage))
        return 1.0f;

    VsmProjectionGpu projection = g_VsmProjections[shadowView.ProjectionDataIndex];
    float4 clipPosition = ProjectWorldToVsmClip(GetVsmProjectionRows(projection), worldPosition);
    if (clipPosition.w <= 0.0f)
        return 1.0f;

    float receiverDepth = clipPosition.z / clipPosition.w;
    if (receiverDepth < 0.0f || receiverDepth > 1.0f)
        return 1.0f;

    uint2 atlasTexel = GetVsmPhysicalPageCoord(physicalPageIndex) * VSM_PAGE_SIZE + address.TexelInPage;
    float shadowDepth = GetVsmPhysicalPagePoolSRV().Load(int3(atlasTexel, 0));

    // The VSM uses reverse Z: larger depth values are closer to the light.
    return receiverDepth >= shadowDepth ? 1.0f : 0.0f;
}

#endif

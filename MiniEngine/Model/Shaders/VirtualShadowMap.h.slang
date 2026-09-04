#ifndef __VIRTUAL_SHADOW_MAP_H_SLANG__
#define __VIRTUAL_SHADOW_MAP_H_SLANG__

#include "../VirtualShadowMapShared.h"

uint SelectDirectionalVsmClipmapLevel(
    float3 receiverPositionWS,
    DirectionalVsmClipmapGpu clipmap,
    float lodBias)
{
    float firstLevelRadius = max(clipmap.OriginAndFirstLevelRadius.w, 1e-6f);
    float distanceToOrigin = length(receiverPositionWS - clipmap.OriginAndFirstLevelRadius.xyz);
    float levelFloat = log2(max(distanceToOrigin / firstLevelRadius, 1e-6f)) + lodBias;
    int maxLevel = max(int(clipmap.LevelCount) - 1, 0);
    return uint(clamp(int(ceil(levelFloat)), 0, maxLevel));
}

int VsmFloorMod(int value, int divisor)
{
    int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

int2 VsmFloorMod(int2 value, int divisor)
{
    return int2(VsmFloorMod(value.x, divisor), VsmFloorMod(value.y, divisor));
}

uint2 GetVsmPageTableCoord(int2 globalPage)
{
    return uint2(VsmFloorMod(globalPage, int(VSM_PAGE_TABLE_DIM)));
}

uint GetVsmPageTableIndex(uint2 pageTableCoord)
{
    return pageTableCoord.y * VSM_PAGE_TABLE_DIM + pageTableCoord.x;
}

uint2 GetVsmPhysicalPageCoord(uint physicalPageIndex)
{
    return uint2(
        physicalPageIndex & VSM_PHYSICAL_POOL_DIM_PAGES_MASK,
        physicalPageIndex >> VSM_PHYSICAL_POOL_DIM_PAGES_LOG2);
}

struct VsmProjectionRows
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
    float4 Row3;
};

VsmProjectionRows GetVsmProjectionRows(VsmProjectionGpu projection)
{
    VsmProjectionRows rows;
    rows.Row0 = projection.ViewProjRow0;
    rows.Row1 = projection.ViewProjRow1;
    rows.Row2 = projection.ViewProjRow2;
    rows.Row3 = projection.ViewProjRow3;
    return rows;
}

// pageCoord is relative to the full projection's virtual-page origin and may be signed.
VsmProjectionRows BuildVsmPageProjection(VsmProjectionRows projection, int2 pageCoord)
{
    float dimension = float(VSM_PAGE_TABLE_DIM);
    float biasX = dimension - 2.0f * float(pageCoord.x) - 1.0f;
    float biasY = 2.0f * float(pageCoord.y) + 1.0f - dimension;

    VsmProjectionRows pageProjection;
    pageProjection.Row0 = dimension * projection.Row0 + biasX * projection.Row3;
    pageProjection.Row1 = dimension * projection.Row1 + biasY * projection.Row3;
    pageProjection.Row2 = projection.Row2;
    pageProjection.Row3 = projection.Row3;
    return pageProjection;
}

float4 ProjectWorldToVsmClip(VsmProjectionRows projection, float3 worldPosition)
{
    float4 position = float4(worldPosition, 1.0f);
    return float4(
        dot(projection.Row0, position),
        dot(projection.Row1, position),
        dot(projection.Row2, position),
        dot(projection.Row3, position));
}

float4 ProjectWorldToVsmPageClip(VsmPhysicalPageView pageView, float3 worldPosition)
{
    float4 position = float4(worldPosition, 1.0f);
    return float4(
        dot(pageView.ViewProjRow0, position),
        dot(pageView.ViewProjRow1, position),
        dot(pageView.ViewProjRow2, position),
        dot(pageView.ViewProjRow3, position));
}

float4 ProjectWorldToPreviousVsmPageClip(VsmPhysicalPageView pageView, float3 worldPosition)
{
    float4 position = float4(worldPosition, 1.0f);
    return float4(
        dot(pageView.PrevViewProjRow0, position),
        dot(pageView.PrevViewProjRow1, position),
        dot(pageView.PrevViewProjRow2, position),
        dot(pageView.PrevViewProjRow3, position));
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

uint2 GetVsmPageTableCoord(uint pageTableIndex)
{
    return uint2(pageTableIndex & VSM_PAGE_TABLE_DIM_MASK, pageTableIndex >> VSM_PAGE_TABLE_DIM_LOG2);
}

uint GetVsmPageTableEntryIndex(VsmShadowView shadowView, uint pageTableIndex)
{
    return shadowView.PageTableBase + pageTableIndex;
}

uint GetVsmPageTableEntryIndex(VsmShadowView shadowView, uint2 pageTableCoord)
{
    return GetVsmPageTableEntryIndex(shadowView, GetVsmPageTableIndex(pageTableCoord));
}

uint EncodeVsmPageTableEntry(uint physicalPageIndex, uint levelOffset)
{
    return (physicalPageIndex & VSM_PAGE_TABLE_PHYSICAL_PAGE_INDEX_MASK) |
        ((levelOffset & VSM_PAGE_TABLE_LEVEL_OFFSET_MASK) << VSM_PAGE_TABLE_LEVEL_OFFSET_SHIFT);
}

bool IsVsmPageTableEntryValid(uint pageTableEntry)
{
    return pageTableEntry != VSM_INVALID_PAGE_TABLE_ENTRY;
}

uint GetVsmPageTablePhysicalPageIndex(uint pageTableEntry)
{
    return pageTableEntry & VSM_PAGE_TABLE_PHYSICAL_PAGE_INDEX_MASK;
}

uint GetVsmPageTableLevelOffset(uint pageTableEntry)
{
    return (pageTableEntry >> VSM_PAGE_TABLE_LEVEL_OFFSET_SHIFT) & VSM_PAGE_TABLE_LEVEL_OFFSET_MASK;
}

bool IsVsmPageTableEntryExact(uint pageTableEntry)
{
    return IsVsmPageTableEntryValid(pageTableEntry) && GetVsmPageTableLevelOffset(pageTableEntry) == 0u;
}

uint GetVsmRequestMaskWordByteAddress(VsmShadowView shadowView, uint wordIndex)
{
    return (shadowView.RequestMaskWordBase + wordIndex) * sizeof(uint);
}

struct VsmRequestMaskLocation
{
    uint ByteAddress;
    uint BitMask;
};

VsmRequestMaskLocation GetVsmRequestMaskLocation(VsmShadowView shadowView, uint2 pageTableCoord)
{
    uint pageTableIndex = GetVsmPageTableIndex(pageTableCoord);

    VsmRequestMaskLocation location;
    location.ByteAddress =
        GetVsmRequestMaskWordByteAddress(shadowView, pageTableIndex >> VSM_REQUEST_MASK_WORD_LOG2);
    location.BitMask = 1u << (pageTableIndex & VSM_REQUEST_MASK_WORD_BIT_MASK);
    return location;
}

bool MarkVsmPageInMask(
    RWByteAddressBuffer pageMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    VsmRequestMaskLocation location = GetVsmRequestMaskLocation(shadowView, pageTableCoord);

    uint previousMask;
    pageMask.InterlockedOr(location.ByteAddress, location.BitMask, previousMask);
    return (previousMask & location.BitMask) == 0u;
}

bool IsVsmPageInMask(
    ByteAddressBuffer pageMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    VsmRequestMaskLocation location = GetVsmRequestMaskLocation(shadowView, pageTableCoord);
    return (pageMask.Load(location.ByteAddress) & location.BitMask) != 0u;
}

bool MarkVsmPageRequested(
    RWByteAddressBuffer requestMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    return MarkVsmPageInMask(requestMask, shadowView, pageTableCoord);
}

bool IsVsmPageRequested(
    ByteAddressBuffer requestMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    return IsVsmPageInMask(requestMask, shadowView, pageTableCoord);
}

uint GetVsmPhysicalPageIndex(
    StructuredBuffer<uint> pageTable,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    uint pageTableEntry = pageTable[GetVsmPageTableEntryIndex(shadowView, pageTableCoord)];
    return IsVsmPageTableEntryValid(pageTableEntry)
        ? GetVsmPageTablePhysicalPageIndex(pageTableEntry)
        : VSM_INVALID_PHYSICAL_PAGE;
}

bool IsVsmPhysicalPageOwnedByView(VsmPhysicalPageMetadata metadata, VsmShadowView shadowView)
{
    return (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_VALID) != 0u &&
        metadata.StableShadowMapId == shadowView.StableShadowMapId &&
        metadata.AddressGeneration == shadowView.AddressGeneration &&
        metadata.AddressType == shadowView.AddressType &&
        metadata.Layer == shadowView.Layer;
}

bool IsVsmPhysicalPageMetadataMatch(
    VsmPhysicalPageMetadata metadata,
    VsmShadowView shadowView,
    int2 virtualPage)
{
    return IsVsmPhysicalPageOwnedByView(metadata, shadowView) && all(metadata.VirtualPage == virtualPage);
}

int2 GetDirectionalVsmGlobalPage(uint pageTableIndex, DirectionalVsmAddressGpu addressData)
{
    int2 windowOriginPage = addressData.AddressAndWindowOriginPage.zw;
    int2 windowOriginCoord = VsmFloorMod(windowOriginPage, int(VSM_PAGE_TABLE_DIM));
    int2 pageTableCoord = int2(GetVsmPageTableCoord(pageTableIndex));
    int2 localPage = VsmFloorMod(pageTableCoord - windowOriginCoord, int(VSM_PAGE_TABLE_DIM));
    return windowOriginPage + localPage;
}

bool IsVsmPageInsideWindow(int2 globalPage, int2 windowOriginPage)
{
    int2 localPage = globalPage - windowOriginPage;
    return all(localPage >= int2(0)) && all(localPage < int2(VSM_PAGE_TABLE_DIM));
}

VsmVirtualAddress GetDirectionalVsmVirtualAddress(float3 worldPosition, DirectionalVsmAddressConstants addressConstants)
{
    float3 relativeWorldPosition = worldPosition - addressConstants.AddressOriginWS;
    float2 relativeLightPosition = mul(addressConstants.WorldToLightRotation, relativeWorldPosition).xy;

    // Light-space Y points up while texture/page Y points down.
    relativeLightPosition.y = -relativeLightPosition.y;

    float2 relativePagePosition = relativeLightPosition * addressConstants.InvWorldUnitsPerPage;
    float2 relativePageFloor = floor(relativePagePosition);
    float2 fractionInPage = relativePagePosition - relativePageFloor;

    VsmVirtualAddress address;
    address.VirtualPage = addressConstants.AddressOriginPage + int2(relativePageFloor);
    address.LocalPage = address.VirtualPage - addressConstants.WindowOriginPage;
    address.PageTableCoord = GetVsmPageTableCoord(address.VirtualPage);
    address.TexelInPage = min(uint2(floor(fractionInPage * float(VSM_PAGE_SIZE))), uint2(VSM_PAGE_SIZE - 1u));
    address.LocalVirtualTexel = address.LocalPage * int(VSM_PAGE_SIZE) + int2(address.TexelInPage);
    address.Valid = IsVsmPageInsideWindow(address.VirtualPage, addressConstants.WindowOriginPage) ? 1u : 0u;

    return address;
}

VsmVirtualAddress GetDirectionalVsmVirtualAddress(float3 worldPosition, DirectionalVsmAddressGpu addressData)
{
    float3 addressOriginWS = addressData.AddressOriginAndInvWorldUnitsPerPage.xyz;
    float3 relativeWorldPosition = worldPosition - addressOriginWS;
    float2 relativeLightPosition = float2(
        dot(addressData.WorldToLightRow0.xyz, relativeWorldPosition),
        dot(addressData.WorldToLightRow1.xyz, relativeWorldPosition));

    // Light-space Y points up while texture/page Y points down.
    relativeLightPosition.y = -relativeLightPosition.y;

    float invWorldUnitsPerPage = addressData.AddressOriginAndInvWorldUnitsPerPage.w;
    float2 relativePagePosition = relativeLightPosition * invWorldUnitsPerPage;
    float2 relativePageFloor = floor(relativePagePosition);
    float2 fractionInPage = relativePagePosition - relativePageFloor;

    int2 addressOriginPage = addressData.AddressAndWindowOriginPage.xy;
    int2 windowOriginPage = addressData.AddressAndWindowOriginPage.zw;

    VsmVirtualAddress address;
    address.VirtualPage = addressOriginPage + int2(relativePageFloor);
    address.LocalPage = address.VirtualPage - windowOriginPage;
    address.PageTableCoord = GetVsmPageTableCoord(address.VirtualPage);
    address.TexelInPage = min(uint2(floor(fractionInPage * float(VSM_PAGE_SIZE))), uint2(VSM_PAGE_SIZE - 1u));
    address.LocalVirtualTexel = address.LocalPage * int(VSM_PAGE_SIZE) + int2(address.TexelInPage);
    address.Valid = IsVsmPageInsideWindow(address.VirtualPage, windowOriginPage) ? 1u : 0u;

    return address;
}

VsmVirtualAddress GetLocalVsmVirtualAddress(float3 worldPosition, VsmProjectionGpu projectionData)
{
    VsmVirtualAddress address;
    address.VirtualPage = int2(0);
    address.LocalPage = int2(0);
    address.LocalVirtualTexel = int2(0);
    address.PageTableCoord = uint2(0u);
    address.TexelInPage = uint2(0u);
    address.Valid = 0u;

    float4 clipPosition = ProjectWorldToVsmClip(GetVsmProjectionRows(projectionData), worldPosition);
    if (clipPosition.w <= 0.0f)
        return address;

    float3 ndcPosition = clipPosition.xyz / clipPosition.w;
    bool insideProjection = all(ndcPosition.xy >= float2(-1.0f)) &&
        all(ndcPosition.xy <= float2(1.0f)) &&
        ndcPosition.z >= 0.0f && ndcPosition.z <= 1.0f;
    if (!insideProjection)
        return address;

    float2 virtualUv = float2(
        ndcPosition.x * 0.5f + 0.5f,
        0.5f - ndcPosition.y * 0.5f);
    uint2 virtualTexel = min(
        uint2(floor(virtualUv * float(VSM_VIRTUAL_RESOLUTION))),
        uint2(VSM_VIRTUAL_RESOLUTION - 1u));

    address.PageTableCoord = virtualTexel >> VSM_PAGE_SIZE_LOG2;
    address.VirtualPage = int2(address.PageTableCoord);
    address.LocalPage = address.VirtualPage;
    address.LocalVirtualTexel = int2(virtualTexel);
    address.TexelInPage = virtualTexel & VSM_PAGE_SIZE_MASK;
    address.Valid = 1u;
    return address;
}

#endif

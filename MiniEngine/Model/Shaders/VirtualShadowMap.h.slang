#ifndef __VIRTUAL_SHADOW_MAP_H_SLANG__
#define __VIRTUAL_SHADOW_MAP_H_SLANG__

#include "../VirtualShadowMapShared.h"

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
        (shadowView.RequestMaskWordBase + (pageTableIndex >> VSM_REQUEST_MASK_WORD_LOG2)) * sizeof(uint);
    location.BitMask = 1u << (pageTableIndex & VSM_REQUEST_MASK_WORD_BIT_MASK);
    return location;
}

bool MarkVsmPageRequested(
    RWByteAddressBuffer requestMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    VsmRequestMaskLocation location = GetVsmRequestMaskLocation(shadowView, pageTableCoord);

    uint previousMask;
    requestMask.InterlockedOr(location.ByteAddress, location.BitMask, previousMask);
    return (previousMask & location.BitMask) == 0u;
}

bool IsVsmPageRequested(
    ByteAddressBuffer requestMask,
    VsmShadowView shadowView,
    uint2 pageTableCoord)
{
    VsmRequestMaskLocation location = GetVsmRequestMaskLocation(shadowView, pageTableCoord);
    return (requestMask.Load(location.ByteAddress) & location.BitMask) != 0u;
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
    address.GlobalPage = addressConstants.AddressOriginPage + int2(relativePageFloor);
    address.LocalPage = address.GlobalPage - addressConstants.WindowOriginPage;
    address.PageTableCoord = GetVsmPageTableCoord(address.GlobalPage);
    address.TexelInPage = min(uint2(floor(fractionInPage * float(VSM_PAGE_SIZE))), uint2(VSM_PAGE_SIZE - 1u));
    address.LocalVirtualTexel = address.LocalPage * int(VSM_PAGE_SIZE) + int2(address.TexelInPage);
    address.InsideWindow = IsVsmPageInsideWindow(address.GlobalPage, addressConstants.WindowOriginPage) ? 1u : 0u;

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
    address.GlobalPage = addressOriginPage + int2(relativePageFloor);
    address.LocalPage = address.GlobalPage - windowOriginPage;
    address.PageTableCoord = GetVsmPageTableCoord(address.GlobalPage);
    address.TexelInPage = min(uint2(floor(fractionInPage * float(VSM_PAGE_SIZE))), uint2(VSM_PAGE_SIZE - 1u));
    address.LocalVirtualTexel = address.LocalPage * int(VSM_PAGE_SIZE) + int2(address.TexelInPage);
    address.InsideWindow = IsVsmPageInsideWindow(address.GlobalPage, windowOriginPage) ? 1u : 0u;

    return address;
}

#endif

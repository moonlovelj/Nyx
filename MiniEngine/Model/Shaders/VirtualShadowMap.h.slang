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

#endif

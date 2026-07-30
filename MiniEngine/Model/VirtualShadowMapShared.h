#pragma once

// Shared C++/Slang virtual shadow-map data contract.
#define VSM_PAGE_SIZE_LOG2 7u
#define VSM_PAGE_SIZE (1u << VSM_PAGE_SIZE_LOG2)
#define VSM_PAGE_SIZE_MASK (VSM_PAGE_SIZE - 1u)

#define VSM_PAGE_TABLE_DIM_LOG2 7u
#define VSM_PAGE_TABLE_DIM (1u << VSM_PAGE_TABLE_DIM_LOG2)
#define VSM_PAGE_TABLE_DIM_MASK (VSM_PAGE_TABLE_DIM - 1u)

#define VSM_VIRTUAL_RESOLUTION (VSM_PAGE_SIZE * VSM_PAGE_TABLE_DIM)

#ifdef __cplusplus

#include "../Core/VectorMath.h"

#include <DirectXMath.h>
#include <cstdint>

#define VSM_FLOAT3X3 Math::Matrix3
#define VSM_FLOAT3 Math::Vector3
#define VSM_INT2 DirectX::XMINT2
#define VSM_UINT2 DirectX::XMUINT2
#define VSM_UINT uint32_t

namespace Renderer::VirtualShadowMap
{
    inline constexpr uint32_t kPageSizeLog2 = VSM_PAGE_SIZE_LOG2;
    inline constexpr uint32_t kPageSize = VSM_PAGE_SIZE;
    inline constexpr uint32_t kPageSizeMask = VSM_PAGE_SIZE_MASK;

    inline constexpr uint32_t kPageTableDimLog2 = VSM_PAGE_TABLE_DIM_LOG2;
    inline constexpr uint32_t kPageTableDim = VSM_PAGE_TABLE_DIM;
    inline constexpr uint32_t kPageTableDimMask = VSM_PAGE_TABLE_DIM_MASK;

    inline constexpr uint32_t kVirtualResolution = VSM_VIRTUAL_RESOLUTION;

#else

#define VSM_FLOAT3X3 float3x3
#define VSM_FLOAT3 float3
#define VSM_INT2 int2
#define VSM_UINT2 uint2
#define VSM_UINT uint

#endif

    // Describes one directional-light clipmap level's stable address space.
    //
    // AddressOriginWS must lie on the boundary of AddressOriginPage in the light
    // plane. Moving the clipmap window must only update WindowOriginPage; it must
    // not change the mapping between a world position and GlobalPage.
    struct DirectionalVsmAddressConstants
    {
        VSM_FLOAT3X3 WorldToLightRotation;
        float InvWorldUnitsPerPage;

        VSM_FLOAT3 AddressOriginWS;
        VSM_UINT LightIndex;

        VSM_INT2 AddressOriginPage;
        VSM_INT2 WindowOriginPage;

        VSM_UINT ClipmapLevel;
        VSM_UINT AddressGeneration;

#ifdef __cplusplus
        DirectionalVsmAddressConstants()
            : WorldToLightRotation(Math::kIdentity), InvWorldUnitsPerPage(0.0f), AddressOriginWS(Math::kZero), LightIndex(0),
              AddressOriginPage{}, WindowOriginPage{}, ClipmapLevel(0), AddressGeneration(0)
        {
        }
#endif
    };

    // Address-only result. It intentionally contains no physical-page state.
    struct VsmVirtualAddress
    {
        VSM_INT2 GlobalPage;
        VSM_INT2 LocalPage;
        VSM_INT2 LocalVirtualTexel;
        VSM_UINT2 PageTableCoord;
        VSM_UINT2 TexelInPage;
        VSM_UINT InsideWindow;
    };

#ifdef __cplusplus
}
#endif

#undef VSM_FLOAT3X3
#undef VSM_FLOAT3
#undef VSM_INT2
#undef VSM_UINT2
#undef VSM_UINT

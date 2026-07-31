#pragma once

// Shared C++/Slang virtual shadow-map data contract.
#define VSM_PAGE_SIZE_LOG2 7u
#define VSM_PAGE_SIZE (1u << VSM_PAGE_SIZE_LOG2)
#define VSM_PAGE_SIZE_MASK (VSM_PAGE_SIZE - 1u)

#define VSM_PAGE_TABLE_DIM_LOG2 7u
#define VSM_PAGE_TABLE_DIM (1u << VSM_PAGE_TABLE_DIM_LOG2)
#define VSM_PAGE_TABLE_DIM_MASK (VSM_PAGE_TABLE_DIM - 1u)

#define VSM_VIRTUAL_RESOLUTION (VSM_PAGE_SIZE * VSM_PAGE_TABLE_DIM)
#define VSM_PAGES_PER_VIEW (VSM_PAGE_TABLE_DIM * VSM_PAGE_TABLE_DIM)

#define VSM_PHYSICAL_POOL_DIM_PAGES_LOG2 5u
#define VSM_PHYSICAL_POOL_DIM_PAGES (1u << VSM_PHYSICAL_POOL_DIM_PAGES_LOG2)
#define VSM_PHYSICAL_PAGE_CAPACITY (VSM_PHYSICAL_POOL_DIM_PAGES * VSM_PHYSICAL_POOL_DIM_PAGES)
#define VSM_PHYSICAL_POOL_RESOLUTION (VSM_PHYSICAL_POOL_DIM_PAGES * VSM_PAGE_SIZE)
#define VSM_INVALID_PHYSICAL_PAGE 0xffffffffu

#define VSM_REQUEST_MASK_WORD_LOG2 5u
#define VSM_REQUEST_MASK_WORD_BITS (1u << VSM_REQUEST_MASK_WORD_LOG2)
#define VSM_REQUEST_MASK_WORD_BIT_MASK (VSM_REQUEST_MASK_WORD_BITS - 1u)
#define VSM_REQUEST_MASK_WORD_COUNT_PER_VIEW (VSM_PAGES_PER_VIEW / VSM_REQUEST_MASK_WORD_BITS)

#define VSM_INVALID_VIEW_ID 0xffffffffu
#define VSM_ADDRESS_TYPE_INVALID 0u
#define VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP 1u

#define VSM_REQUEST_STATISTICS_STRIDE 16u
#define VSM_REQUESTED_PAGE_COUNT_OFFSET 0u
#define VSM_OUT_OF_WINDOW_PIXEL_COUNT_OFFSET 4u

#define VSM_ALLOCATION_STATISTICS_STRIDE 16u
#define VSM_ALLOCATION_REQUESTED_PAGE_COUNT_OFFSET 0u
#define VSM_ALLOCATED_PAGE_COUNT_OFFSET 4u
#define VSM_OVERFLOW_PAGE_COUNT_OFFSET 8u

#ifdef __cplusplus

#include "../Core/VectorMath.h"

#include <DirectXMath.h>
#include <cstdint>

#define VSM_FLOAT3X3 Math::Matrix3
#define VSM_FLOAT3 Math::Vector3
#define VSM_FLOAT4 DirectX::XMFLOAT4
#define VSM_INT2 DirectX::XMINT2
#define VSM_INT4 DirectX::XMINT4
#define VSM_UINT2 DirectX::XMUINT2
#define VSM_UINT uint32_t
#define VSM_ALIGN_16 alignas(16)

namespace Renderer::VirtualShadowMap
{
    inline constexpr uint32_t kPageSizeLog2 = VSM_PAGE_SIZE_LOG2;
    inline constexpr uint32_t kPageSize = VSM_PAGE_SIZE;
    inline constexpr uint32_t kPageSizeMask = VSM_PAGE_SIZE_MASK;

    inline constexpr uint32_t kPageTableDimLog2 = VSM_PAGE_TABLE_DIM_LOG2;
    inline constexpr uint32_t kPageTableDim = VSM_PAGE_TABLE_DIM;
    inline constexpr uint32_t kPageTableDimMask = VSM_PAGE_TABLE_DIM_MASK;

    inline constexpr uint32_t kVirtualResolution = VSM_VIRTUAL_RESOLUTION;
    inline constexpr uint32_t kPagesPerView = VSM_PAGES_PER_VIEW;

    inline constexpr uint32_t kPhysicalPoolDimPagesLog2 = VSM_PHYSICAL_POOL_DIM_PAGES_LOG2;
    inline constexpr uint32_t kPhysicalPoolDimPages = VSM_PHYSICAL_POOL_DIM_PAGES;
    inline constexpr uint32_t kPhysicalPageCapacity = VSM_PHYSICAL_PAGE_CAPACITY;
    inline constexpr uint32_t kPhysicalPoolResolution = VSM_PHYSICAL_POOL_RESOLUTION;
    inline constexpr uint32_t kInvalidPhysicalPage = VSM_INVALID_PHYSICAL_PAGE;

    inline constexpr uint32_t kRequestMaskWordBits = VSM_REQUEST_MASK_WORD_BITS;
    inline constexpr uint32_t kRequestMaskWordCountPerView = VSM_REQUEST_MASK_WORD_COUNT_PER_VIEW;
    inline constexpr uint32_t kInvalidViewId = VSM_INVALID_VIEW_ID;

    static_assert(kPagesPerView % kRequestMaskWordBits == 0);
    static_assert(kPhysicalPageCapacity == 1024);
    static_assert(kPhysicalPoolResolution == 4096);

#else

#define VSM_FLOAT3X3 float3x3
#define VSM_FLOAT3 float3
#define VSM_FLOAT4 float4
#define VSM_INT2 int2
#define VSM_INT4 int4
#define VSM_UINT2 uint2
#define VSM_UINT uint
#define VSM_ALIGN_16

#endif

    // A frame-local shadow projection. One light can own several views:
    // directional clipmap levels, spot mips, or point-light cube faces.
    struct VSM_ALIGN_16 VsmShadowView
    {
        VSM_UINT StableShadowMapId;
        VSM_UINT AddressGeneration;
        VSM_UINT AddressType;
        VSM_UINT AddressDataIndex;

        // RequestMaskWordBase is measured in uint words. PageTableBase is
        // measured in page-table entries.
        VSM_UINT RequestMaskWordBase;
        VSM_UINT PageTableBase;
        VSM_UINT Layer;
        VSM_UINT LightIndex;
    };

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

    // Structured-buffer representation with an explicit 16-byte layout.
    // Rows are stored explicitly to avoid C++/Slang matrix-layout differences.
    struct VSM_ALIGN_16 DirectionalVsmAddressGpu
    {
        VSM_FLOAT4 WorldToLightRow0;
        VSM_FLOAT4 WorldToLightRow1;
        VSM_FLOAT4 WorldToLightRow2;

        // xyz = AddressOriginWS, w = InvWorldUnitsPerPage.
        VSM_FLOAT4 AddressOriginAndInvWorldUnitsPerPage;

        // xy = AddressOriginPage, zw = WindowOriginPage.
        VSM_INT4 AddressAndWindowOriginPage;
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

    // One entry in the dense frame-local list consumed by the future shadow render pass.
    struct VSM_ALIGN_16 VsmPageRenderRequest
    {
        VSM_UINT ViewId;
        VSM_UINT PageTableIndex;
        VSM_UINT PhysicalPageIndex;
        VSM_UINT Padding;
    };

#ifdef __cplusplus
    static_assert(sizeof(VsmShadowView) == 32);
    static_assert(sizeof(DirectionalVsmAddressGpu) == 80);
    static_assert(sizeof(VsmPageRenderRequest) == 16);
}
#endif

#undef VSM_FLOAT3X3
#undef VSM_FLOAT3
#undef VSM_FLOAT4
#undef VSM_INT2
#undef VSM_INT4
#undef VSM_UINT2
#undef VSM_UINT
#undef VSM_ALIGN_16

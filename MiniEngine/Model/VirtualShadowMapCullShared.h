#pragma once

#include "StructsIO.h"

#define VSM_PHYSICAL_PAGE_RENDER_COUNTERS_SIZE 16u
#define VSM_RENDER_PAGE_COUNT_OFFSET 0u
#define VSM_ACTIVE_VIEW_COUNT_OFFSET 4u
#define VSM_RENDER_MASK_PAGE_COUNT_OFFSET 8u
#define VSM_INVALID_RENDER_REQUEST_COUNT_OFFSET 12u

#define VSM_CULL_COUNTERS_SIZE 4u
#define VSM_CULL_OVERFLOW_COUNT_OFFSET 0u

#define VSM_RASTER_DISPATCH_WIDTH 2048u

#define VSM_RASTER_WINDOW_X_SHIFT 0u
#define VSM_RASTER_WINDOW_Y_SHIFT 5u
#define VSM_RASTER_WINDOW_PAGE_MASK_SHIFT 10u
#define VSM_RASTER_WINDOW_COORD_MASK 0x1fu
#define VSM_RASTER_WINDOW_PAGE_MASK_MASK 0xffffu

#ifdef __cplusplus

#include <cstdint>

#define VSM_CULL_UINT uint32_t

namespace Renderer::VirtualShadowMap
{

#else

#define VSM_CULL_UINT uint

#endif

    struct VsmNodeTask
    {
        VSM_CULL_UINT InstanceIndex;
        VSM_CULL_UINT NodeIndex;
        VSM_CULL_UINT ViewId;
    };

    struct VsmCandidateMeshlet
    {
        VisibleMeshletPayload Meshlet;
        VSM_CULL_UINT ViewId;
    };

    struct VsmRasterItem
    {
        VisibleMeshletPayload Meshlet;
        VSM_CULL_UINT ViewId;
        VSM_CULL_UINT PackedWindowAndPageMask;
    };

#ifdef __cplusplus

    static_assert(sizeof(VsmNodeTask) == 12);
    static_assert(sizeof(VsmCandidateMeshlet) == 12);
    static_assert(sizeof(VsmRasterItem) == 16);
}

#endif

#undef VSM_CULL_UINT

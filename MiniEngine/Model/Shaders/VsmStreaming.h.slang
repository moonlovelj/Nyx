#ifndef NYX_VSM_STREAMING_H_SLANG
#define NYX_VSM_STREAMING_H_SLANG

#include "VsmCull.h.slang"

RWByteAddressBuffer g_VsmStreamingIncompletePagesUAV;

void MarkVsmStreamingIncompleteWindow(uint viewId, uint2 window, uint mask)
{
    while (mask != 0u)
    {
        uint bit = uint(firstbitlow(mask));
        uint2 localPage = (window << VSM_RASTER_WINDOW_PAGES_LOG2) +
            uint2(bit & (VSM_RASTER_WINDOW_PAGES - 1u), bit >> VSM_RASTER_WINDOW_PAGES_LOG2);
        VsmPhysicalPageView page;
        if (TryGetDirectionalVsmCullPageView(viewId, localPage, page))
        {
            uint unused;
            g_VsmStreamingIncompletePagesUAV.InterlockedOr(
                (page.PhysicalPageIndex >> 5u) * sizeof(uint),
                1u << (page.PhysicalPageIndex & 31u), unused);
        }
        mask &= mask - 1u;
    }
}

// Include empty pages and the footprint of coarse fallback geometry. Missing
// geometry cannot be discovered by inspecting only emitted raster items.
void MarkVsmStreamingIncompletePages(uint viewId, VsmCullPageBounds bounds)
{
    if (!bounds.Valid)
        return;

    uint2 windowMin = uint2(bounds.Min) >> VSM_RASTER_WINDOW_PAGES_LOG2;
    uint2 windowMax = uint2(bounds.Max) >> VSM_RASTER_WINDOW_PAGES_LOG2;
    for (uint y = windowMin.y; y <= windowMax.y; ++y)
    {
        for (uint x = windowMin.x; x <= windowMax.x; ++x)
        {
            uint2 window = uint2(x, y);
            uint mask = g_VsmRasterWindowPageMasks[GetVsmRasterWindowIndex(viewId, window)];
            if (mask == 0u)
                continue;

            mask &= GetVsmRasterWindowCoverageMask(bounds, window);
            MarkVsmStreamingIncompleteWindow(viewId, window, mask);
        }
    }
}

#endif

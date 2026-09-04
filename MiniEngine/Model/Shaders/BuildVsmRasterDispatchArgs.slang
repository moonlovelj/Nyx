#include "../VirtualShadowMapCullShared.h"

StructuredBuffer<QueueState> g_VsmCullQueueState;
RWByteAddressBuffer g_VsmRasterDispatchArgs;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    uint rasterItemCount = min(
        g_VsmCullQueueState[0].PassState[0].VisibleMeshletCount,
        uint(MAX_VISIBLE_MESHLETS));
    uint dispatchWidth = min(rasterItemCount, VSM_RASTER_DISPATCH_WIDTH);
    uint dispatchHeight = (rasterItemCount + VSM_RASTER_DISPATCH_WIDTH - 1u) /
        VSM_RASTER_DISPATCH_WIDTH;

    g_VsmRasterDispatchArgs.Store3(0u, uint3(dispatchWidth, dispatchHeight, 1u));
}

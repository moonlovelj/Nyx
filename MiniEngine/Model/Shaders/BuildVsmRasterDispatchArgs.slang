#include "../VirtualShadowMapCullShared.h"

struct BuildVsmRasterDispatchArgsConstants
{
    uint PassIndex;
};

ConstantBuffer<BuildVsmRasterDispatchArgsConstants> g_BuildVsmRasterDispatchArgs;
StructuredBuffer<QueueState> g_VsmCullQueueState;
RWByteAddressBuffer g_VsmRasterDispatchArgs;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    uint passIndex = min(g_BuildVsmRasterDispatchArgs.PassIndex, 1u);
    uint rasterItemOffset = passIndex == 0u
        ? 0u
        : min(g_VsmCullQueueState[0].PassState[0].VisibleMeshletCount, uint(MAX_VISIBLE_MESHLETS));
    uint rasterItemCount = min(
        g_VsmCullQueueState[0].PassState[passIndex].VisibleMeshletCount,
        uint(MAX_VISIBLE_MESHLETS) - rasterItemOffset);
    uint dispatchWidth = min(rasterItemCount, VSM_RASTER_DISPATCH_WIDTH);
    uint dispatchHeight = (rasterItemCount + VSM_RASTER_DISPATCH_WIDTH - 1u) /
        VSM_RASTER_DISPATCH_WIDTH;

    g_VsmRasterDispatchArgs.Store3(0u, uint3(dispatchWidth, dispatchHeight, 1u));
}

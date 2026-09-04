#include "../VirtualShadowMapCullShared.h"

struct BuildVsmCullDispatchArgsConstants
{
    uint InstanceGroupCount;
    uint DagGroupCount;
};

ConstantBuffer<BuildVsmCullDispatchArgsConstants> g_BuildVsmCullDispatchArgs;

ByteAddressBuffer g_VsmPhysicalPageRenderCounters;
RWByteAddressBuffer g_VsmCullDispatchArgs;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    uint activeViewCount = g_VsmPhysicalPageRenderCounters.Load(VSM_ACTIVE_VIEW_COUNT_OFFSET);
    uint hasActiveViews = activeViewCount != 0u ? 1u : 0u;
    uint instanceGroupCountX = min(g_BuildVsmCullDispatchArgs.InstanceGroupCount, 65535u);
    uint instanceGroupCountY = instanceGroupCountX != 0u
        ? (g_BuildVsmCullDispatchArgs.InstanceGroupCount + instanceGroupCountX - 1u) / instanceGroupCountX
        : 0u;

    g_VsmCullDispatchArgs.Store3(0u, uint3(
        instanceGroupCountX * hasActiveViews,
        instanceGroupCountY,
        activeViewCount));
    g_VsmCullDispatchArgs.Store3(sizeof(uint3), uint3(
        g_BuildVsmCullDispatchArgs.DagGroupCount * hasActiveViews,
        1u,
        1u));
}

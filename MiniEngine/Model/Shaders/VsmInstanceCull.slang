#ifndef VSM_CULL_PASS_INDEX
#error VSM_CULL_PASS_INDEX must be supplied through ProgramDesc::AddDefine().
#endif

#include "CommonResources.slang"
#include "CullCommon.slang"
#include "VsmCull.h.slang"

struct VsmInstanceCullConstants
{
    uint MaxCommands;
    float4 HZBSizeAndInv;
    DescriptorHandle<Texture2D<float>> PreviousHZB;
    DescriptorHandle<Texture2D<float>> CurrentHZB;
};

ConstantBuffer<VsmInstanceCullConstants> g_VsmInstanceCull;

StructuredBuffer<uint> g_VsmActiveViews;
RWStructuredBuffer<QueueState> g_VsmCullQueueState;
RWStructuredBuffer<VsmNodeTask> g_VsmNodeTasks;
RWByteAddressBuffer g_VsmCullCounters;
SamplerState g_HZBSampler;

void CountVsmCullOverflow(uint count)
{
    if (count == 0u)
        return;

    uint unused;
    g_VsmCullCounters.InterlockedAdd(VSM_CULL_OVERFLOW_COUNT_OFFSET, count, unused);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint instanceGroupCountX = min((g_VsmInstanceCull.MaxCommands + 63u) / 64u, 65535u);
    uint commandIndex = (groupId.y * instanceGroupCountX + groupId.x) * 64u + groupIndex;
    if (commandIndex >= g_VsmInstanceCull.MaxCommands)
        return;

    uint viewId = g_VsmActiveViews[groupId.z];
    VsmShadowView shadowView = g_VsmCullShadowViews[viewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return;

    DrawItem item = GetPotentialDrawItemBufferSRV()[commandIndex];
    InstanceConstant instanceData = LoadInstanceConstant(item.InstanceIndex);
    MeshConstant meshData = LoadMeshConstant(instanceData.MeshBufferIdx);
    VsmCullPageBounds bounds = GetDirectionalVsmCullPageBounds(
        instanceData.BBoxMin,
        instanceData.BBoxMax,
        meshData.WorldMatrix,
        shadowView);

    CullCommonContext cullContext;
    cullContext.ViewportWidth = VSM_PAGE_SIZE;
    cullContext.ViewportHeight = VSM_PAGE_SIZE;
    cullContext.HZBSourceWidth = VSM_PHYSICAL_POOL_RESOLUTION;
    cullContext.HZBSourceHeight = VSM_PHYSICAL_POOL_RESOLUTION;
    cullContext.HZBSizeAndInv = g_VsmInstanceCull.HZBSizeAndInv;
    cullContext.HZBUvScaleBias = float4(1.0f, 1.0f, 0.0f, 0.0f);

    Texture2D<float> previousHzb = g_VsmInstanceCull.PreviousHZB;
    Texture2D<float> currentHzb = g_VsmInstanceCull.CurrentHZB;
    const uint passIndex = VSM_CULL_PASS_INDEX;
    bool visible = DoesVsmPageBoundsNeedTraversal(
        instanceData.BBoxMin,
        instanceData.BBoxMax,
        meshData.WorldMatrix,
        viewId,
        bounds,
        passIndex,
        previousHzb,
        currentHzb,
        g_HZBSampler,
        cullContext);

    uint waveTaskCount = WaveActiveCountBits(visible);
    uint waveTaskStart = 0u;
    uint waveWritableCount = 0u;
    if (WaveIsFirstLane() && waveTaskCount > 0u)
    {
        InterlockedAdd(
            g_VsmCullQueueState[0].PassState[passIndex].NodeWriteOffset,
            waveTaskCount,
            waveTaskStart);

        waveWritableCount = waveTaskStart < MAX_NODES
            ? min(waveTaskCount, MAX_NODES - waveTaskStart)
            : 0u;

        if (waveWritableCount > 0u)
        {
            uint unused;
            InterlockedAdd(
                g_VsmCullQueueState[0].PassState[passIndex].NodeCount,
                waveWritableCount,
                unused);
        }
        CountVsmCullOverflow(waveTaskCount - waveWritableCount);
    }

    waveTaskStart = WaveReadLaneFirst(waveTaskStart);
    waveWritableCount = WaveReadLaneFirst(waveWritableCount);
    uint laneTaskOffset = WavePrefixCountBits(visible);
    if (!visible || laneTaskOffset >= waveWritableCount)
        return;

    VsmNodeTask task;
    task.InstanceIndex = item.InstanceIndex;
    task.NodeIndex = item.NodeIndex;
    task.ViewId = viewId;
    g_VsmNodeTasks[waveTaskStart + laneTaskOffset] = task;
}

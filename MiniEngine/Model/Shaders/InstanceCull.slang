//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

//#ifndef INSTANCE_CULL_PASS_INDEX
//#error INSTANCE_CULL_PASS_INDEX must be supplied through ProgramDesc::AddDefine().
//#endif

#include "CullCommon.slang"
#include "CommonResources.slang"
#ifdef VSM_PHYSICAL_PAGE_PASS
#include "VsmPhysicalPagePass.h.slang"
#endif

struct InstanceCullConstants
{
    float4x4 ViewProjMatrix;
    uint ViewportWidth;
    uint ViewportHeight;
    uint MaxCommands;

#ifndef DISABLE_HZB_CULL
    float4x4 PrevViewProjMatrix;
    float4 HZBSizeAndInv;
    DescriptorHandle<Texture2D<float>> PreviousHZB;
    DescriptorHandle<Texture2D<float>> CurrentHZB;
#endif
};

ConstantBuffer<InstanceCullConstants> g_InstanceCull;

#ifdef VSM_PHYSICAL_PAGE_PASS
RWStructuredBuffer<QueueState> g_VsmTaskQueueStateUAV;
#endif

#ifndef DISABLE_HZB_CULL
SamplerState g_HZBSampler;
#endif

RWStructuredBuffer<QueueState> GetCullTaskQueueStateUAV()
{
#ifdef VSM_PHYSICAL_PAGE_PASS
    return g_VsmTaskQueueStateUAV;
#else
    return GetTaskQueueStateBufferUAV();
#endif
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= g_InstanceCull.MaxCommands)
        return;

    StructuredBuffer<DrawItem> drawItems = GetPotentialDrawItemBufferSRV();

    DrawItem item = drawItems[dispatchThreadID.x];
    InstanceConstant instanceConstant = LoadInstanceConstant(item.InstanceIndex);
    MeshConstant meshConstant = LoadMeshConstant(instanceConstant.MeshBufferIdx);

#ifndef DISABLE_HZB_CULL
    CullCommonContext cullContext;
    cullContext.ViewportWidth = g_InstanceCull.ViewportWidth;
    cullContext.ViewportHeight = g_InstanceCull.ViewportHeight;
    cullContext.HZBSizeAndInv = g_InstanceCull.HZBSizeAndInv;

#ifdef VSM_PHYSICAL_PAGE_PASS
    VsmPhysicalPageView pageView;
    if (!TryGetVsmPhysicalPageView(pageView))
        return;

    cullContext.HZBSourceWidth = VSM_PHYSICAL_POOL_RESOLUTION;
    cullContext.HZBSourceHeight = VSM_PHYSICAL_POOL_RESOLUTION;
    cullContext.HZBUvScaleBias = pageView.PhysicalAtlasUvScaleBias;

#if INSTANCE_CULL_PASS_INDEX == 1
    const uint passIndex = 1;
    bool visible = HasVsmPhysicalPageHistory(pageView) && EvaluateBoundsVisibility(
        instanceConstant.BBoxMin,
        instanceConstant.BBoxMax,
        meshConstant.WorldMatrix,
        GetVsmPhysicalPageViewProj(pageView),
        g_InstanceCull.CurrentHZB,
        g_HZBSampler,
        cullContext);
#else
    const uint passIndex = 0;
    bool visible = HasVsmPhysicalPageHistory(pageView)
        ? EvaluateBoundsVisibility(
            instanceConstant.BBoxMin,
            instanceConstant.BBoxMax,
            meshConstant.WorldMatrix,
            GetVsmPhysicalPagePrevViewProj(pageView),
            g_InstanceCull.PreviousHZB,
            g_HZBSampler,
            cullContext)
        : EvaluateBoundsInFrustum(
            instanceConstant.BBoxMin,
            instanceConstant.BBoxMax,
            meshConstant.WorldMatrix,
            GetVsmPhysicalPageViewProj(pageView),
            cullContext);
#endif
#else
    cullContext.HZBSourceWidth = g_InstanceCull.ViewportWidth;
    cullContext.HZBSourceHeight = g_InstanceCull.ViewportHeight;
    cullContext.HZBUvScaleBias = float4(1.0f, 1.0f, 0.0f, 0.0f);

#if INSTANCE_CULL_PASS_INDEX == 1
    const uint passIndex = 1;
    const float4x4 viewProjMatrix = g_InstanceCull.ViewProjMatrix;
    Texture2D<float> hzbTexture = g_InstanceCull.CurrentHZB;
#else
    const uint passIndex = 0;
    const float4x4 viewProjMatrix = g_InstanceCull.PrevViewProjMatrix;
    Texture2D<float> hzbTexture = g_InstanceCull.PreviousHZB;
#endif

    bool visible = EvaluateBoundsVisibility(
        instanceConstant.BBoxMin,
        instanceConstant.BBoxMax,
        meshConstant.WorldMatrix,
        viewProjMatrix,
        hzbTexture,
        g_HZBSampler,
        cullContext);
#endif
#else
    CullCommonContext cullContext;
    cullContext.ViewportWidth = g_InstanceCull.ViewportWidth;
    cullContext.ViewportHeight = g_InstanceCull.ViewportHeight;
#ifdef VSM_PHYSICAL_PAGE_PASS
    VsmPhysicalPageView pageView;
    if (!TryGetVsmPhysicalPageView(pageView))
        return;

    float4x4 viewProjMatrix = GetVsmPhysicalPageViewProj(pageView);
#else
    float4x4 viewProjMatrix = g_InstanceCull.ViewProjMatrix;
#endif
    const uint passIndex = 0;
    bool visible = EvaluateBoundsInFrustum(
        instanceConstant.BBoxMin,
        instanceConstant.BBoxMax,
        meshConstant.WorldMatrix,
        viewProjMatrix,
        cullContext);
#endif

    uint waveVisibleCount = WaveActiveCountBits(visible);
    uint waveBaseIndex = 0;
    if (WaveIsFirstLane() && waveVisibleCount > 0)
    {
        RWStructuredBuffer<QueueState> taskState = GetCullTaskQueueStateUAV();
        // Allocate space for the whole wave at once
        InterlockedAdd(taskState[0].PassState[passIndex].NodeWriteOffset, waveVisibleCount, waveBaseIndex);
        InterlockedAdd(taskState[0].PassState[passIndex].NodeCount, waveVisibleCount);
    }

    // Broadcast base index and compute lane offset
    waveBaseIndex = WaveReadLaneFirst(waveBaseIndex);
    uint laneOffset = WavePrefixCountBits(visible);

    // Only visible lanes perform the store
    if (visible)
    {
        uint finalIndex = waveBaseIndex + laneOffset;

        RWByteAddressBuffer taskQueue = GetTaskQueueBufferUAV();
        uint byteOffset = finalIndex * sizeof(DrawItem);
        taskQueue.Store(byteOffset, item.InstanceIndex);
        taskQueue.Store(byteOffset + 4, item.NodeIndex);
    }
}

#ifndef __INSTANCE_CULL_CS_HLSL__
#define __INSTANCE_CULL_CS_HLSL__

#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "BindlessIndices.hlsli"
#include "CullCommon.hlsli"

[RootSignature(Renderer_RootSig)]
[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
#ifdef INSTANCE_CULL_PASS1
    const uint passIndex = 1;
#else
    const uint passIndex = 0;
#endif
    
   if (DTid.x < MaxCommands)
   {
        StructuredBuffer<DrawItem> drawItems = GetPotentialDrawItemBufferSRV();
        DrawItem item = drawItems[DTid.x];
        InstanceConstant instanceConstant = GetInstanceConstantSRV(item.InstanceIndex);
        MeshConstant meshConstant = GetMeshConstantSRV(instanceConstant.MeshBufferIdx);

        bool bVisible = false;
#ifdef INSTANCE_CULL_PASS0
        bVisible = EvaluateBoundsVisibility(instanceConstant.BBoxMin, instanceConstant.BBoxMax,
            meshConstant.WorldMatrix, PrevViewProjMatrix, GetPrevSceneHZBSRV(FrameIndexMod2));
#endif
        
#ifdef INSTANCE_CULL_PASS1
        bVisible = EvaluateBoundsVisibility(instanceConstant.BBoxMin, instanceConstant.BBoxMax,
            meshConstant.WorldMatrix, ViewProjMatrix, GetCurrentSceneHZBSRV(FrameIndexMod2));
#endif
        
        uint WaveVisibleCount = WaveActiveCountBits(bVisible);
        uint WaveBaseIndex = 0;
        if (WaveIsFirstLane() && WaveVisibleCount > 0)
        {
            globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
            // Allocate space for the whole wave at once
            InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeWriteOffset, WaveVisibleCount, WaveBaseIndex);
            InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, WaveVisibleCount);
        }
        
        // Broadcast base index and compute lane offset
        WaveBaseIndex = WaveReadLaneFirst(WaveBaseIndex);
        uint LaneOffset = WavePrefixCountBits(bVisible);

        // Only visible lanes perform the store
        if (bVisible)
        {
            uint FinalIndex = WaveBaseIndex + LaneOffset;
        
            globallycoherent RWByteAddressBuffer taskQueueUAV = GetTaskQueueBufferUAV();
            uint byteOffset = FinalIndex * NODE_BYTE_STRIDE;
            taskQueueUAV.Store(byteOffset, item.InstanceIndex);
            taskQueueUAV.Store(byteOffset + 4, item.NodeIndex);
        }
   }
}
    
#endif

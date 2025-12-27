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
        bool bInFrustrum = IsSphereInFrustum(meshConstant.WorldMatrix, instanceConstant.BoundingSphere);
        bool bVisible = bInFrustrum;
#ifdef INSTANCE_CULL_PASS0
        bVisible = bVisible && 
            IsSphereNotOccluded(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, PrevViewProjMatrix, 
                            meshConstant.WorldMatrix, instanceConstant.BoundingSphere);
#endif         
#ifdef INSTANCE_CULL_PASS1
        bVisible = bVisible && 
            IsSphereNotOccluded(GetCurrentSceneHZBSRV(FrameIndexMod2), ViewerPos, ViewProjMatrix, 
                            meshConstant.WorldMatrix, instanceConstant.BoundingSphere);
#endif
        
        uint WaveVisibleCount = WaveActiveCountBits(bVisible);
        uint WaveBaseIndex = 0;
        if (WaveIsFirstLane() && WaveVisibleCount > 0)
        {
            globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
            // 一次性为整个 Wave 申请空间
            InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeWriteOffset, WaveVisibleCount, WaveBaseIndex);
            InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, WaveVisibleCount);
        }
        
        // 广播起始索引并计算波内偏移
        WaveBaseIndex = WaveReadLaneFirst(WaveBaseIndex);
        uint LaneOffset = WavePrefixCountBits(bVisible);

        // 只有可见的线程才进行 Store
        if (bVisible)
        {
            uint FinalIndex = WaveBaseIndex + LaneOffset;
        
            globallycoherent RWByteAddressBuffer taskQueueUAV = GetTaskQueueBufferUAV();
            uint byteOffset = FinalIndex * NODE_BYTE_STRIDE;
            taskQueueUAV.Store(byteOffset, item.InstanceIndex);
            taskQueueUAV.Store(byteOffset + 4, item.NodeIndex);
        }
        
//#ifdef INSTANCE_CULL_PASS0

//        // Pass1
//        if (visible & CULLING_FRUSTUM_VISIBLE &&
//            IsSphereVisible(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, PrevViewProjMatrix, WorldMatrix, meshletConstant.BoundingSphere))
//        {
//            visibleFlagUAV.Store(index * 4, visible | CULLING_OCCLUSION_PASS1_VISIBLE);
//        }
//#endif
        
//#ifdef INSTANCE_CULL_PASS1
        
//        // Pass2
//        if ((visible & CULLING_FRUSTUM_VISIBLE) && !(visible & CULLING_OCCLUSION_PASS1_VISIBLE) &&
//            IsSphereVisible(GetCurrentSceneHZBSRV(FrameIndexMod2), ViewerPos, ViewProjMatrix, WorldMatrix, meshletConstant.BoundingSphere))
//        {
//            visibleFlagUAV.Store(index * 4, visible | CULLING_OCCLUSION_PASS2_VISIBLE);
//        }
//#endif
   }
}
    
#endif
#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "../StructsIO.h"

[RootSignature(Renderer_RootSig)]
[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    globallycoherent RWStructuredBuffer<QueueState> taskQueueStateUAV = GetTaskQueueStateBufferUAV();
    // 汇总两次 Pass 的可见 Meshlet 数量
    taskQueueStateUAV[0].PassState[0].VisibleMeshletCount += taskQueueStateUAV[0].PassState[1].VisibleMeshletCount;
}
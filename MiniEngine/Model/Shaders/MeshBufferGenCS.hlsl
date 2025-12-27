#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "../StructsIO.h"

[RootSignature(Renderer_RootSig)]
[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    RWStructuredBuffer<DispatchMeshCommand> MeshCommand = GetDispatchMeshBufferUAV();
    StructuredBuffer<QueueState> taskStateSRV = GetTaskQueueStateBufferSRV();

    // MaxCommands在这里的语义是Pass Index
    const uint meshletCount = taskStateSRV[0].PassState[MaxCommands].VisibleMeshletCount;
    MeshCommand[DTid.x].ThreadGroupCountX = meshletCount;
    MeshCommand[DTid.x].ThreadGroupCountY = 1;
    MeshCommand[DTid.x].ThreadGroupCountZ = 1;
}
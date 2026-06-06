#include "CommonResources.slang"

struct MeshBufferGenConstants
{
    uint PassIndex;
};

ConstantBuffer<MeshBufferGenConstants> g_MeshBufferGen;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 DTid : SV_DispatchThreadID)
{
    RWStructuredBuffer<DispatchMeshCommand> MeshCommand = GetDispatchMeshBufferUAV();
    StructuredBuffer<QueueState> taskStateSRV = GetTaskQueueStateBufferSRV();

    // PassIndex replaces the old MaxCommands root constant used by the HLSL path.
    const uint meshletCount = taskStateSRV[0].PassState[g_MeshBufferGen.PassIndex].VisibleMeshletCount;
    MeshCommand[DTid.x].ThreadGroupCountX = meshletCount;
    MeshCommand[DTid.x].ThreadGroupCountY = 1;
    MeshCommand[DTid.x].ThreadGroupCountZ = 1;
}

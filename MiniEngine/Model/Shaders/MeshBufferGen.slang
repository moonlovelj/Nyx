#include "CommonResources.slang"

struct MeshBufferGenConstants
{
    uint PassIndex;
};

ConstantBuffer<MeshBufferGenConstants> g_MeshBufferGen;

#ifdef VSM_PHYSICAL_PAGE_PASS
StructuredBuffer<QueueState> g_VsmTaskQueueStateSRV;
RWStructuredBuffer<DispatchMeshCommand> g_VsmIndirectDispatchMeshUAV;
#endif

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 DTid : SV_DispatchThreadID)
{
#ifdef VSM_PHYSICAL_PAGE_PASS
    RWStructuredBuffer<DispatchMeshCommand> MeshCommand = g_VsmIndirectDispatchMeshUAV;
    StructuredBuffer<QueueState> taskStateSRV = g_VsmTaskQueueStateSRV;
#else
    RWStructuredBuffer<DispatchMeshCommand> MeshCommand = GetDispatchMeshBufferUAV();
    StructuredBuffer<QueueState> taskStateSRV = GetTaskQueueStateBufferSRV();
#endif

    // PassIndex replaces the old MaxCommands root constant used by the HLSL path.
    const uint meshletCount = taskStateSRV[0].PassState[g_MeshBufferGen.PassIndex].VisibleMeshletCount;
    MeshCommand[DTid.x].ThreadGroupCountX = meshletCount;
    MeshCommand[DTid.x].ThreadGroupCountY = 1;
    MeshCommand[DTid.x].ThreadGroupCountZ = 1;
}

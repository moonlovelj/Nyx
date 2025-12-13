#include "Common.hlsli"
#include "CommonResources.hlsli"

[RootSignature(Renderer_RootSig)]
[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    RWStructuredBuffer<DispatchMeshCommand> MeshCommand = GetDispatchMeshBufferUAV(PsoIdx);
    ByteAddressBuffer CounterBuffer = GetIndirectCullingResultsCounterBufferSRV(PsoIdx);
    uint count = CounterBuffer.Load(0);
    MeshCommand[DTid.x].ThreadGroupCountX = count;
    MeshCommand[DTid.x].ThreadGroupCountY = 1;
    MeshCommand[DTid.x].ThreadGroupCountZ = 1;
}
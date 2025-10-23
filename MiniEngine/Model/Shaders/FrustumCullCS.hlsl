#ifndef __FRUSTUM_CULL_HLSLI__
#define __FRUSTUM_CULL_HLSLI__

#include "Common.hlsli"
#include "CommonResources.hlsli"

#define threadBlockSize 128

cbuffer CullConstants : register(b2)
{
    uint startCommand;
    uint maxCommands;
};

struct IndirectCommand
{
    uint ObjectIndex;
    uint4 drawArgumentsLo;
    uint drawArgumentsHi;
    uint2 paddings;
};

StructuredBuffer<IndirectCommand> inputCommands : register(t30); // SRV: Indirect commands
AppendStructuredBuffer<IndirectCommand> outputCommands : register(u5); // UAV: Processed indirect commands

[RootSignature(Renderer_RootSig)]
[numthreads(threadBlockSize, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * threadBlockSize) + groupIndex;
    if (index < maxCommands)
    {
        outputCommands.Append(inputCommands[index + startCommand]);
    }
}

#endif
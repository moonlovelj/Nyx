#ifndef __FRUSTUM_CULL_HLSLI__
#define __FRUSTUM_CULL_HLSLI__

#include "Common.hlsli"
#include "CommonResources.hlsli"

#define threadBlockSize 128

cbuffer CullConstants : register(b3)
{
    uint startCommand;
    uint maxCommands;
};

struct IndirectCommand
{
    uint MeshletIndex;
    uint4 drawArgumentsLo;
    uint drawArgumentsHi;
    uint2 paddings;
};

StructuredBuffer<IndirectCommand> inputCommands : register(t30); // SRV: Indirect commands
AppendStructuredBuffer<IndirectCommand> outputCommands : register(u5); // UAV: Processed indirect commands


bool IsSphereInFrustum(float4x4 WorldMatrix, float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));
    
    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    float4 sphereVS = float4(mul(ViewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);
    
    // Sphere: xyz = center, w = radius
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 plane;
        if (i == 0) plane = ViewSpaceFrustumPlanes0;
        else if (i == 1) plane = ViewSpaceFrustumPlanes1;
        else if (i == 2) plane = ViewSpaceFrustumPlanes2;
        else if (i == 3) plane = ViewSpaceFrustumPlanes3;
        else if (i == 4) plane = ViewSpaceFrustumPlanes4;
        else plane = ViewSpaceFrustumPlanes5;
        float distance = dot(plane.xyz, sphereVS.xyz) + plane.w;
        if (distance < -sphereVS.w)
            return false;
    }
    return true;
}

[RootSignature(Renderer_RootSig)]
[numthreads(threadBlockSize, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * threadBlockSize) + groupIndex;
    if (index < maxCommands)
    {
        IndirectCommand inCommand = inputCommands[index + startCommand];
        MeshletConstant meshletConstant = MeshletConstants[inCommand.MeshletIndex];
        MeshConstant meshConstant = MeshConstants[meshletConstant.MeshConstantsIndex];
        float4x4 WorldMatrix = meshConstant.WorldMatrix;
        if(IsSphereInFrustum(WorldMatrix, meshletConstant.BoundingSphere))
        {
            outputCommands.Append(inCommand);
        }
    }
}

#endif
#ifndef __FRUSTUM_CULL_HLSLI__
#define __FRUSTUM_CULL_HLSLI__

#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "CullingCommon.hlsli"

#define PIXEL_ERROR_THRESHOLD 1.0

bool nearlyGreater(float a, float b, float rel = 1e-4, float abse = 1e-6)
{
    return a > b - max(rel * max(abs(a), abs(b)), abse);
}

bool nearlyLess(float a, float b, float rel = 1e-4, float abse = 1e-6)
{
    return a < b + max(rel * max(abs(a), abs(b)), abse);
}

float GetProjectedError(float4 sphereVS, float errorWS)
{
    float d2 = dot(sphereVS.xyz, sphereVS.xyz);
    float r = errorWS;
    return 1.0 * ScreenErrorConstant * r / sqrt(max(d2 - r * r, 1e-6));
}

float GetProjectedErrorPerspective(float clipW, float errorWS, float lodScalePixels)
{
    return errorWS * lodScalePixels / max(abs(clipW), 1e-6);
}

bool ShouldMeshletLodVisible(
    float4x4 WorldMatrix,
    float lodError, float4 lodBounds,
    float parentError, float4 parentBounds)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));
    
    float4 sphereWS = float4(mul(WorldMatrix, float4(lodBounds.xyz, 1)).xyz, sphereScale * lodBounds.w);
    float4 sphereVS = float4(mul(ViewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);
    float screenError = GetProjectedError(sphereVS, lodError * sphereScale);
    if (screenError >= PIXEL_ERROR_THRESHOLD)
    {
        return false;
    }
    
    if (!isinf(parentError))
    {
        float4 parentSphereWS = float4(mul(WorldMatrix, float4(parentBounds.xyz, 1)).xyz, sphereScale * parentBounds.w);
        float4 parentSphereVS = float4(mul(ViewMatrix, float4(parentSphereWS.xyz, 1)).xyz, parentSphereWS.w);
        float parentScreenError = GetProjectedError(parentSphereVS, parentError * sphereScale);
        if (parentScreenError < PIXEL_ERROR_THRESHOLD)
        {
            return false;
        }
    }
    
    return true;
}

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
        if (i == 0)
            plane = ViewSpaceFrustumPlanes0;
        else if (i == 1)
            plane = ViewSpaceFrustumPlanes1;
        else if (i == 2)
            plane = ViewSpaceFrustumPlanes2;
        else if (i == 3)
            plane = ViewSpaceFrustumPlanes3;
        else if (i == 4)
            plane = ViewSpaceFrustumPlanes4;
        else
            plane = ViewSpaceFrustumPlanes5;
        float distance = dot(plane.xyz, sphereVS.xyz) + plane.w;
        if (distance < -sphereVS.w)
            return false;
    }
    return true;
}

[RootSignature(Renderer_RootSig)]
[numthreads(CULLING_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * CULLING_THREAD_GROUP_SIZE) + groupIndex;
    if (index < MaxCommands)
    {
        RWByteAddressBuffer visibleFlagUAV = GetIndirectVisibleFlagsBufferUAV(PsoIdx);
        IndirectCommand inCommand = GetIndirectCommandsBufferSRV(PsoIdx, index);

        MeshletConstant meshletConstant = GetMeshletConstantSRV(inCommand.MeshletIndex);
        InstanceConstant instanceConstant = GetInstanceConstantSRV(inCommand.InstanceIndex);
            MeshConstant meshConstant = GetMeshConstantSRV(instanceConstant.MeshBufferIdx + meshletConstant.MeshConstantsIndexOffset);
        float4x4 WorldMatrix = meshConstant.WorldMatrix;

        if (IsSphereInFrustum(WorldMatrix, meshletConstant.BoundingSphere) &&
            ShouldMeshletLodVisible(WorldMatrix,
            meshletConstant.lodError, meshletConstant.lodBounds,
            meshletConstant.parentError, meshletConstant.parentBounds))
        {
            visibleFlagUAV.Store(index * 4, CULLING_FRUSTUM_VISIBLE);
        }
        else
        {
            visibleFlagUAV.Store(index * 4, 0);
        }
    }
}

#endif
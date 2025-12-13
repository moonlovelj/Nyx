//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//

#include "Common.hlsli"
#include "DataCodec.hlsli"
#include "VSTOPSCommon.hlsli"
#include "CommonResources.hlsli"

#ifdef ENABLE_SKINNING
//#undef ENABLE_SKINNING
#endif

struct VSInput
{
    uint vertexID : SV_VertexID;
};

[RootSignature(Renderer_RootSig)]
VSOutput main(VSInput vsInput)
{
    VSOutput vsOutput;
    
    InstanceConstant instanceConstant = GetInstanceConstantSRV(0);
    
    MeshletConstant meshletConstant = GetMeshletConstantSRV(0);
    
    ByteAddressBuffer IndexBuffer = GetIndexBufferSRV();
    uint vertexID = IndexBuffer.Load(meshletConstant.IndexBufferOffset + vsInput.vertexID * 4);
    
    uint VertexLoadOffset = meshletConstant.VertexBufferOffset + vertexID * meshletConstant.VertexStride;
    
    ByteAddressBuffer VertexBuffer = GetVertexBufferSRV();
    uint3 PackedPos = VertexBuffer.Load3(VertexLoadOffset);
    float4 position = float4(asfloat(PackedPos), 1.0);
    VertexLoadOffset += 12;
    
    uint PackedNormal = VertexBuffer.Load(VertexLoadOffset);
    float3 normal = DecodeR10G10B10A2UNORMToFloat4(PackedNormal).xyz * 2 - 1;
    VertexLoadOffset += 4;
    
#ifndef NO_TANGENT_FRAME
    uint PackedTangent = VertexBuffer.Load(VertexLoadOffset);
    float4 tangent = DecodeR10G10B10A2UNORMToFloat4(PackedTangent) * 2 - 1;
    VertexLoadOffset += 4;
#endif
    
    uint PackedUV = VertexBuffer.Load(VertexLoadOffset);
    VertexLoadOffset += 4;
    
#ifndef NO_SECOND_UV
    uint PackedUV1 = VertexBuffer.Load(VertexLoadOffset);
    VertexLoadOffset += 4;
#endif

#ifdef ENABLE_SKINNING
    
    uint2 PackedJointIndices = VertexBuffer.Load2(VertexLoadOffset);
    VertexLoadOffset += 8;
    uint2 PackedWeights = VertexBuffer.Load2(VertexLoadOffset);
    VertexLoadOffset += 8;
    
    uint4 jointIndices = DecodeR16G16B16A16UINTToUint4(PackedJointIndices);
    float4 jointWeights = DecodeR16G16B16A16UNORMToFloat4(PackedWeights);
    
    // I don't like this hack.  The weights should be normalized already, but something is fishy.
    float4 weights = jointWeights / dot(jointWeights, 1);

    float4x4 skinPosMat =
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.x).PosMatrix * weights.x +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.y).PosMatrix * weights.y +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.z).PosMatrix * weights.z +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.w).PosMatrix * weights.w;

    position = mul(skinPosMat, position);

    float4x3 skinNrmMat =
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.x).NrmMatrix * weights.x +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.y).NrmMatrix * weights.y +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.z).NrmMatrix * weights.z +
        GetJointBufferSRV(instanceConstant.JointBase + meshletConstant.MeshJointsIndexOffset + jointIndices.w).NrmMatrix * weights.w;

    normal = mul(skinNrmMat, normal).xyz;
#ifndef NO_TANGENT_FRAME
    tangent.xyz = mul(skinNrmMat, tangent.xyz).xyz;
#endif

#endif

    MeshConstant meshConstant = GetMeshConstantSRV(instanceConstant.MeshConstantsBase + meshletConstant.MeshConstantsIndexOffset);
    float4x4 WorldMatrix = meshConstant.WorldMatrix;
    float4x3 WorldIT = meshConstant.WorldIT;
    vsOutput.worldPos = mul(WorldMatrix, position).xyz;
    vsOutput.position = mul(ViewProjMatrix, float4(vsOutput.worldPos, 1.0));
    vsOutput.sunShadowCoord = mul(SunShadowMatrix, float4(vsOutput.worldPos, 1.0)).xyz;
    vsOutput.normal = mul(WorldIT, normal).xyz;
#ifndef NO_TANGENT_FRAME
    vsOutput.tangent = float4(mul(WorldIT, tangent.xyz).xyz, tangent.w);
#endif
    
    vsOutput.uv0 = DecodeR16G16FLOATToFloat2(PackedUV);
#ifndef NO_SECOND_UV
    vsOutput.uv1 = DecodeR16G16FLOATToFloat2(PackedUV1);
#endif

    return vsOutput;
}

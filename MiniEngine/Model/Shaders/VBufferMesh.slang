//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

#ifndef VBUFFER_MESH_PASS_INDEX
#error VBUFFER_MESH_PASS_INDEX must be supplied through ProgramDesc::AddDefine().
#endif

#include "CommonResources.slang"
#include "DataCodec.h.slang"
#include "GeometryCommon.slang"
#include "WaveUtil.h.slang"
#include "CullCommon.slang"

#define MAX_VERTS 128
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 32

struct VBufferMeshConstants
{
    float4x4 ViewProjMatrix;
    uint ViewportWidth;
    uint ViewportHeight;
};

struct MeshVertexOutput
{
    float4 Position : SV_POSITION;
    float2 UV0 : TEXCOORD0;
    nointerpolation uint CommandIndex : TEXCOORD1;
    nointerpolation uint PackedMaterialInfo : TEXCOORD2;
};

struct PrimitiveAttributes
{
    nointerpolation uint PrimitiveIndex : TEXCOORD3;
};

ConstantBuffer<VBufferMeshConstants> g_VBufferMesh;

groupshared uint s_PrimCount;
groupshared uint s_PrimOutIndex[MAX_PRIMS];
groupshared uint3 s_PrimIndices[MAX_PRIMS];

float4 GetClipPosition(
    ByteAddressBuffer geometryChunksBuffer,
    uint vertexByteOffset,
    uint vertexStride,
    uint localVertexIdx,
    float4x4 worldMatrix,
    float4x4 vpMatrix)
{
    uint vertexOffset = vertexByteOffset + vertexStride * localVertexIdx;
    float4 position = float4(asfloat(geometryChunksBuffer.Load3(vertexOffset)), 1.0);

    float3 worldPos = mul(worldMatrix, position).xyz;
    float4 clipPos = mul(vpMatrix, float4(worldPos, 1.0));
    return clipPos;
}

float2 GetScreenPos(float4 clipPos)
{
    float2 ndc = clipPos.xy / clipPos.w;
    return (ndc * 0.5 + 0.5) * float2(float(g_VBufferMesh.ViewportWidth), float(g_VBufferMesh.ViewportHeight));
}

MeshVertexOutput BuildVertexOutput(
    ByteAddressBuffer geometryChunksBuffer,
    MeshletHeader meshletHeader,
    MaterialConstant mat,
    MeshConstant meshInstance,
    uint vertexByteOffset,
    uint vertexStride,
    uint localVertexID,
    uint commandIndex)
{
    uint vertexOffset = vertexByteOffset + vertexStride * localVertexID;
    float4 position = float4(asfloat(geometryChunksBuffer.Load3(vertexOffset)), 1.0);
    vertexOffset += 12;

    uint psoFlags = meshletHeader.GetPSOFlags();
    float2 uv0 = 0;
    if ((psoFlags & PSO_ALPHA_TEST) != 0)
    {
        uint uvLoadOffset = vertexOffset + 4; // normal
        if ((psoFlags & PSO_HAS_TANGENT) != 0)
            uvLoadOffset += 4; // tangent

        if (((mat.flags & MAT_FLAG_BASE_COLOR_UV) != 0) && ((psoFlags & PSO_HAS_UV1) != 0))
        {
            if ((psoFlags & PSO_HAS_UV0) != 0)
                uvLoadOffset += 4;
            uint packedUV = geometryChunksBuffer.Load(uvLoadOffset);
            uv0 = DecodeR16G16FLOATToFloat2(packedUV);

        }
        else if ((psoFlags & PSO_HAS_UV0) != 0)
        {
            uint packedUV = geometryChunksBuffer.Load(uvLoadOffset);
            uv0 = DecodeR16G16FLOATToFloat2(packedUV);
        }
    }

    // Skinning (dynamic branch)
    //if (mlet.psoFlags & PSO_HAS_SKIN)
    //{
    //    uint2 PackedJointIndices = geometryData.Load2(vertexOffset);
    //    vertexOffset += 8;
    //    uint2 PackedWeights = geometryData.Load2(vertexOffset);
    //    vertexOffset += 8;

    //    uint4 jointIndices = DecodeR16G16B16A16UINTToUint4(PackedJointIndices);
    //    float4 jointWeights = DecodeR16G16B16A16UNORMToFloat4(PackedWeights);

    //    // I don't like this hack.  The weights should be normalized already, but something is fishy.
    //    float4 weights = jointWeights / dot(jointWeights, 1);

    //    float4x4 skinPosMat =
    //        GetJointBufferSRV(inst.JointBufferIdx + mlet.MeshJointsIndexOffset + jointIndices.x).PosMatrix * weights.x +
    //        GetJointBufferSRV(inst.JointBufferIdx + mlet.MeshJointsIndexOffset + jointIndices.y).PosMatrix * weights.y +
    //        GetJointBufferSRV(inst.JointBufferIdx + mlet.MeshJointsIndexOffset + jointIndices.z).PosMatrix * weights.z +
    //        GetJointBufferSRV(inst.JointBufferIdx + mlet.MeshJointsIndexOffset + jointIndices.w).PosMatrix * weights.w;

    //    position = mul(skinPosMat, position);
    //}

    float3 worldPos = mul(meshInstance.WorldMatrix, position).xyz;
    float4 clipPos = mul(g_VBufferMesh.ViewProjMatrix, float4(worldPos, 1.0));

    uint packedMaterialInfo = (meshletHeader.GetMaterialBufferIndex() & 0xFFFF)
        | ((mat.flags & 0xFF) << 16);

    MeshVertexOutput output;
    output.Position = clipPos;
    output.CommandIndex = commandIndex;
    output.PackedMaterialInfo = packedMaterialInfo;
    output.UV0 = uv0;
    return output;
}

[shader("mesh")]
[numthreads(MS_GROUP_SIZE, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    OutputVertices<MeshVertexOutput, MAX_VERTS> verts,
    OutputIndices<uint3, MAX_PRIMS> outIndices,
    OutputPrimitives<PrimitiveAttributes, MAX_PRIMS> sharedPrimitives)
{
#if VBUFFER_MESH_PASS_INDEX == 1
    StructuredBuffer<QueueState> taskStateSRV = GetTaskQueueStateBufferSRV();
    const uint commandStart = taskStateSRV[0].PassState[0].VisibleMeshletCount;
#else
    const uint commandStart = 0;
#endif

    uint commandIndex = gid + commandStart;
    VisibleMeshletPayload payload = GetVisibleMeshletPayload(commandIndex);
    InstanceConstant inst = LoadInstanceConstant(payload.InstanceIndex);
    MeshConstant meshInstance = LoadMeshConstant(inst.MeshBufferIdx);

    StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
    GroupDataLocation groupDataLocation = groupDataLocationSRV[payload.GetGroupIndex()];
    uint groupByteOffset = groupDataLocation.ByteOffset;
    ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
    MeshletHeader meshletHeader = geometryChunksBuffer.Load<MeshletHeader>(
        groupByteOffset + sizeof(GroupHeader) + payload.GetMeshletIndex() * sizeof(MeshletHeader));

    MaterialConstant mat = LoadMaterialConstant(meshletHeader.GetMaterialBufferIndex());

    uint vertexCount = meshletHeader.GetVertexCount();
    uint primitiveCount = meshletHeader.GetTriangleCount();

    uint vertexByteOffset = groupByteOffset + meshletHeader.VertexOffset;
    uint indexByteOffset = groupByteOffset + meshletHeader.TriangleOffset;
    uint vertexStride = meshletHeader.GetVertexStride();

    float3x3 worldRotationScale = (float3x3)meshInstance.WorldMatrix;
    float detWorld = determinant(worldRotationScale);
    bool isWorldFlipped = detWorld < 0.0;
    bool twoSided = (meshletHeader.GetPSOFlags() & PSO_TWO_SIDED) > 0;

    if (gtid == 0)
    {
        s_PrimCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // --------------------------------------------------------
    // Primitive culling + wave compaction
    // --------------------------------------------------------
    [unroll]
    for (uint j = 0; j < MAX_PRIMS; j += MS_GROUP_SIZE)
    {
        const uint primitiveID = j + gtid;
        bool inRange = primitiveID < primitiveCount;
        bool isVisible = false;
        uint3 triIndices = uint3(0, 0, 0);

        if (inRange)
        {
            triIndices = LoadAndUnpackTriangle(geometryChunksBuffer, indexByteOffset, primitiveID);

            float4 h0 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.x, meshInstance.WorldMatrix, g_VBufferMesh.ViewProjMatrix);
            float4 h1 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.y, meshInstance.WorldMatrix, g_VBufferMesh.ViewProjMatrix);
            float4 h2 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.z, meshInstance.WorldMatrix, g_VBufferMesh.ViewProjMatrix);

            bool logicalFrontFacing = IsFrontFacing(h0, h1, h2) != isWorldFlipped;
            isVisible = twoSided || logicalFrontFacing;

            if (isVisible)
            {
                isVisible = !IsTriangleOutsideFrustum(h0, h1, h2);
            }

            //if (isVisible)
            //{
            //    float minW = min(h0.w, min(h1.w, h2.w));
            //    if (minW > 0.0f)
            //    {
            //        isVisible = !IsTinyTriangle(h0, h1, h2);
            //    }
            //}
        }

        uint outIndex = 0;
        WaveInterlockedAddScalar(s_PrimCount, isVisible, 1, outIndex);

        if (inRange)
        {
            s_PrimOutIndex[primitiveID] = isVisible ? outIndex : 0xFFFFFFFFu;
            s_PrimIndices[primitiveID] = triIndices;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint finalPrimCount = s_PrimCount;
    SetMeshOutputCounts(vertexCount, finalPrimCount);

    // --------------------------------------------------------
    // Vertex processing
    // --------------------------------------------------------

    [unroll]
    for (uint i = 0; i < MAX_VERTS; i += MS_GROUP_SIZE)
    {
        const uint vertexID = i + gtid;
        if (vertexID < vertexCount)
        {
            verts[vertexID] = BuildVertexOutput(
                geometryChunksBuffer,
                meshletHeader,
                mat,
                meshInstance,
                vertexByteOffset,
                vertexStride,
                vertexID,
                commandIndex);
        }
    }

    // --------------------------------------------------------
    // Primitive emit
    // --------------------------------------------------------
    [unroll]
    for (uint j = 0; j < MAX_PRIMS; j += MS_GROUP_SIZE)
    {
        const uint primitiveID = j + gtid;
        if (primitiveID < primitiveCount)
        {
            uint outIndex = s_PrimOutIndex[primitiveID];
            if (outIndex != 0xFFFFFFFFu)
            {
                outIndices[outIndex] = s_PrimIndices[primitiveID];
                sharedPrimitives[outIndex].PrimitiveIndex = primitiveID;
            }
        }
    }
}

[shader("fragment")]
void pixelMain(MeshVertexOutput input, nointerpolation uint primID: TEXCOORD3)
{
    uint matFlags = (input.PackedMaterialInfo >> 16) & 0xFF;

    // processed in dag cull
    //if (matFlags & MAT_FLAG_ALPHA_BLEND)
    //{
    //    discard;
    //}

    if ((matFlags & MAT_FLAG_ALPHA_TEST) != 0)
    {
        const uint materialIndex = input.PackedMaterialInfo & 0xFFFF;
        MaterialConstant materialConstant = LoadMaterialConstant(materialIndex);

        float4 baseColorFactor = materialConstant.baseColorFactor;
        uint textureStartIndex = materialConstant.TextureStartIndex;
        uint samplerStartIndex = materialConstant.SamplerStartIndex;

        Texture2D<float4> baseColorTexture = GetResourceDescriptorHandle<Texture2D<float4>>(textureStartIndex);
        SamplerState baseColorSampler = GetSamplerDescriptor(samplerStartIndex);
        float alpha = baseColorFactor.a * baseColorTexture.Sample(baseColorSampler, input.UV0).a;
        float cutoff = f16tof32(materialConstant.flags >> MAT_FLAG_ALPHA_REF_SHIFT);
        if (alpha < cutoff)
            discard;
    }

    uint64_t packed = (((uint64_t)asuint(input.Position.z)) << 32) |
        ((input.CommandIndex & 0x01FFFFFF) << 7) | (primID & 0x7F);

    RWTexture2D<uint64_t> vbufferUAV = GetVBufferUAV();
    InterlockedMax(vbufferUAV[uint2(input.Position.xy)], packed);
}

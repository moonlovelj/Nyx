#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"
#include "GeometryCommon.hlsli"
#include "WaveUtil.hlsli"
#include "CullCommon.hlsli"

#define MAX_VERTS 128
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 32

groupshared uint s_PrimCount;
groupshared uint s_PrimOutIndex[MAX_PRIMS];
groupshared uint3 s_PrimIndices[MAX_PRIMS];

#ifdef UBER_MESH_SHADER_DEPTH_ONLY_PASS
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    nointerpolation uint packedMaterialInfo : TEXCOORD1;
};
#else
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
    nointerpolation uint packedMaterialInfo : TEXCOORD2;
};
#endif

#ifndef UBER_MESH_SHADER_DEPTH_ONLY_PASS
struct PrimitiveAttributes
{
    uint primitiveIndex : SV_PrimitiveID;
};
#endif

inline float4 GetClipPosition(ByteAddressBuffer geometryChunksBuffer,
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

inline float2 GetScreenPos(float4 clipPos)
{
    float2 ndc = clipPos.xy / clipPos.w;
    return (ndc * 0.5f + 0.5f) * float2((float)ViewportWidth, (float)ViewportHeight);
}

inline bool IsTriangleOutsideFrustum(float4 h0, float4 h1, float4 h2)
{
    uint cullBits = ComputeHomogeneousClipMask(h0) & ComputeHomogeneousClipMask(h1) & ComputeHomogeneousClipMask(h2);
    return cullBits != 0;
}

inline bool IsTinyTriangle(float4 h0, float4 h1, float4 h2)
{
    return false;
    float2 a = GetScreenPos(h0);
    float2 b = GetScreenPos(h1);
    float2 c = GetScreenPos(h2);

    float2 pixelMin = min(a, min(b, c));
    float2 pixelMax = max(a, max(b, c));

    const float epsilon = 1.0f / 256.0f;
    pixelMin -= epsilon;
    pixelMax += epsilon;

    int2 ipMin = int2(round(pixelMin));
    int2 ipMax = int2(round(pixelMax));

    return (ipMin.x == ipMax.x) && (ipMin.y == ipMax.y);
}

[RootSignature(Renderer_RootSig)]
[NumThreads(MS_GROUP_SIZE, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out vertices VSOutput verts[MAX_VERTS],
    out indices uint3 outIndices[MAX_PRIMS]
#ifndef UBER_MESH_SHADER_DEPTH_ONLY_PASS
    ,out primitives PrimitiveAttributes sharedPrimitives[MAX_PRIMS]
#endif
)
{
#ifdef UBER_MESH_SHADER_PASS1
    StructuredBuffer<QueueState> taskStateSRV = GetTaskQueueStateBufferSRV();
    const uint commandStart = taskStateSRV[0].PassState[0].VisibleMeshletCount;
#else
    const uint commandStart = 0;
#endif
    uint commandIndex = gid + commandStart;
    VisibleMeshletPayload payload = GetVisibleMeshletPayload(commandIndex);
    InstanceConstant inst = GetInstanceConstantSRV(payload.InstanceIndex);
    MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);
    
    StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
    GroupDataLocation groupDataLocation = groupDataLocationSRV[payload.GetGroupIndex()];
    uint groupByteOffset = groupDataLocation.ByteOffset;
    ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
    MeshletHeader meshletHeader = geometryChunksBuffer.Load<MeshletHeader>(
        groupByteOffset + sizeof(GroupHeader) + payload.GetMeshletIndex() * sizeof(MeshletHeader));
    
    MaterialConstant mat = GetMaterialConstantSRV(meshletHeader.GetMaterialBufferIndex());
    
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

            float4 h0 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.x, meshInstance.WorldMatrix, ViewProjMatrix);
            float4 h1 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.y, meshInstance.WorldMatrix, ViewProjMatrix);
            float4 h2 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.z, meshInstance.WorldMatrix, ViewProjMatrix);

            bool logicalFrontFacing = IsFrontFacing(h0, h1, h2) ^ isWorldFlipped;
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
            uint vertexOffset = vertexByteOffset + vertexStride * vertexID;
            float4 position = float4(asfloat(geometryChunksBuffer.Load3(vertexOffset)), 1.0);
            vertexOffset += 12;

            uint psoFlags = meshletHeader.GetPSOFlags();
            float2 uv0 = 0;
            if (psoFlags & PSO_ALPHA_TEST)
            {
                uint uvLoadOffset = vertexOffset + 4; // normal
                if (psoFlags & PSO_HAS_TANGENT)
                    uvLoadOffset += 4; // tangent

                if ((mat.flags & MAT_FLAG_BASE_COLOR_UV) && (psoFlags & PSO_HAS_UV1))
                {
                    if (psoFlags & PSO_HAS_UV0)
                        uvLoadOffset += 4;
                    uint PackedUV = geometryChunksBuffer.Load(uvLoadOffset);
                    uv0 = DecodeR16G16FLOATToFloat2(PackedUV);

                }
                else if (psoFlags & PSO_HAS_UV0)
                {
                    uint PackedUV = geometryChunksBuffer.Load(uvLoadOffset);
                    uv0 = DecodeR16G16FLOATToFloat2(PackedUV);
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
            float4 clipPos = mul(ViewProjMatrix, float4(worldPos, 1.0));

            verts[vertexID].position = clipPos;
#ifndef UBER_MESH_SHADER_DEPTH_ONLY_PASS
            verts[vertexID].commandIndex = commandIndex;
#endif
            uint packedMaterialInfo = (meshletHeader.GetMaterialBufferIndex() & 0xFFFF)
                                    | ((mat.flags & 0xFF) << 16);

            verts[vertexID].packedMaterialInfo = packedMaterialInfo;
            verts[vertexID].uv0 = uv0;

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
#ifndef UBER_MESH_SHADER_DEPTH_ONLY_PASS
                sharedPrimitives[outIndex].primitiveIndex = primitiveID;
#endif
            }
        }
    }
}

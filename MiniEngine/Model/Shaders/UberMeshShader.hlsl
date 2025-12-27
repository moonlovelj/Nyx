#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"
#include "GeometryCommon.hlsli"

#define MAX_VERTS 256
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 128

struct VSOutput
{
    float4 position : SV_POSITION;
    //float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
};

struct PrimitiveAttributes
{
    uint primitiveIndex : SV_PrimitiveID;
};

[RootSignature(Renderer_RootSig)]
[NumThreads(MS_GROUP_SIZE, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out vertices VSOutput verts[MAX_VERTS],
    out indices uint3 outIndices[MAX_PRIMS],
    out primitives PrimitiveAttributes sharedPrimitives[MAX_PRIMS])
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
    SetMeshOutputCounts(vertexCount, primitiveCount);
    
    uint vertexByteOffset = groupByteOffset + meshletHeader.VertexOffset;
    uint indexByteOffset = groupByteOffset + meshletHeader.TriangleOffset;
    uint vertexStride = meshletHeader.GetVertexStride();
    
    // --------------------------------------------------------
    // 顶点处理 (Vertex Processing)
    // --------------------------------------------------------
    
    // 每个线程最多处理两个顶点
    [unroll]
    for (uint i = 0; i < 2; ++i)
    {
        uint localVertexIdx = gtid * 2 + i;
        if (localVertexIdx < vertexCount)
        {
            // 顶点拉取
            uint vertexOffset = vertexByteOffset + vertexStride * localVertexIdx;

            // Position (总是存在)
            float4 position = float4(asfloat(geometryChunksBuffer.Load3(vertexOffset)), 1.0);
            vertexOffset += 12;

            //float2 uv0 = 0;
            //if (mlet.psoFlags & PSO_ALPHA_TEST)
            //{
            //    uint PackedUV = geometryData.Load(vertexOffset);
            //    uv0 = DecodeR16G16FLOATToFloat2(PackedUV);
            //    vertexOffset += 4;
            //}

            // Skinning (动态分支)
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

            float4x4 WorldMatrix = meshInstance.WorldMatrix;
            float4x3 WorldIT = meshInstance.WorldIT;
            float3 worldPos = mul(WorldMatrix, position).xyz;
            verts[localVertexIdx].position = mul(ViewProjMatrix, float4(worldPos, 1.0));
            //verts[localVertexIdx].uv0 = uv0;
            verts[localVertexIdx].commandIndex = commandIndex;
        }
    }
    
    // --------------------------------------------------------
    // 图元处理 (Primitive Processing)
    // --------------------------------------------------------
    if (gtid < primitiveCount)
    {
        uint3 triIndices = LoadAndUnpackTriangle(geometryChunksBuffer, indexByteOffset, gtid);

        outIndices[gtid] = triIndices;
        sharedPrimitives[gtid].primitiveIndex = gtid;
    }
}
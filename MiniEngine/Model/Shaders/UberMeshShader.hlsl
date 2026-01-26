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
    float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
    nointerpolation uint packedMaterialInfo : TEXCOORD2;
};

struct PrimitiveAttributes
{
    uint primitiveIndex : SV_PrimitiveID;
};

//groupshared float4 s_ClipPositions[MAX_VERTS];
//groupshared uint s_VisiblePrimitiveCount;

bool isFrontFacingHW(float4 ha, float4 hb, float4 hc)
{
    return determinant(float3x3(ha.xyw, hb.xyw, hc.xyw)) >= 0;
}

float4 GetClipPosition(ByteAddressBuffer geometryChunksBuffer, 
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
    //if (gtid == 0)
    //{
    //    s_VisiblePrimitiveCount = 0;
    //}
    
    //GroupMemoryBarrierWithGroupSync();
    
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
    
    SetMeshOutputCounts(vertexCount, primitiveCount);
    
    // --------------------------------------------------------
    // 顶点处理 (Vertex Processing)
    // --------------------------------------------------------
    
    // 每个线程最多处理两个顶点
    // TODO 这里可以尝试更好的分配线程负载
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
            //float4x3 WorldIT = meshInstance.WorldIT;
            float3 worldPos = mul(WorldMatrix, position).xyz;
            float4 clipPos = mul(ViewProjMatrix, float4(worldPos, 1.0));
            
            //s_ClipPositions[localVertexIdx] = clipPos;
            
            verts[localVertexIdx].position = clipPos;
            verts[localVertexIdx].commandIndex = commandIndex;
            uint packedMaterialInfo = (meshletHeader.GetMaterialBufferIndex() & 0xFFFF)
                                    | ((mat.flags & 0xFF) << 16);
                                    
            verts[localVertexIdx].packedMaterialInfo = packedMaterialInfo;
            verts[localVertexIdx].uv0 = uv0;

        }
    }
    
    //GroupMemoryBarrierWithGroupSync();
    
    // --------------------------------------------------------
    // 图元处理 (Primitive Processing)
    // --------------------------------------------------------
    //bool isVisible = false;
    //uint3 triIndices;
    if (gtid < primitiveCount)
    {
        uint3 triIndices = LoadAndUnpackTriangle(geometryChunksBuffer, indexByteOffset, gtid);
        
        // 这里重新Load并且计算，要比使用group shared memory性能好
        float4 h0 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.x, meshInstance.WorldMatrix, ViewProjMatrix);
        float4 h1 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.y, meshInstance.WorldMatrix, ViewProjMatrix);
        float4 h2 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.z, meshInstance.WorldMatrix, ViewProjMatrix);

        // 背面剔除
        bool twoSided = (meshletHeader.GetPSOFlags() & PSO_TWO_SIDED) > 0;
        bool isVisible = twoSided || isFrontFacingHW(h0, h1, h2);
        
        outIndices[gtid] = isVisible ? triIndices : uint3(0, 0, 0);
        sharedPrimitives[gtid].primitiveIndex = gtid;
    }
    
    //uint4 ballot = WaveActiveBallot(isVisible);
    //uint waveOffset = WavePrefixCountBits(isVisible);
    //uint waveTotalVisible = countbits(ballot.x) + countbits(ballot.y) + countbits(ballot.z) + countbits(ballot.w);

    //uint threadGroupOffset;
    //if (WaveIsFirstLane())
    //{
    //    InterlockedAdd(s_VisiblePrimitiveCount, waveTotalVisible, threadGroupOffset);
    //}
    
    //threadGroupOffset = WaveReadLaneFirst(threadGroupOffset);

    //GroupMemoryBarrierWithGroupSync();
    
    //SetMeshOutputCounts(vertexCount, s_VisiblePrimitiveCount);
    
    // // 写入顶点输出
    //[unroll]
    //for (uint j = 0; j < 2; ++j)
    //{
    //    uint vIdx = gtid * 2 + j;
    //    if (vIdx < vertexCount)
    //    {
    //        verts[vIdx].position = s_ClipPositions[vIdx];
    //        verts[vIdx].commandIndex = commandIndex;
    //    }
    //}

    //// 写入图元输出 (流压缩后的位置)
    //if (isVisible)
    //{
    //    uint finalOutIndex = threadGroupOffset + waveOffset;
    //    outIndices[finalOutIndex] = triIndices;
    //    sharedPrimitives[finalOutIndex].primitiveIndex = gtid;
    //}
}
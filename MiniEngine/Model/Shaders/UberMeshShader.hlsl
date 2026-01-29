#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"
#include "GeometryCommon.hlsli"

#define MAX_VERTS 128
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 32

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

inline bool isFrontFacingHW(float4 ha, float4 hb, float4 hc)
{
    return determinant(float3x3(ha.xyw, hb.xyw, hc.xyw)) >= 0;
}

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

    uint vertexByteOffset = groupByteOffset + meshletHeader.VertexOffset;
    uint indexByteOffset = groupByteOffset + meshletHeader.TriangleOffset;
    uint vertexStride = meshletHeader.GetVertexStride();
    
    SetMeshOutputCounts(vertexCount, primitiveCount);
    
    // --------------------------------------------------------
    // 顶点处理 (Vertex Processing)
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

            float3 worldPos = mul(meshInstance.WorldMatrix, position).xyz;
            float4 clipPos = mul(ViewProjMatrix, float4(worldPos, 1.0));
            
            verts[vertexID].position = clipPos;
            verts[vertexID].commandIndex = commandIndex;
            uint packedMaterialInfo = (meshletHeader.GetMaterialBufferIndex() & 0xFFFF)
                                    | ((mat.flags & 0xFF) << 16);
                                    
            verts[vertexID].packedMaterialInfo = packedMaterialInfo;
            verts[vertexID].uv0 = uv0;

        }
    }

    // --------------------------------------------------------
    // 图元处理 (Primitive Processing)
    // --------------------------------------------------------
    [unroll]
    for (uint j = 0; j < MAX_PRIMS; j += MS_GROUP_SIZE)
    {
        const uint primitiveID = j + gtid;
        if (primitiveID < primitiveCount)
        {
            uint3 triIndices = LoadAndUnpackTriangle(geometryChunksBuffer, indexByteOffset, primitiveID);
        
            // 这里重新Load并且计算，要比使用group shared memory性能好
            float4 h0 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.x, meshInstance.WorldMatrix, ViewProjMatrix);
            float4 h1 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.y, meshInstance.WorldMatrix, ViewProjMatrix);
            float4 h2 = GetClipPosition(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.z, meshInstance.WorldMatrix, ViewProjMatrix);

            float3x3 worldRotationScale = (float3x3) meshInstance.WorldMatrix;
            float detWorld = determinant(worldRotationScale);
            bool isWorldFlipped = detWorld < 0.0;
        
            // 背面剔除
            bool twoSided = (meshletHeader.GetPSOFlags() & PSO_TWO_SIDED) > 0;
            bool logicalFrontFacing = isFrontFacingHW(h0, h1, h2) ^ isWorldFlipped;
            bool isVisible = twoSided || logicalFrontFacing;
        
            outIndices[primitiveID] = isVisible ? triIndices : uint3(0, 0, 0);
            sharedPrimitives[primitiveID].primitiveIndex = primitiveID;
        }
    }
}
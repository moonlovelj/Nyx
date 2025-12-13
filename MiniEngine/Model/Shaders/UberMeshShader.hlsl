#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"

#define MAX_VERTS 256
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 128

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float3 sunShadowCoord : TEXCOORD3;
    uint meshletIndex : TEXCOORD4;
};

[RootSignature(Renderer_RootSig)]
[NumThreads(MS_GROUP_SIZE, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out vertices VSOutput verts[MAX_VERTS],
    out indices uint3 outIndices[MAX_PRIMS])
{
    IndirectCommand command = GetIndirectCullingResultsBufferSRV(PsoIdx, gid);
    MeshletConstant mlet = GetMeshletConstantSRV(command.MeshletIndex);
    InstanceConstant inst = GetInstanceConstantSRV(command.InstanceIndex);
    MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshConstantsBase + mlet.MeshConstantsIndexOffset);
    MaterialConstant mat = GetMaterialConstantSRV(mlet.MaterialConstantsIndex);
    
    SetMeshOutputCounts(mlet.VertexCount, mlet.PrimitiveCount);
    
    ByteAddressBuffer geometryData = GetGeometryBufferSRV();
    // --------------------------------------------------------
    // 阶段 A: 顶点处理 (Vertex Processing)
    // --------------------------------------------------------
    
    // 每个线程最多处理两个顶点
    for (uint i = 0; i < 2; ++i)
    {
        uint localVertexIdx = gtid * 2 + i;
        if (localVertexIdx < mlet.VertexCount)
        {
            // (Local Index -> Mesh Vertex Index)
            uint globalVertexIdx = geometryData.Load(mlet.MeshletVerticesOffset + localVertexIdx * 4);

            // 顶点拉取
            uint vertexOffset = mlet.VertexBufferOffset + globalVertexIdx * mlet.VertexStride;

            // Position (总是存在)
            float4 position = float4(asfloat(geometryData.Load3(vertexOffset)), 1.0);
            vertexOffset += 12;

            // Normal (总是存在)
            uint packedNormal = geometryData.Load(vertexOffset);
            float3 normal = DecodeR10G10B10A2UNORMToFloat4(packedNormal).xyz * 2.0 - 1.0;
            vertexOffset += 4;
        
            float4 tangent = float4(1, 0, 0, 1);
            if (mlet.psoFlags & PSO_HAS_TANGENT)
            {
                uint packedTangent = geometryData.Load(vertexOffset);
                tangent = DecodeR10G10B10A2UNORMToFloat4(packedTangent) * 2.0 - 1.0;
                vertexOffset += 4;
            }

            // UV (总是存在)
            uint packedUV = geometryData.Load(vertexOffset);
            float2 uv0 = DecodeR16G16FLOATToFloat2(packedUV);
            vertexOffset += 4;
            
            float2 uv1 = 0;
            if (mlet.psoFlags & PSO_HAS_UV1)
            {
                uint packedUV1 = geometryData.Load(vertexOffset);
                uv1 = DecodeR16G16FLOATToFloat2(packedUV1);
                vertexOffset += 4;
            }

            // Skinning (动态分支)
            if (mlet.psoFlags & PSO_HAS_SKIN)
            {
                uint2 PackedJointIndices = geometryData.Load2(vertexOffset);
                vertexOffset += 8;
                uint2 PackedWeights = geometryData.Load2(vertexOffset);
                vertexOffset += 8;
    
                uint4 jointIndices = DecodeR16G16B16A16UINTToUint4(PackedJointIndices);
                float4 jointWeights = DecodeR16G16B16A16UNORMToFloat4(PackedWeights);
    
                // I don't like this hack.  The weights should be normalized already, but something is fishy.
                float4 weights = jointWeights / dot(jointWeights, 1);

                float4x4 skinPosMat =
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.x).PosMatrix * weights.x +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.y).PosMatrix * weights.y +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.z).PosMatrix * weights.z +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.w).PosMatrix * weights.w;

                position = mul(skinPosMat, position);

                float4x3 skinNrmMat =
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.x).NrmMatrix * weights.x +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.y).NrmMatrix * weights.y +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.z).NrmMatrix * weights.z +
                    GetJointBufferSRV(inst.JointBase + mlet.MeshJointsIndexOffset + jointIndices.w).NrmMatrix * weights.w;

                normal = mul(skinNrmMat, normal).xyz;
                
                if (mlet.psoFlags & PSO_HAS_TANGENT)
                {
                    tangent.xyz = mul(skinNrmMat, tangent.xyz).xyz;
                }
            }

            float4x4 WorldMatrix = meshInstance.WorldMatrix;
            float4x3 WorldIT = meshInstance.WorldIT;
            float3 worldPos = mul(WorldMatrix, position).xyz;
            verts[localVertexIdx].worldPos = worldPos;
            verts[localVertexIdx].position = mul(ViewProjMatrix, float4(worldPos, 1.0));
            verts[localVertexIdx].sunShadowCoord = mul(SunShadowMatrix, float4(worldPos, 1.0)).xyz;
            verts[localVertexIdx].normal = mul(WorldIT, normal).xyz;
            verts[localVertexIdx].tangent = float4(mul(WorldIT, tangent.xyz).xyz, tangent.w);
            verts[localVertexIdx].uv0 = uv0;
            verts[localVertexIdx].uv1 = uv1;
            verts[localVertexIdx].meshletIndex = command.MeshletIndex;
        }
    }
    
    // --------------------------------------------------------
    // 阶段 B: 图元处理 (Primitive Processing)
    // --------------------------------------------------------
    if (gtid < mlet.PrimitiveCount)
    {
        uint packedTri = geometryData.Load(mlet.MeshletPrimitivesOffset + gtid * 4);

        // 解包 (3 x 8 bits)
        uint i0 = packedTri & 0xFF;
        uint i1 = (packedTri >> 8) & 0xFF;
        uint i2 = (packedTri >> 16) & 0xFF;

        outIndices[gtid] = uint3(i0, i1, i2);
    }
}
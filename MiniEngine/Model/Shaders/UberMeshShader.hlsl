#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"

#define MAX_VERTS 256
#define MAX_PRIMS 128
#define MS_GROUP_SIZE 128

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
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
    uint commandIndex = GetIndirectCullingResultsBufferSRV(PsoIdx, gid);
    IndirectCommand command = GetIndirectCommandsBufferSRV(PsoIdx, commandIndex);
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
    [unroll]
    for (uint i = 0; i < 2; ++i)
    {
        uint localVertexIdx = gtid * 2 + i;
        if (localVertexIdx < mlet.VertexCount)
        {
            // (Local Index -> Mesh Vertex Index)
            uint globalVertexIdx = geometryData.Load(mlet.MeshletVerticesOffset + localVertexIdx * 4);

            // 顶点拉取
            uint vertexOffset = mlet.VertexBufferDepthOffset + globalVertexIdx * mlet.VertexDepthStride;

            // Position (总是存在)
            float4 position = float4(asfloat(geometryData.Load3(vertexOffset)), 1.0);
            vertexOffset += 12;

            float2 uv0 = 0;
            if (mlet.psoFlags & PSO_ALPHA_TEST)
            {
                uint PackedUV = geometryData.Load(vertexOffset);
                DecodeR16G16FLOATToFloat2(PackedUV);
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
            }

            float4x4 WorldMatrix = meshInstance.WorldMatrix;
            float4x3 WorldIT = meshInstance.WorldIT;
            float3 worldPos = mul(WorldMatrix, position).xyz;
            verts[localVertexIdx].position = mul(ViewProjMatrix, float4(worldPos, 1.0));
            verts[localVertexIdx].uv0 = uv0;
            verts[localVertexIdx].commandIndex = commandIndex;
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
        sharedPrimitives[gtid].primitiveIndex = gtid;
    }
}
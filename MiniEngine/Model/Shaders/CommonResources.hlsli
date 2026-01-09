#ifndef __COMMON_RESOURCES_HLSLI__
#define __COMMON_RESOURCES_HLSLI__

#include "BindlessIndices.hlsli"
#include "../StructsIO.h"
#include "../MeshletStructs.h"

#define PSO_HAS_POSITION      0x0001
#define PSO_HAS_NORMAL        0x0002
#define PSO_HAS_TANGENT       0x0004
#define PSO_HAS_UV0           0x0008
#define PSO_HAS_UV1           0x0010
#define PSO_ALPHA_BLEND       0x0020
#define PSO_ALPHA_TEST        0x0040
#define PSO_TWO_SIDED         0x0080
#define PSO_HAS_SKIN          0x0100

#define PSO_IDX_SHADOW        0
#define PSO_IDX_MAIN          1
#define PSO_IDX_TRANSPARENT   2

cbuffer GlobalConstants : register(b2)
{
    float4x4 ViewMatrix;
    float4 ViewSpaceFrustumPlanes0;
    float4 ViewSpaceFrustumPlanes1;
    float4 ViewSpaceFrustumPlanes2;
    float4 ViewSpaceFrustumPlanes3;
    float4 ViewSpaceFrustumPlanes4;
    float4 ViewSpaceFrustumPlanes5;
    float4x4 ViewProjMatrix;
    float4x4 ProjMatrix;
    float4x4 InverseViewProjMatrix;
    float4x4 PrevViewMatrix;
    float4x4 PrevViewProjMatrix;
    float4x4 PrevProjMatrix;
    float4x4 SunShadowMatrix;
    float3 ViewerPos;
    float3 PrevViewerPos;
    float3 SunDirection;
    float3 SunIntensity;
    float4 ShadowTexelSize;

    float4 InvTileDim;
    float4 HZBSizeAndInv;
    uint4 TileCount;
    uint4 FirstLightIndex;

    uint ViewportWidth;
    uint ViewportHeight;

    uint FrameIndexMod2;

    uint IBLLutTextureSize;
    uint IBLSpecularLDMapMipCount;

    uint BindlessResourcesBaseIndex;
}

cbuffer CommandConstants : register(b3)
{
    uint MaxCommands;
    float ScreenErrorConstant; // 计算meshlet屏幕误差时使用的提前计算的常量 (cotHalfFov * screenHeight) / 2.0
    uint PsoIdx;
    uint CullingStage;
};

cbuffer CB4 : register(b5)
{
    uint ViewMode;
}

struct InstanceConstant
{
    float4 BoundingSphere; // Local space bounding sphere
    uint MeshBufferIdx;
    uint JointBufferIdx;
};

struct MeshletConstant
{
    float4 BoundingSphere;
    uint VertexBufferOffset;
    uint VertexStride;
    uint VertexBufferDepthOffset;
    uint VertexDepthStride;

    uint32_t MeshletVerticesOffset;   // 指向该 Meshlet 的顶点索引列表 (uint32)
    uint32_t MeshletPrimitivesOffset; // 指向该 Meshlet 的三角形列表
    uint32_t VertexCount;             // Meshlet 唯一顶点数 (Max 256)
    uint32_t PrimitiveCount;          // Meshlet 三角形数 (Max 128)

    uint IndexBufferOffset;
    uint MeshConstantsIndexOffset;
    uint MaterialConstantsIndex;
    uint MeshJointsIndexOffset;

    // Nanite LOD 数据
    float4   parentBounds;     // 父层级包围球
    float4   lodBounds;        // 当前层级包围球
    float    parentError;      // 父层级简化误差（Infinity = 根节点）
    float    lodError;	       // 当前层级简化误差
    uint     lodLevel;         // 当前 LOD 层级（0 = 最精细）
    uint     psoFlags;
};

// 实际是Instance级别
struct MeshConstant
{
    float4x4 WorldMatrix;
    float4x3 WorldIT; // Inverse-transpose of PosMatrix
};

struct MaterialConstant
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
    uint TextureStartIndex;
    uint SamplerStartIndex;
};

struct Joint
{
    float4x4 PosMatrix;
    float4x3 NrmMatrix; // Inverse-transpose of PosMatrix
};
Joint GetJointBufferSRV(uint index)
{
    ByteAddressBuffer JointBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_JOINTS_BUFFER];
    return JointBuffer.Load<Joint>(index * sizeof(Joint));
}

struct IndirectCommand
{
    uint InstanceIndex;
    uint MeshletIndex;
};

struct DispatchMeshCommand
{
    uint ThreadGroupCountX;
    uint ThreadGroupCountY;
    uint ThreadGroupCountZ;
};

struct LightData
{
    float3 pos;
    float radiusSq;

    float3 color;
    uint type;

    float3 coneDir;
    float2 coneAngles; // x = 1.0f / (cos(coneInner) - cos(coneOuter)), y = cos(coneOuter)

    float4x4 shadowTextureMatrix;
};

MeshletConstant GetMeshletConstantSRV(uint index)
{
	ByteAddressBuffer MeshletConstantBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_MESHLET_BUFFER];
	return MeshletConstantBuffer.Load<MeshletConstant>(index * sizeof(MeshletConstant));
}

MeshConstant GetMeshConstantSRV(uint index)
{
    ByteAddressBuffer MeshConstantBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_MESH_CONSTANTS_BUFFER];
    return MeshConstantBuffer.Load<MeshConstant>(index * sizeof(MeshConstant));
}

MaterialConstant GetMaterialConstantSRV(uint index)
{
	ByteAddressBuffer MaterialConstantBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_MATERIAL_CONSTANTS_BUFFER];
	return MaterialConstantBuffer.Load<MaterialConstant>(index * sizeof(MaterialConstant));
}

InstanceConstant GetInstanceConstantSRV(uint index)
{
    ByteAddressBuffer InstanceConstantBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INSTANCE_CONSTANTS_BUFFER];
    return InstanceConstantBuffer.Load<InstanceConstant>(index * sizeof(InstanceConstant));
}

ByteAddressBuffer GetVertexBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_VERTEX_BUFFER];
}

ByteAddressBuffer GetIndexBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INDEX_BUFFER];
}

ByteAddressBuffer GetGeometryBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GEOMETRY_BUFFER];
}

Texture2D<uint2> GetVBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_VBUFFER];
}

Texture2D<float4> GetGBufferASRV()
{
	return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GBUFFER_A];
}

Texture2D<float4> GetGBufferBSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GBUFFER_B];
}

Texture2D<float4> GetGBufferCSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GBUFFER_C];
}

Texture2D<float4> GetGBufferDSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GBUFFER_D];
}

Texture2D<float4> GetSceneColorSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SCENE_COLOR];
}

Texture2D<float> GetSceneDepthSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SCENE_DEPTH];
}

Texture2D<float> GetPrevSceneHZBSRV(uint frameIndexMod2)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SCENE_HZB0 + 1 - frameIndexMod2];
}

Texture2D<float> GetCurrentSceneHZBSRV(uint frameIndexMod2)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SCENE_HZB0 + frameIndexMod2];
}

TextureCube<float3> GetIBLDiffuseLDSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_IBL_DIFFUSE_LD];
}

TextureCube<float3> GetIBLSpecularLDSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_IBL_SPECULAR_LD];
}

Texture2D<float4> GetIBLLutSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_IBL_LUT];
}

TextureCube<float3> GetIBLCubeMapSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_IBL_CUBE_MAP];
}

Texture2D<float> GetSSAOSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SSAO];
}

Texture2D<float> GetShadowMapSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_SHADOW_MAP];
}

Texture2DArray<float> GetLightShadowArraySRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_LIGHT_SHADOW_ARRAY];
}

ByteAddressBuffer GetLightGridSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_LIGHT_GRID];
}

ByteAddressBuffer GetLightGridMaskSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_LIGHT_GRID_MASK];
}

StructuredBuffer<LightData> GetLightBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_LIGHT_BUFFER];
}

// bufferOffset = psoContinuousIdx
IndirectCommand GetIndirectCommandsBufferSRV(uint bufferOffset, uint index)
{
    ByteAddressBuffer indirectCommandBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INDIRECT_COMMANDS_BASE + bufferOffset];
    return indirectCommandBuffer.Load<IndirectCommand>(index * sizeof(IndirectCommand));
}

// bufferOffset = psoContinuousIdx
ByteAddressBuffer GetIndirectVisibleFlagsBufferSRV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INDIRECT_VISIBLE_FLAGS_BASE + bufferOffset];
}

// bufferOffset = psoContinuousIdx
uint GetIndirectCullingResultsBufferSRV(uint bufferOffset, uint index)
{
    StructuredBuffer<uint> cullingResultBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INDIRECT_CULLING_RESULTS_BASE + bufferOffset];
    return cullingResultBuffer[index];
}

ByteAddressBuffer GetIndirectCullingResultsCounterBufferSRV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_INDIRECT_CULLING_RESULTS_COUNTER_BASE + bufferOffset];
}

ByteAddressBuffer GetTaskQueueBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_TASK_QUEUE_BUFFER];
}

ByteAddressBuffer GetTaskQueueCounterBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_TASK_QUEUE_COUNTER_BUFFER];
}

StructuredBuffer<HierarchyNode> GetHierarchyNodesBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_HIERARCHY_NODES_BUFFER];
}

ByteAddressBuffer GetGeometryChunksBufferSRV(uint chunkOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GEOMETRY_CHUNK_DATA_BUFFER + chunkOffset];
}

StructuredBuffer<GroupDataLocation> GetGroupDataLocationBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_GROUP_DATA_LOCATION_BUFFER];
}

StructuredBuffer<VisibleMeshletPayload> GetVisibleMeshletBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_VISIBLE_MESHLET_BUFFER];
}

VisibleMeshletPayload GetVisibleMeshletPayload(uint index)
{
    return GetVisibleMeshletBufferSRV()[index];
}

StructuredBuffer<DrawItem> GetPotentialDrawItemBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_POTENTIAL_DRAW_ITEM_BUFFER];
}

StructuredBuffer<QueueState> GetTaskQueueStateBufferSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_TASK_QUEUE_STATE_BUFFER];
}


// ------------------------
// ------------------------
// UAV
RWTexture2D<float4> GetSceneColorUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_SCENE_COLOR];
}

RWTexture2D<float4> GetGBufferAUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_GBUFFER_A];
}

RWTexture2D<float4> GetGBufferBUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_GBUFFER_B];
}

RWTexture2D<float4> GetGBufferCUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_GBUFFER_C];
}

RWTexture2D<float4> GetGBufferDUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_GBUFFER_D];
}

// bufferOffset = psoIdx
RWByteAddressBuffer GetIndirectVisibleFlagsBufferUAV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_INDIRECT_VISIBLE_FLAGS_BASE + bufferOffset];
}

// bufferOffset = psoIdx
AppendStructuredBuffer<uint> GetIndirectCullingResultsBufferUAV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_INDIRECT_CULLING_RESULTS_BASE + bufferOffset];
}

RWByteAddressBuffer GetLightGridUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_LIGHT_GRID];
}

RWByteAddressBuffer GetLightGridBitMaskUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_LIGHT_GRID_MASK];
}

RWStructuredBuffer<DispatchMeshCommand> GetDispatchMeshBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_INDIRECT_DISPATCH_MESHES_BASE];
}

globallycoherent RWStructuredBuffer<QueueState> GetTaskQueueStateBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_TASK_QUEUE_STATE_BUFFER];
}

globallycoherent RWByteAddressBuffer GetTaskQueueBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_TASK_QUEUE_BUFFER];
}

globallycoherent RWByteAddressBuffer GetMeshletBatchBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_MESHLET_BATCH_BUFFER];
}

globallycoherent RWByteAddressBuffer GetCandidateMeshletBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_CANDIDATE_MESHLET_BUFFER];
}

RWStructuredBuffer<VisibleMeshletPayload> GetVisibleMeshletBufferUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_VISIBLE_MESHLET_BUFFER];
}

RWTexture2D<uint64_t> GetVBufferUAV()
{
	return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_VBUFFER];
}
#endif

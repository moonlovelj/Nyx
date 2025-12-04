#ifndef __COMMON_RESOURCES_HLSLI__
#define __COMMON_RESOURCES_HLSLI__

#include "BindlessIndices.hlsli"

cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewMatrix;
    float4 ViewSpaceFrustumPlanes0;
    float4 ViewSpaceFrustumPlanes1;
    float4 ViewSpaceFrustumPlanes2;
    float4 ViewSpaceFrustumPlanes3;
    float4 ViewSpaceFrustumPlanes4;
    float4 ViewSpaceFrustumPlanes5;
    float4x4 ViewProjMatrix;
    float4x4 InverseViewProjMatrix;
    float4x4 PrevViewProjMatrix;
    float4x4 SunShadowMatrix;
    float3 ViewerPos;
    float3 SunDirection;
    float3 SunIntensity;
    float4 ShadowTexelSize;

    float4 InvTileDim;
    uint4 TileCount;
    uint4 FirstLightIndex;

    uint ViewportWidth;
    uint ViewportHeight;

    uint FrameIndexMod2;

    uint IBLLutTextureSize;
    uint IBLSpecularLDMapMipCount;

    uint BindlessResourcesBaseIndex;
}

cbuffer CB2 : register(b2)
{
    uint InstanceIndex;
    uint MeshletIndex;
}

cbuffer CB4 : register(b4)
{
    uint ViewMode;
}


struct InstanceConstant
{
    uint MeshConstantsBase;
    uint JointBase;
};

struct MeshletConstant
{
    float4 BoundingSphere;
    uint VertexBufferOffset;
    uint VertexStride;
    uint VertexBufferDepthOffset;
    uint VertexDepthStride;
    uint MeshConstantsIndexOffset;
    uint MaterialConstantsIndex;
    uint MeshJointsIndexOffset;

    // Nanite LOD 数据
    float    parentError;      // 父层级简化误差（Infinity = 根节点）
    float4   parentBounds;     // 父层级包围球
    float4   lodBounds;        // 当前层级包围球
    float    lodError;	       // 当前层级简化误差
    uint     lodLevel;         // 当前 LOD 层级（0 = 最精细）
    float    padding1;
    float    padding2;
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

#ifdef ENABLE_SKINNING
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
#endif

struct IndirectCommand
{
    uint InstanceIndex;
    uint MeshletIndex;
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
    uint paddings;
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

// bufferOffset 必须是SRV_INDIRECT_SHADOW_BUFFER、SRV_INDIRECT_DEPTH_BUFFER、SRV_INDIRECT_COLOR_BUFFER
IndirectCommand GetIndirectCommandBufferSRV(uint bufferOffset, uint index)
{
    ByteAddressBuffer indirectCommandBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + bufferOffset];
    return indirectCommandBuffer.Load<IndirectCommand>(index * sizeof(IndirectCommand));
}

IndirectCommand GetIndirectCommandShadowSRV(uint index)
{
    return GetIndirectCommandBufferSRV(SRV_INDIRECT_SHADOW_BUFFER, index);
}

IndirectCommand GetIndirectCommandDepthSRV(uint index)
{
    return GetIndirectCommandBufferSRV(SRV_INDIRECT_DEPTH_BUFFER, index);
}

IndirectCommand GetIndirectCommandColorSRV(uint index)
{
    return GetIndirectCommandBufferSRV(SRV_INDIRECT_COLOR_BUFFER, index);
}

// bufferOffset = psoContinuousIdx
ByteAddressBuffer GetVisibleFlagSRV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_VISIBLE_FLAGS_BASE + bufferOffset];
}

// bufferOffset = psoContinuousIdx
IndirectCommand GetCullingResultSRV(uint bufferOffset, uint index)
{
	ByteAddressBuffer cullingResultBuffer = ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_CULLING_RESULT_BASE + bufferOffset];
	return cullingResultBuffer.Load<IndirectCommand>(index * sizeof(IndirectCommand));
}

// UAV
RWTexture2D<float4> GetSceneColorUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_SCENE_COLOR];
}

// bufferOffset = psoIdx
RWByteAddressBuffer GetVisibleFlagUAV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_VISIBLE_FLAGS_BASE + bufferOffset];
}

// bufferOffset = psoIdx
AppendStructuredBuffer<IndirectCommand> GetCullingResultUAV(uint bufferOffset)
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_CULLING_RESULT_BASE + bufferOffset];
}

RWByteAddressBuffer GetLightGridUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_LIGHT_GRID];
}

RWByteAddressBuffer GetLightGridBitMaskUAV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + UAV_LIGHT_GRID_MASK];
}

#endif

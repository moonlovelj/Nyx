//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

#ifndef NYX_COMMON_RESOURCES_SLANG
#define NYX_COMMON_RESOURCES_SLANG

#include "BindlessIndices.h.slang"
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

#define MAT_FLAG_BASE_COLOR_UV         0x00000001
#define MAT_FLAG_METALLIC_ROUGHNESS_UV 0x00000002
#define MAT_FLAG_OCCLUSION_UV          0x00000004
#define MAT_FLAG_EMISSIVE_UV           0x00000008
#define MAT_FLAG_NORMAL_UV             0x00000010

#define MAT_FLAG_TWO_SIDED             0x00000020
#define MAT_FLAG_ALPHA_TEST            0x00000040
#define MAT_FLAG_ALPHA_BLEND           0x00000080

#define MAT_FLAG_ALPHA_REF_MASK        0xFFFF0000
#define MAT_FLAG_ALPHA_REF_SHIFT       16

#define PSO_IDX_SHADOW        0
#define PSO_IDX_MAIN          1
#define PSO_IDX_TRANSPARENT   2

struct CommonResourcesConstants
{
    uint BindlessResourcesBaseIndex;
    uint FrameIndexMod2;
};

ConstantBuffer<CommonResourcesConstants> g_CommonResources;

uint GetBindlessResourceIndex(uint bindlessIndex)
{
    return g_CommonResources.BindlessResourcesBaseIndex + bindlessIndex;
}

DescriptorHandle<T> GetBindlessResourceHandle<T : IOpaqueDescriptor>(uint bindlessIndex)
{
    return DescriptorHandle<T>(uint2(GetBindlessResourceIndex(bindlessIndex), 0));
}

DescriptorHandle<T> GetResourceDescriptorHandle<T : IOpaqueDescriptor>(uint descriptorIndex)
{
    return DescriptorHandle<T>(uint2(descriptorIndex, 0));
}

SamplerState GetSamplerDescriptor(uint descriptorIndex)
{
    return DescriptorHandle<SamplerState>(uint2(descriptorIndex, 0));
}

// Mirrors C++ InstanceConstants in ConstantBuffers.h.
struct InstanceConstant
{
    float3 BBoxMin;
    float3 BBoxMax;
    uint MeshBufferIdx;
    uint JointBufferIdx;
};

// Mirrors C++ MeshConstants in ConstantBuffers.h.
struct MeshConstant
{
    float4x4 WorldMatrix;
    float4x3 WorldIT;
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
    float4x3 NrmMatrix;
};

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
    float2 coneAngles;

    float4x4 shadowTextureMatrix;
};

TextureCube<float3> GetIBLCubeMapSRV()
{
    return GetBindlessResourceHandle<TextureCube<float3>>(SRV_IBL_CUBE_MAP);
}

Texture2D<uint2> GetVBufferSRV()
{
    return GetBindlessResourceHandle<Texture2D<uint2>>(SRV_VBUFFER);
}

Texture2D<float4> GetGBufferASRV()
{
    return GetBindlessResourceHandle<Texture2D<float4>>(SRV_GBUFFER_A);
}

Texture2D<float4> GetGBufferBSRV()
{
    return GetBindlessResourceHandle<Texture2D<float4>>(SRV_GBUFFER_B);
}

Texture2D<float4> GetGBufferCSRV()
{
    return GetBindlessResourceHandle<Texture2D<float4>>(SRV_GBUFFER_C);
}

Texture2D<float4> GetGBufferDSRV()
{
    return GetBindlessResourceHandle<Texture2D<float4>>(SRV_GBUFFER_D);
}

Texture2D<float> GetSceneDepthSRV()
{
    return GetBindlessResourceHandle<Texture2D<float>>(SRV_SCENE_DEPTH);
}

Texture2D<float> GetPrevSceneHZBSRV()
{
    return GetBindlessResourceHandle<Texture2D<float>>(
        SRV_SCENE_HZB0 + 1 - g_CommonResources.FrameIndexMod2);
}

Texture2D<float> GetCurrentSceneHZBSRV()
{
    return GetBindlessResourceHandle<Texture2D<float>>(
        SRV_SCENE_HZB0 + g_CommonResources.FrameIndexMod2);
}

TextureCube<float3> GetIBLDiffuseLDSRV()
{
    return GetBindlessResourceHandle<TextureCube<float3>>(SRV_IBL_DIFFUSE_LD);
}

TextureCube<float3> GetIBLSpecularLDSRV()
{
    return GetBindlessResourceHandle<TextureCube<float3>>(SRV_IBL_SPECULAR_LD);
}

Texture2D<float4> GetIBLLutSRV()
{
    return GetBindlessResourceHandle<Texture2D<float4>>(SRV_IBL_LUT);
}

Texture2D<float> GetSSAOSRV()
{
    return GetBindlessResourceHandle<Texture2D<float>>(SRV_SSAO);
}

Texture2D<float> GetShadowMapSRV()
{
    return GetBindlessResourceHandle<Texture2D<float>>(SRV_SHADOW_MAP);
}

Texture2DArray<float> GetLightShadowArraySRV()
{
    return GetBindlessResourceHandle<Texture2DArray<float>>(SRV_LIGHT_SHADOW_ARRAY);
}

ByteAddressBuffer GetLightGridSRV()
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_LIGHT_GRID);
}

StructuredBuffer<LightData> GetLightBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<LightData>>(SRV_LIGHT_BUFFER);
}

StructuredBuffer<DrawItem> GetPotentialDrawItemBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<DrawItem>>(SRV_POTENTIAL_DRAW_ITEM_BUFFER);
}

StructuredBuffer<HierarchyNode> GetHierarchyNodesBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<HierarchyNode>>(SRV_HIERARCHY_NODES_BUFFER);
}

ByteAddressBuffer GetGeometryChunksBufferSRV(uint chunkOffset)
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_GEOMETRY_CHUNK_DATA_BUFFER + chunkOffset);
}

StructuredBuffer<GroupDataLocation> GetGroupDataLocationBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<GroupDataLocation>>(SRV_GROUP_DATA_LOCATION_BUFFER);
}

StructuredBuffer<VisibleMeshletPayload> GetVisibleMeshletBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<VisibleMeshletPayload>>(SRV_VISIBLE_MESHLET_BUFFER);
}

VisibleMeshletPayload GetVisibleMeshletPayload(uint index)
{
    return GetVisibleMeshletBufferSRV()[index];
}

StructuredBuffer<QueueState> GetTaskQueueStateBufferSRV()
{
    return GetBindlessResourceHandle<StructuredBuffer<QueueState>>(SRV_TASK_QUEUE_STATE_BUFFER);
}

ByteAddressBuffer GetInstanceConstantBufferSRV()
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_INSTANCE_CONSTANTS_BUFFER);
}

ByteAddressBuffer GetMeshConstantBufferSRV()
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_MESH_CONSTANTS_BUFFER);
}

ByteAddressBuffer GetMaterialConstantBufferSRV()
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_MATERIAL_CONSTANTS_BUFFER);
}

ByteAddressBuffer GetJointBufferSRV()
{
    return GetBindlessResourceHandle<ByteAddressBuffer>(SRV_JOINTS_BUFFER);
}

RWStructuredBuffer<QueueState> GetTaskQueueStateBufferUAV()
{
    return GetBindlessResourceHandle<RWStructuredBuffer<QueueState>>(UAV_TASK_QUEUE_STATE_BUFFER);
}

RWByteAddressBuffer GetTaskQueueBufferUAV()
{
    return GetBindlessResourceHandle<RWByteAddressBuffer>(UAV_TASK_QUEUE_BUFFER);
}

RWStructuredBuffer<VisibleMeshletPayload> GetVisibleMeshletBufferUAV()
{
    return GetBindlessResourceHandle<RWStructuredBuffer<VisibleMeshletPayload>>(UAV_VISIBLE_MESHLET_BUFFER);
}

RWStructuredBuffer<DispatchMeshCommand> GetDispatchMeshBufferUAV()
{
    return GetBindlessResourceHandle<RWStructuredBuffer<DispatchMeshCommand>>(UAV_INDIRECT_DISPATCH_MESHES_BASE);
}

RWByteAddressBuffer GetLightGridUAV()
{
    return GetBindlessResourceHandle<RWByteAddressBuffer>(UAV_LIGHT_GRID);
}

RWByteAddressBuffer GetLightGridBitMaskUAV()
{
    return GetBindlessResourceHandle<RWByteAddressBuffer>(UAV_LIGHT_GRID_MASK);
}

RWTexture2D<float4> GetSceneColorUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<float4>>(UAV_SCENE_COLOR);
}

RWTexture2D<float4> GetGBufferAUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<float4>>(UAV_GBUFFER_A);
}

RWTexture2D<float4> GetGBufferBUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<float4>>(UAV_GBUFFER_B);
}

RWTexture2D<float4> GetGBufferCUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<float4>>(UAV_GBUFFER_C);
}

RWTexture2D<float4> GetGBufferDUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<float4>>(UAV_GBUFFER_D);
}

RWTexture2D<uint64_t> GetVBufferUAV()
{
    return GetBindlessResourceHandle<RWTexture2D<uint64_t>>(UAV_VBUFFER);
}

RWByteAddressBuffer GetGeometryStreamingRequestMaskBufferUAV()
{
    return GetBindlessResourceHandle<RWByteAddressBuffer>(UAV_GEOMETRY_STREAMING_REQUEST_MASK_BUFFER);
}

InstanceConstant LoadInstanceConstant(ByteAddressBuffer instanceConstantBuffer, uint index)
{
    return instanceConstantBuffer.Load<InstanceConstant>(index * sizeof(InstanceConstant));
}

InstanceConstant LoadInstanceConstant(uint index)
{
    return LoadInstanceConstant(GetInstanceConstantBufferSRV(), index);
}

MeshConstant LoadMeshConstant(ByteAddressBuffer meshConstantBuffer, uint index)
{
    return meshConstantBuffer.Load<MeshConstant>(index * sizeof(MeshConstant));
}

MeshConstant LoadMeshConstant(uint index)
{
    return LoadMeshConstant(GetMeshConstantBufferSRV(), index);
}

MaterialConstant LoadMaterialConstant(ByteAddressBuffer materialConstantBuffer, uint index)
{
    return materialConstantBuffer.Load<MaterialConstant>(index * sizeof(MaterialConstant));
}

MaterialConstant LoadMaterialConstant(uint index)
{
    return LoadMaterialConstant(GetMaterialConstantBufferSRV(), index);
}

Joint LoadJoint(ByteAddressBuffer jointBuffer, uint index)
{
    return jointBuffer.Load<Joint>(index * sizeof(Joint));
}

Joint LoadJoint(uint index)
{
    return LoadJoint(GetJointBufferSRV(), index);
}

#endif

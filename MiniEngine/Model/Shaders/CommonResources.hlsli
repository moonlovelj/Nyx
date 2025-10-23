#ifndef __GPU_SCENE_HLSLI__
#define __GPU_SCENE_HLSLI__

cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;
    float4x4 InverseViewProjMatrix;
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
}

cbuffer CB2 : register(b2)
{
    uint ObjectIndex;
}

struct ObjectConstant
{
    float4 BoundingSphere;
    uint VertexBufferOffset;
    uint VertexStride;
    uint VertexBufferDepthOffset;
    uint VertexDepthStride;
    uint MeshConstantsIndex;
    uint MaterialConstantsIndex;
    uint MeshJointsIndexOffset;
};

StructuredBuffer<ObjectConstant> ObjectConstants : register(t20);

struct MeshConstant
{
    float4x4 WorldMatrix;
    float4x3 WorldIT; // Inverse-transpose of PosMatrix
};
StructuredBuffer<MeshConstant> MeshConstants : register(t21);

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
StructuredBuffer<MaterialConstant> MaterialConstants : register(t22);

ByteAddressBuffer VertexBuffer : register(t23);

#ifdef ENABLE_SKINNING
struct Joint
{
    float4x4 PosMatrix;
    float4x3 NrmMatrix; // Inverse-transpose of PosMatrix
};

StructuredBuffer<Joint> Joints : register(t24);
#endif


#endif

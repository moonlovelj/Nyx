
#include "LightGrid.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

#define FLT_MIN         1.175494351e-38F        // min positive value
#define FLT_MAX         3.402823466e+38F        // max value
#define PI				3.1415926535f
#define TWOPI			6.283185307f

cbuffer CSConstants : register(b0)
{
    float3 SunDirection;
    float3 SunColor;
    float3 AmbientColor;
    float4 ShadowTexelSize;
    float4x4 SunShadowMatrix;

    float4 InvTileDim;
    uint4 TileCount;
    uint4 FirstLightIndex;

    uint ViewportWidth;
    uint ViewportHeight;

    uint FrameIndexMod2;
};

Texture2D<float4> gBufferA : register(t0);
Texture2D<float4> gBufferB : register(t1);
Texture2D<float4> gBufferC : register(t2);
Texture2D<float4> gBufferD : register(t3);

Texture2D<float> texSSAO : register(t4);
Texture2D<float> texShadow : register(t5);

StructuredBuffer<LightData> lightBuffer : register(t6);
ByteAddressBuffer lightGrid : register(t7);
ByteAddressBuffer lightGridBitMask : register(t8);
Texture2DArray<float> lightShadowArrayTex : register(t9);

SamplerComparisonState shadowSampler : register(s0);
SamplerState cubeMapSampler : register(s1);

RWTexture2D<float4> sceneColor : register(u0);

#define _RootSig \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 10))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))," \
    "StaticSampler(s0," \
        "addressU = TEXTURE_ADDRESS_CLAMP," \
        "addressV = TEXTURE_ADDRESS_CLAMP," \
        "addressW = TEXTURE_ADDRESS_CLAMP," \
        "comparisonFunc = COMPARISON_GREATER_EQUAL," \
        "filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)," \
    "StaticSampler(s1, maxAnisotropy = 8)"

[RootSignature(_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint2 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 DTid = Gid * uint2(8, 8) + GTid;
    sceneColor[DTid] = float4(1, 0, 0, 0);
}

#ifndef __MATERIAL_COMMON_HLSLI__
#define __MATERIAL_COMMON_HLSLI__

#include "VSTOPSCommon.hlsli"

Texture2D<float4> baseColorTexture          : register(t0);
Texture2D<float3> metallicRoughnessTexture  : register(t1);
Texture2D<float1> occlusionTexture          : register(t2);
Texture2D<float3> emissiveTexture           : register(t3);
Texture2D<float3> normalTexture             : register(t4);

SamplerState baseColorSampler               : register(s0);
SamplerState metallicRoughnessSampler       : register(s1);
SamplerState occlusionSampler               : register(s2);
SamplerState emissiveSampler                : register(s3);
SamplerState normalSampler                  : register(s4);

cbuffer MaterialConstants                   : register(b0)
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
}

// Flag helpers
static const uint BASECOLOR_UV_OFFSET = 0;
static const uint METALLICROUGHNESS_UV_OFFSET = 1;
static const uint OCCLUSION_UV_OFFSET = 2;
static const uint EMISSIVE_UV_OFFSET = 3;
static const uint NORMAL_UV_OFFSET = 4;
#ifdef NO_SECOND_UV
#define UVSET( offset ) vsOutput.uv0
#else
#define UVSET( offset ) lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1)
#endif

float3 ComputeNormal(VSOutput vsOutput)
{
    float3 normal = normalize(vsOutput.normal);

#ifdef NO_TANGENT_FRAME
    return normal;
#else
    // Construct tangent frame
    float3 tangent = normalize(vsOutput.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * vsOutput.tangent.w;
    float3x3 tangentFrame = float3x3(tangent, bitangent, normal);

    // Read normal map and convert to SNORM (TODO:  convert all normal maps to R8G8B8A8_SNORM?)
    normal = normalTexture.Sample(normalSampler, UVSET(NORMAL_UV_OFFSET)) * 2.0 - 1.0;

    // glTF spec says to normalize N before and after scaling, but that's excessive
    normal = normalize(normal * float3(normalTextureScale, normalTextureScale, 1));

    // Multiply by transpose (reverse order)
    return mul(normal, tangentFrame);
#endif
}

struct MaterialProperties
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Occlusion;
    float3 Emissive;
    float3 Normal;
};

MaterialProperties GetMaterialProperties(VSOutput vsOutput)
{
    MaterialProperties MatProps;
    MatProps.BaseColor = baseColorFactor * baseColorTexture.Sample(baseColorSampler, UVSET(BASECOLOR_UV_OFFSET));
    float2 metallicRoughness = metallicRoughnessFactor *
        metallicRoughnessTexture.Sample(metallicRoughnessSampler, UVSET(METALLICROUGHNESS_UV_OFFSET)).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = occlusionTexture.Sample(occlusionSampler, UVSET(OCCLUSION_UV_OFFSET));
    MatProps.Emissive = emissiveFactor * emissiveTexture.Sample(emissiveSampler, UVSET(EMISSIVE_UV_OFFSET));
    MatProps.Normal = ComputeNormal(vsOutput);
    return MatProps;
}

#endif
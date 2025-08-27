//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):	James Stanard
//              Justin Saunders (ATG)

#include "Common.hlsli"
#include "LightingCommon.hlsli"

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

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD0;
#ifndef NO_SECOND_UV
    float2 uv1 : TEXCOORD1;
#endif
    float3 worldPos : TEXCOORD2;
    float3 sunShadowCoord : TEXCOORD3;
};

// Flag helpers
static const uint BASECOLOR = 0;
static const uint METALLICROUGHNESS = 1;
static const uint OCCLUSION = 2;
static const uint EMISSIVE = 3;
static const uint NORMAL = 4;
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
    normal = normalTexture.Sample(normalSampler, UVSET(NORMAL)) * 2.0 - 1.0;

    // glTF spec says to normalize N before and after scaling, but that's excessive
    normal = normalize(normal * float3(normalTextureScale, normalTextureScale, 1));

    // Multiply by transpose (reverse order)
    return mul(normal, tangentFrame);
#endif
}

[RootSignature(Renderer_RootSig)]
float4 main(VSOutput vsOutput) : SV_Target0
{
    // Load and modulate textures
    float4 baseColor = baseColorFactor * baseColorTexture.Sample(baseColorSampler, UVSET(BASECOLOR));
    float2 metallicRoughness = metallicRoughnessFactor * 
        metallicRoughnessTexture.Sample(metallicRoughnessSampler, UVSET(METALLICROUGHNESS)).bg;
    metallicRoughness.y = max(0.04, metallicRoughness.y);
    float occlusion = occlusionTexture.Sample(occlusionSampler, UVSET(OCCLUSION));
    float3 emissive = emissiveFactor * emissiveTexture.Sample(emissiveSampler, UVSET(EMISSIVE));
    float3 normal = ComputeNormal(vsOutput);

    SurfaceProperties Surface;
    Surface.N = normal;
    Surface.V = normalize(ViewerPos - vsOutput.worldPos);
    Surface.NdotV = saturate(dot(Surface.N, Surface.V));
    Surface.c_diff = baseColor.rgb * (1 - kDielectricSpecular) * (1 - metallicRoughness.x) * occlusion;
    Surface.c_spec = lerp(kDielectricSpecular, baseColor.rgb, metallicRoughness.x) * occlusion;
    Surface.roughness = metallicRoughness.y;
    Surface.alpha = metallicRoughness.y * metallicRoughness.y;
    Surface.alphaSqr = Surface.alpha * Surface.alpha;

    // Begin accumulating light starting with emissive
    float3 colorAccum = emissive;

#if 0
    float sunShadow = texSunShadow.SampleCmpLevelZero( shadowSampler, vsOutput.sunShadowCoord.xy, vsOutput.sunShadowCoord.z );
    colorAccum += ShadeDirectionalLight(Surface, SunDirection, sunShadow * SunIntensity);

    uint2 pixelPos = uint2(vsOutput.position.xy);
    float ssao = texSSAO[pixelPos];

    Surface.c_diff *= ssao;
    Surface.c_spec *= ssao;

    // Old-school ambient light
    colorAccum += Surface.c_diff * 0.1;

#else

    uint2 pixelPos = uint2(vsOutput.position.xy);
    float ssao = texSSAO[pixelPos];

    Surface.c_diff *= ssao;
    Surface.c_spec *= ssao;

    // Add IBL
    //colorAccum += Diffuse_IBL(Surface);
    //colorAccum += Specular_IBL(Surface);

    float2 ScreenUV = (float2(0.5, 0.5) + pixelPos) / float2(ViewportWidth, ViewportHeight);
    float4 shadowCoord = mul(SunShadowMatrix, float4(vsOutput.worldPos, 1.0));
    shadowCoord.xyz *= rcp(shadowCoord.w);
    float sunShadow = GetDirectionalShadow(ScreenUV, shadowCoord.xyz, texShadow);
    colorAccum.rgb += ShadeDirectionalLight(Surface, SunDirection, sunShadow * SunColor);

    ShadeLights(colorAccum.rgb, Surface, pixelPos, vsOutput.worldPos);

    // Add IBL
    //colorAccum.rgb += Diffuse_IBL(Surface) * ssao;
    colorAccum.rgb += EvaluateIBLDiffuse(Surface) * ssao;

    //colorAccum.rgb += Specular_IBL(Surface) * ssao;
    colorAccum.rgb += EvaluateIBLSpecular(Surface) * ssao;


#endif

    // TODO: Shade each light using Forward+ tiles

    return float4(colorAccum, baseColor.a);
}

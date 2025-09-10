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
#include "MaterialCommon.hlsli"


[RootSignature(Renderer_RootSig)]
float4 main(VSOutput vsOutput) : SV_Target0
{
    // Load and modulate textures
    MaterialProperties MatProps = GetMaterialProperties(vsOutput);

    SurfaceProperties Surface;
    Surface.N = MatProps.Normal;
    Surface.V = normalize(ViewerPos - vsOutput.worldPos);
    Surface.NdotV = saturate(dot(Surface.N, Surface.V));
    Surface.c_diff = MatProps.BaseColor.rgb * (1 - MatProps.Metallic) * MatProps.Occlusion;
    Surface.c_spec = lerp(kDielectricSpecular, MatProps.BaseColor.rgb, MatProps.Metallic.x) * MatProps.Occlusion;
    Surface.roughness = MatProps.Roughness;
    Surface.alpha = MatProps.Roughness * MatProps.Roughness;
    Surface.alphaSqr = Surface.alpha * Surface.alpha;

    // Begin accumulating light starting with emissive
    float3 colorAccum = MatProps.Emissive;

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

    return float4(colorAccum, MatProps.BaseColor.a);
}

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
// Author:  James Stanard 
//

#include "ShaderUtility.hlsli"
#include "PBRNeutralToneMapping.hlsli"
#include "PostEffectsRS.hlsli"
#include "PixelPacking.hlsli"

StructuredBuffer<float> Exposure : register(t0);
Texture2D<float3> Bloom : register(t1);
#if SUPPORT_TYPED_UAV_LOADS
RWTexture2D<float3> SrcColor : register(u0);
#else
RWTexture2D<uint> DstColor : register(u0);
Texture2D<float3> SrcColor : register(t2);
#endif
RWTexture2D<float> OutLuma : register(u1);
SamplerState LinearSampler : register( s0 );

cbuffer CB0 : register(b0)
{
    float2 g_RcpBufferDim;
    float g_BloomStrength;
    float ToneMapPeakBrightnessRatio;
};

[RootSignature(PostEffects_RootSig)]
[numthreads( 8, 8, 1 )]
void main( uint3 DTid : SV_DispatchThreadID )
{
    float2 TexCoord = (DTid.xy + 0.5) * g_RcpBufferDim;

    // Load HDR and bloom
    float3 hdrColor = SrcColor[DTid.xy] + g_BloomStrength * Bloom.SampleLevel(LinearSampler, TexCoord, 0);

    // Tone map to LDR and convert to greyscale
    float luma = RGBToLuminance(PBRNeutralToneMapping(hdrColor * Exposure[0], ToneMapPeakBrightnessRatio));

    float logLuma = LinearToLogLuminance(luma / ToneMapPeakBrightnessRatio);

#if SUPPORT_TYPED_UAV_LOADS
    SrcColor[DTid.xy] = luma.xxx;
#else
    DstColor[DTid.xy] = Pack_R11G11B10_FLOAT(luma.xxx);
#endif
    OutLuma[DTid.xy] = logLuma;
}

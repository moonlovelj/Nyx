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

#include "ToneMappingUtility.hlsli"
#include "PBRNeutralToneMapping.hlsli"
#include "PostEffectsRS.hlsli"
#include "PixelPacking.hlsli"


StructuredBuffer<float> Exposure : register( t0 );
Texture2D<float3> Bloom : register( t1 );
#if SUPPORT_TYPED_UAV_LOADS
RWTexture2D<float3> ColorRW : register( u0 );
#else
RWTexture2D<uint> DstColor : register( u0 );
Texture2D<float3> SrcColor : register( t2 );
#endif
RWTexture2D<float> OutLuma : register( u1 );
SamplerState LinearSampler : register( s0 );

cbuffer CB0 : register(b0)
{
    float2 g_RcpBufferDim;
    float g_BloomStrength;
    float ToneMapPeakBrightnessRatio; // HDR: peak nits / paper-white nits; SDR: 1
    float MaxBrightness;
};


[RootSignature(PostEffects_RootSig)]
[numthreads( 8, 8, 1 )]
void main( uint3 DTid : SV_DispatchThreadID )
{
    float2 TexCoord = (DTid.xy + 0.5) * g_RcpBufferDim;

    // Load HDR and bloom
#if SUPPORT_TYPED_UAV_LOADS
    float3 hdrColor = ColorRW[DTid.xy];
#else
    float3 hdrColor = SrcColor[DTid.xy];
#endif

    hdrColor += g_BloomStrength * Bloom.SampleLevel(LinearSampler, TexCoord, 0);
    hdrColor *= Exposure[0];

#if ENABLE_HDR_DISPLAY_MAPPING
    // Preserve highlight headroom above paper white. Display encoding happens at present.
    float3 mappedColor = PBRNeutralToneMapping(hdrColor, ToneMapPeakBrightnessRatio);
#else
    float3 mappedColor = PBRNeutralToneMapping(hdrColor);
#endif

#if SUPPORT_TYPED_UAV_LOADS
    ColorRW[DTid.xy] = mappedColor;
#else
    DstColor[DTid.xy] = Pack_R11G11B10_FLOAT(mappedColor);
#endif
    // The FXAA luminance buffer is UNORM; normalize HDR highlights to avoid clipping it.
    OutLuma[DTid.xy] = RGBToLogLuminance(mappedColor / ToneMapPeakBrightnessRatio);
}

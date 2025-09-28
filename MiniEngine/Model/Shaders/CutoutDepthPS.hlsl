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
//

#include "Common.hlsli"

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TexCoord0;
};

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
    uint TextureStartIndex;
    uint SamplerStartIndex;
}

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput)
{
    float cutoff = f16tof32(flags >> 16);
    
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[TextureStartIndex];
    SamplerState BaseColorSampler = SamplerDescriptorHeap[SamplerStartIndex];
    
    if (BaseColorTexture.Sample(BaseColorSampler, vsOutput.uv).a < cutoff)
        discard;
}

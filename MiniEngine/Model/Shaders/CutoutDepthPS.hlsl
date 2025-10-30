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
#include "CommonResources.hlsli"

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TexCoord0;
};

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput)
{
    MeshletConstant meshletConstant = MeshletConstants[MeshletIndex];
    MaterialConstant materilConstant = MaterialConstants[meshletConstant.MaterialConstantsIndex];
    
    float cutoff = f16tof32(materilConstant.flags >> 16);
    
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[materilConstant.TextureStartIndex];
    SamplerState BaseColorSampler = SamplerDescriptorHeap[materilConstant.SamplerStartIndex];
    
    if (BaseColorTexture.Sample(BaseColorSampler, vsOutput.uv).a < cutoff)
        discard;
}

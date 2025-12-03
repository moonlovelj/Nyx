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

#include "Common.hlsli"
#include "BindlessIndices.hlsli"

cbuffer PSConstants : register(b0)
{
    float TextureLevel;
    uint BindlessResourcesBaseIndex;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 viewDir : TEXCOORD3;
};

TextureCube<float3> GetIBLCubeMapSRV()
{
    return ResourceDescriptorHeap[BindlessResourcesBaseIndex + SRV_IBL_CUBE_MAP];
}

[RootSignature(Renderer_RootSig)]
float4 main(VSOutput vsOutput) : SV_Target0
{
    TextureCube<float3> radianceIBLTexture = GetIBLCubeMapSRV();
    return float4(radianceIBLTexture.SampleLevel(defaultSampler, vsOutput.viewDir, 0), 1);
}

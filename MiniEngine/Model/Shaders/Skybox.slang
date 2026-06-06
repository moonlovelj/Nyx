//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "CommonResources.slang"

struct SkyboxVSConstants
{
    float4x4 ProjInverse;
    float3x3 ViewInverse;
};

struct SkyboxVertexOutput
{
    float4 Position : SV_Position;
    float3 ViewDirection : TEXCOORD0;
};

ConstantBuffer<SkyboxVSConstants> g_SkyboxVS;
SamplerState g_SkyboxSampler;

[shader("vertex")]
SkyboxVertexOutput vertexMain(uint vertexID : SV_VertexID)
{
    float2 screenUV = float2(uint2(vertexID, vertexID << 1) & 2);
    float4 projectedPosition = float4(
        lerp(float2(-1.0, 1.0), float2(1.0, -1.0), screenUV),
        0.0,
        1.0);
    float4 viewPosition = mul(g_SkyboxVS.ProjInverse, projectedPosition);

    SkyboxVertexOutput output;
    output.Position = projectedPosition;
    output.ViewDirection = mul(
        g_SkyboxVS.ViewInverse,
        viewPosition.xyz / viewPosition.w);
    return output;
}

[shader("fragment")]
float4 pixelMain(SkyboxVertexOutput input) : SV_Target0
{
    TextureCube<float3> texture = GetIBLCubeMapSRV();
    return float4(
        texture.SampleLevel(
            g_SkyboxSampler,
            input.ViewDirection,
            0.0),
        1.0);
}

//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

#include "CommonResources.slang"

struct VertexOutput
{
    float4 Position : SV_Position;
};

struct PixelOutput
{
    float Depth : SV_Depth;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexID : SV_VertexID)
{
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);

    VertexOutput output;
    output.Position = float4(
        uv * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}

[shader("fragment")]
PixelOutput pixelMain(VertexOutput input)
{
    Texture2D<uint2> vbuffer = GetVBufferSRV();
    uint2 vData = vbuffer.Load(int3(int2(input.Position.xy), 0));

    PixelOutput output;
    output.Depth = asfloat(vData.y);
    return output;
}

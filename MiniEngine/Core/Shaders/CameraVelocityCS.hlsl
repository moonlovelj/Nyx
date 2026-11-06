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

#include "CommonRS.hlsli"
#include "PixelPacking_Velocity.hlsli"

Texture2D<float> DepthBuffer : register(t0);
Texture2D<float> SceneDepth : register(t1);
RWTexture2D<packed_velocity_t> VelocityBuffer : register(u0);
RWTexture2D<float2> ReconstructionVelocity : register(u1);

cbuffer CBuffer : register(b1)
{
    matrix CurToPrevXForm;
    matrix SkyCurToPrevXForm;
    float2 JitterDelta;
    uint UseLinearDepth;
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 st = DTid.xy;
    uint width, height;
    DepthBuffer.GetDimensions(width, height);
    if (any(st >= uint2(width, height)))
        return;

    const float2 currentPixel = st + 0.5;
    const float depth = DepthBuffer[st];
    const bool sky = SceneDepth[st] == 0.0; // Reverse-Z clear depth.
    const float4 position = UseLinearDepth
        ? float4(currentPixel * depth, 1.0, depth)
        : float4(currentPixel, depth, 1.0);
    const float4 previous = sky
        ? mul(SkyCurToPrevXForm, float4(currentPixel, 0.0, 1.0))
        : mul(CurToPrevXForm, position);

    ReconstructionVelocity[st] = 0.0;
    VelocityBuffer[st] = INVALID_VELOCITY;
    if (!all(isfinite(previous)) || previous.w <= 1e-20)
        return;

    float3 velocity;
    velocity.xy = previous.xy / previous.w - currentPixel - JitterDelta;
    velocity.z = sky ? 0.0 : (UseLinearDepth ? previous.w : previous.z / previous.w) - depth;
    if (!all(isfinite(velocity)))
        return;

    // Keep valid offscreen motion, bounded only by the finite RG16F range.
    ReconstructionVelocity[st] = clamp(velocity.xy, -65504.0, 65504.0);
    VelocityBuffer[st] = PackVelocity(velocity);
}

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

// TAA filtering and longest velocity selection adapted from Intel TAA.
// Copyright (c) 2019-2020, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "CommonRS.hlsli"
#include "ShaderUtility.hlsli"
#include "PixelPacking_Velocity.hlsli"

static const uint kLdsPitch = 18;
static const uint kLdsRows = 10;

RWTexture2D<float4> OutTemporal : register(u0);

Texture2D<packed_velocity_t> VelocityBuffer : register(t0);
Texture2D<float3> InColor : register(t1);
Texture2D<float4> InTemporal : register(t2);
Texture2D<float> CurDepth : register(t3);
Texture2D<float> PreDepth : register(t4);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

groupshared float ldsDepth[kLdsPitch * kLdsRows];
groupshared float ldsR[kLdsPitch * kLdsRows];
groupshared float ldsG[kLdsPitch * kLdsRows];
groupshared float ldsB[kLdsPitch * kLdsRows];


cbuffer CB1 : register(b1)
{
    float2 RcpBufferDim;	// 1 / width, 1 / height
    float RcpSpeedLimiter;
    uint HistoryValid;
    float2 JitterDelta;
}

void StoreRGB(uint ldsIdx, float3 RGB)
{
    ldsR[ldsIdx] = RGB.r;
    ldsG[ldsIdx] = RGB.g;
    ldsB[ldsIdx] = RGB.b;
}

float3 LoadRGB(uint ldsIdx)
{
    return float3(ldsR[ldsIdx], ldsG[ldsIdx], ldsB[ldsIdx]);
}

void WriteNoHistory(uint2 ST, uint ldsIdx)
{
    float3 color = LoadRGB(ldsIdx);
    // Intel's five-sample 'X' filter; the prefetched one-pixel border covers every tap.
    color += LoadRGB(ldsIdx - kLdsPitch - 1);
    color += LoadRGB(ldsIdx - kLdsPitch + 1);
    color += LoadRGB(ldsIdx + kLdsPitch - 1);
    color += LoadRGB(ldsIdx + kLdsPitch + 1);
    color *= 0.2;
    // History stores premultiplied color; restart accumulation at confidence 0.5.
    OutTemporal[ST] = float4(color, 1.0) * 0.5;
}

float2 STtoUV(float2 ST)
{
    return (ST + 0.5) * RcpBufferDim;
}

float4 SampleHistory(float2 historyST)
{
    // A full cubic footprint spans floor(ST)-1 through floor(ST)+2.
    // Use bilinear near the image boundary to avoid extending edge texels into the cubic filter.
    if (any(historyST < 1.0) || any(historyST >= rcp(RcpBufferDim) - 2.0))
        return InTemporal.SampleLevel(LinearSampler, STtoUV(historyST), 0);

    const float2 t = frac(historyST);
    const float2 t2 = t * t;
    const float2 t3 = t2 * t;
    const float2 w0 = -0.5 * t3 + t2 - 0.5 * t;
    const float2 w1 = 1.5 * t3 - 2.5 * t2 + 1.0;
    const float2 w2 = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    const float2 w3 = 0.5 * t3 - 0.5 * t2;
    const float2 s0 = w1 + w2;
    const float2 uv = STtoUV(floor(historyST));
    const float2 m0 = uv + (w2 / s0) * RcpBufferDim;
    const float2 tc0 = uv - RcpBufferDim;
    const float2 tc3 = uv + 2.0 * RcpBufferDim;

    // Filter premultiplied RGB and confidence together, using Intel's 5-fetch Catmull-Rom approximation.
    const float4 A = InTemporal.SampleLevel(LinearSampler, float2(m0.x, tc0.y), 0);
    const float4 B = InTemporal.SampleLevel(LinearSampler, float2(tc0.x, m0.y), 0);
    const float4 C = InTemporal.SampleLevel(LinearSampler, m0, 0);
    const float4 D = InTemporal.SampleLevel(LinearSampler, float2(tc3.x, m0.y), 0);
    const float4 E = InTemporal.SampleLevel(LinearSampler, float2(m0.x, tc3.y), 0);
    const float4 history =
        (0.5 * (A + B) * w0.x + A * s0.x + 0.5 * (A + B) * w3.x) * w0.y +
        (B * w0.x + C * s0.x + D * w3.x) * s0.y +
        (0.5 * (B + E) * w0.x + E * s0.x + 0.5 * (D + E) * w3.x) * w3.y;

    // Negative lobes can make confidence unusable; bilinear provides a safe correspondence in that case.
    if (history.a <= 1e-6 || !all(isfinite(history)))
        return InTemporal.SampleLevel(LinearSampler, STtoUV(historyST), 0);
    return history;
}

float3 ClipColor(float3 Color, float3 BoxMin, float3 BoxMax)
{
    float3 BoxCenter = (BoxMax + BoxMin) * 0.5;
    float3 HalfDim = (BoxMax - BoxMin) * 0.5 + 0.001;
    float3 Displacement = Color - BoxCenter;
    float3 Units = abs(Displacement / HalfDim);
    float MaxUnit = max(max(Units.x, Units.y), max(Units.z, 1.0));
    return BoxCenter + Displacement / MaxUnit;
}

float3 RGBToYCoCg(float3 rgb)
{
    return float3(
        dot(rgb, float3(0.25, 0.5, 0.25)),
        dot(rgb, float3(0.5, 0.0, -0.5)),
        dot(rgb, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRGB(float3 color)
{
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

float3 ClipHistoryVariance(float3 historyRGB, uint ldsIdx, float speedFactor)
{
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sampleRGB = TM(LoadRGB((int)ldsIdx + y * (int)kLdsPitch + x));
            const float3 color = RGBToYCoCg(sampleRGB);
            moment1 += color;
            moment2 += color * color;
        }
    }

    const float3 mean = moment1 / 9.0;
    const float3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    const float gamma = lerp(0.75, 2.0, speedFactor * speedFactor);
    // History and neighborhood statistics both use tone-mapped YCoCg.
    const float3 clipped = ClipColor(RGBToYCoCg(historyRGB), mean - gamma * sigma, mean + gamma * sigma);
    return max(YCoCgToRGB(clipped), 0.0);
}

float MaxOf(float4 Depths) { return max(max(Depths.x, Depths.y), max(Depths.z, Depths.w)); }

uint SelectLongestVelocity(uint2 ST, uint ldsIdx, out float SelectedDepth)
{
    uint selected = VelocityBuffer[ST];
    SelectedDepth = ldsDepth[ldsIdx];
    const float2 centerVelocity = UnpackVelocity(selected).xy;
    float longestLengthSq = dot(centerVelocity, centerVelocity);
    uint width, height;
    VelocityBuffer.GetDimensions(width, height);
    const int2 offsets[8] = {
        int2(-1, -1), int2(-1, 0), int2(-1, 1), int2(0, 1),
        int2(1, 1), int2(1, 0), int2(1, -1), int2(0, -1)
    };
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int2 sampleST = int2(ST) + offsets[i];
        if (any(sampleST < 0) || any(sampleST >= int2(width, height)))
            continue;
        const uint candidate = VelocityBuffer[sampleST];
        if (candidate == INVALID_VELOCITY)
            continue;
        const float2 velocity = UnpackVelocity(candidate).xy;
        const float lengthSq = dot(velocity, velocity);
        // Strict comparison keeps the center when velocities tie.
        if (lengthSq > longestLengthSq)
        {
            selected = candidate;
            longestLengthSq = lengthSq;
            // Keep current depth and the selected velocity's Z delta on the same surface.
            SelectedDepth = ldsDepth[(int)ldsIdx + offsets[i].y * (int)kLdsPitch + offsets[i].x];
        }
    }
    return selected;
}

void ApplyTemporalBlend(uint2 ST, uint ldsIdx)
{
    float3 CurrentColor = LoadRGB(ldsIdx);

    if (!HistoryValid || VelocityBuffer[ST] == INVALID_VELOCITY)
    {
        WriteNoHistory(ST, ldsIdx);
        return;
    }

    float CompareDepth;

    // The center is valid and invalid neighbors are skipped by the selector.
    uint PackedVelocity = SelectLongestVelocity(ST, ldsIdx, CompareDepth);
    float3 Velocity = UnpackVelocity(PackedVelocity);

    const float2 HistoryUV = STtoUV(ST + Velocity.xy);
    const float2 HistoryDepthUV = STtoUV(ST + Velocity.xy + JitterDelta);
    // History color and raw depth use different jitter conventions; both must be in bounds.
    if (!(all(HistoryUV >= 0.0) && all(HistoryUV < 1.0) &&
          all(HistoryDepthUV >= 0.0) && all(HistoryDepthUV < 1.0)))
    {
        WriteNoHistory(ST, ldsIdx);
        return;
    }

    CompareDepth += Velocity.z;

    // The temporal depth is the actual depth of the pixel found at the same reprojected location.
    float TemporalDepth = MaxOf(PreDepth.Gather(LinearSampler, HistoryDepthUV)) + 1e-3;

    // Fast-moving pixels cause motion blur and probably don't need TAA
    float SpeedFactor = saturate(1.0 - length(Velocity.xy) * RcpSpeedLimiter);
    if (!(SpeedFactor > 0.0 && CompareDepth <= TemporalDepth))
    {
        WriteNoHistory(ST, ldsIdx);
        return;
    }

    // Fetch temporal color.  Its "confidence" weight is stored in alpha.
    float4 Temp = SampleHistory(ST + Velocity.xy);
    if (!(Temp.a > 1e-6) || !all(isfinite(Temp)))
    {
        WriteNoHistory(ST, ldsIdx);
        return;
    }
    // Recover color using the original filtered alpha, then bound confidence separately to handle cubic overshoot.
    const float3 HistoryColor = max(Temp.rgb / max(Temp.a, 1e-6), 0.0);
    float TemporalWeight = saturate(Temp.a);
    const float3 ClippedHistory = ClipHistoryVariance(TM(HistoryColor), ldsIdx, SpeedFactor);

    // Depth rejection is handled above; retain the continuous motion confidence for valid history.
    TemporalWeight *= SpeedFactor;

    // Blend previous color with new color based on confidence.  Confidence steadily grows with each iteration
    // until it is broken by movement such as through disocclusion, color changes, or moving beyond the resolution
    // of the velocity buffer.
    const float3 BlendedColor = lerp(TM(CurrentColor), ClippedHistory, TemporalWeight);
    // Clipping and RGB nonnegative clamping can push luminance to the inverse's singularity.
    // Protect the inverse and keep linear HDR representable in the FP16 history texture.
    const float denominator = 1.0 - RGBToLuminance(BlendedColor);
    if (!(denominator > 0.0) || !all(isfinite(BlendedColor)))
    {
        WriteNoHistory(ST, ldsIdx);
        return;
    }
    const float3 TemporalColor = min(BlendedColor / max(denominator, 1e-6), 65504.0);

    // Update weight
    TemporalWeight = saturate(rcp(2.0 - TemporalWeight));

    // Quantize weight to what is representable
    TemporalWeight = f16tof32(f32tof16(TemporalWeight));

    // Breaking this up into two buffers means it can be 40 bits instead of 64.
    OutTemporal[ST] = float4(TemporalColor, 1) * TemporalWeight;
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    const uint ldsHalfPitch = kLdsPitch / 2;

    // Prefetch an 16x8 tile of pixels (8x8 colors) including a 1 pixel border
    // 10x18 IDs with 4 IDs per thread = 45 threads
    for (uint i = GI; i < 45; i += 64)
    {
        uint X = (i % ldsHalfPitch) * 2;
        uint Y = (i / ldsHalfPitch) * 2;
        uint TopLeftIdx = X + Y * kLdsPitch;
        int2 TopLeftST = Gid.xy * uint2(8, 8) - 1 + uint2(X / 2, Y);
        float2 UV = RcpBufferDim * (TopLeftST * float2(2, 1) + float2(2, 1));

        float4 Depths = CurDepth.Gather(LinearSampler, UV);
        ldsDepth[TopLeftIdx + 0] = Depths.w;
        ldsDepth[TopLeftIdx + 1] = Depths.z;
        ldsDepth[TopLeftIdx + kLdsPitch] = Depths.x;
        ldsDepth[TopLeftIdx + 1 + kLdsPitch] = Depths.y;

        float4 R4 = InColor.GatherRed(LinearSampler, UV);
        float4 G4 = InColor.GatherGreen(LinearSampler, UV);
        float4 B4 = InColor.GatherBlue(LinearSampler, UV);
        StoreRGB(TopLeftIdx, float3(R4.w, G4.w, B4.w));
        StoreRGB(TopLeftIdx + 1, float3(R4.z, G4.z, B4.z));
        StoreRGB(TopLeftIdx + kLdsPitch, float3(R4.x, G4.x, B4.x));
        StoreRGB(TopLeftIdx + 1 + kLdsPitch, float3(R4.y, G4.y, B4.y));
    }

    GroupMemoryBarrierWithGroupSync();

    uint Idx0 = GTid.x * 2 + GTid.y * kLdsPitch + kLdsPitch + 1;
    uint Idx1 = Idx0 + 1;

    uint2 ST0 = DTid.xy * uint2(2, 1);
    ApplyTemporalBlend(ST0, Idx0);

    uint2 ST1 = ST0 + uint2(1, 0);
    ApplyTemporalBlend(ST1, Idx1);
}

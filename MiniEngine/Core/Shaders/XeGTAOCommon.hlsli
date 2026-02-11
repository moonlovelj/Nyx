#ifndef XE_GTAO_COMMON_HLSLI
#define XE_GTAO_COMMON_HLSLI

#define XE_GTAO_DEPTH_MIP_LEVELS 5
#define XE_GTAO_NUMTHREADS_X 8
#define XE_GTAO_NUMTHREADS_Y 8
#define XE_GTAO_PI 3.14159265358979323846
#define XE_GTAO_PI_HALF 1.57079632679489661923
#define XE_GTAO_OCCLUSION_TERM_SCALE 1.5

struct XeGTAOConstants
{
    uint2 ViewportSize;
    float2 InvViewportSize;

    float2 DepthUnpackConsts;
    float2 CameraTanHalfFov;

    float2 NdcToViewMul;
    float2 NdcToViewAdd;

    float2 NdcToViewMulTimesPixelSize;
    float EffectRadius;
    float EffectFalloffRange;
    float RadiusMultiplier;

    float FinalValuePower;
    float DenoiseBlurBeta;
    float SampleDistributionPower;
    float ThinOccluderCompensation;
    float DepthMipSamplingOffset;

    uint NoiseIndex;
    float Padding0;
};

float XeGTAO_LinearizeDepth(float depthNdc, XeGTAOConstants constants)
{
    return constants.DepthUnpackConsts.x / (constants.DepthUnpackConsts.y - depthNdc);
}

float3 XeGTAO_ComputeViewspacePosition(float2 uv, float viewDepth, XeGTAOConstants constants)
{
    float3 outPos;
    outPos.xy = (constants.NdcToViewMul * uv + constants.NdcToViewAdd) * viewDepth;
    outPos.z = viewDepth;
    return outPos;
}

float4 XeGTAO_CalculateEdges(float centerZ, float leftZ, float rightZ, float topZ, float bottomZ)
{
    float4 edges = float4(leftZ, rightZ, topZ, bottomZ) - centerZ;

    float slopeLR = (edges.y - edges.x) * 0.5;
    float slopeTB = (edges.w - edges.z) * 0.5;
    float4 slopeAdjusted = edges + float4(slopeLR, -slopeLR, slopeTB, -slopeTB);
    edges = min(abs(edges), abs(slopeAdjusted));

    return saturate(1.25 - edges / (centerZ * 0.011));
}

float XeGTAO_PackEdges(float4 edgesLRTB)
{
    edgesLRTB = round(saturate(edgesLRTB) * 2.9);
    return dot(edgesLRTB, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

float4 XeGTAO_UnpackEdges(float packedVal)
{
    uint packed = (uint)(packedVal * 255.5);
    float4 edgesLRTB;
    edgesLRTB.x = ((packed >> 6) & 0x03) / 3.0;
    edgesLRTB.y = ((packed >> 4) & 0x03) / 3.0;
    edgesLRTB.z = ((packed >> 2) & 0x03) / 3.0;
    edgesLRTB.w = ((packed >> 0) & 0x03) / 3.0;
    return saturate(edgesLRTB);
}

float XeGTAO_FastSqrt(float x)
{
    return asfloat(0x1fbd1df5 + (asint(x) >> 1));
}

float XeGTAO_FastAcos(float inX)
{
    const float pi = 3.141593;
    const float halfPi = 1.570796;
    float x = abs(inX);
    float outAngle = -0.156583 * x + halfPi;
    outAngle *= XeGTAO_FastSqrt(1.0 - x);
    return (inX >= 0.0) ? outAngle : (pi - outAngle);
}

float XeGTAO_DepthMipFilter(float d0, float d1, float d2, float d3, XeGTAOConstants constants)
{
    float maxDepth = max(max(d0, d1), max(d2, d3));
    float effectRadius = 0.75 * constants.EffectRadius * constants.RadiusMultiplier;
    float falloffRange = constants.EffectFalloffRange * effectRadius;
    float falloffFrom = effectRadius * (1.0 - constants.EffectFalloffRange);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float w0 = saturate((maxDepth - d0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - d1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - d2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - d3) * falloffMul + falloffAdd);
    float weightSum = w0 + w1 + w2 + w3;
    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / weightSum;
}

float3 XeGTAO_ReconstructNormalFromDepth(uint2 pixCoord, float centerZ, float leftZ, float rightZ, float topZ, float bottomZ, float4 edgesLRTB, XeGTAOConstants constants)
{
    float2 uv = (float2(pixCoord) + 0.5) * constants.InvViewportSize;
    float3 center = XeGTAO_ComputeViewspacePosition(uv, centerZ, constants);
    float3 leftP = XeGTAO_ComputeViewspacePosition(uv + float2(-1, 0) * constants.InvViewportSize, leftZ, constants);
    float3 rightP = XeGTAO_ComputeViewspacePosition(uv + float2(1, 0) * constants.InvViewportSize, rightZ, constants);
    float3 topP = XeGTAO_ComputeViewspacePosition(uv + float2(0, -1) * constants.InvViewportSize, topZ, constants);
    float3 bottomP = XeGTAO_ComputeViewspacePosition(uv + float2(0, 1) * constants.InvViewportSize, bottomZ, constants);

    // Edge-weighted normal reconstruction reduces artifacts at depth discontinuities.
    float4 accepted = saturate(float4(
        edgesLRTB.x * edgesLRTB.z,
        edgesLRTB.z * edgesLRTB.y,
        edgesLRTB.y * edgesLRTB.w,
        edgesLRTB.w * edgesLRTB.x) + 0.01);

    float3 leftDir = normalize(leftP - center);
    float3 rightDir = normalize(rightP - center);
    float3 topDir = normalize(topP - center);
    float3 bottomDir = normalize(bottomP - center);

    float3 n = accepted.x * cross(leftDir, topDir)
             + accepted.y * cross(topDir, rightDir)
             + accepted.z * cross(rightDir, bottomDir)
             + accepted.w * cross(bottomDir, leftDir);
    return normalize(n);
}

void XeGTAO_OutputWorkingAO(uint2 pixCoord, float visibility, RWTexture2D<uint> outputAO)
{
    float encoded = saturate(visibility / XE_GTAO_OCCLUSION_TERM_SCALE);
    outputAO[pixCoord] = uint(encoded * 255.0 + 0.5);
}

float XeGTAO_DecodeWorkingAO(uint packedAO)
{
    return packedAO / 255.0;
}

void XeGTAO_MainPass(
    uint2 pixCoord,
    int sliceCount,
    int stepsPerSlice,
    float2 localNoise,
    XeGTAOConstants constants,
    Texture2D<float> sourceViewDepth,
    SamplerState pointSampler,
    RWTexture2D<uint> outWorkingAO,
    RWTexture2D<unorm float> outWorkingEdges)
{
    if (pixCoord.x >= constants.ViewportSize.x || pixCoord.y >= constants.ViewportSize.y)
    {
        return;
    }

    float2 uv = (float2(pixCoord) + 0.5) * constants.InvViewportSize;

    float4 valuesUL = sourceViewDepth.GatherRed(pointSampler, float2(pixCoord) * constants.InvViewportSize);
    float4 valuesBR = sourceViewDepth.GatherRed(pointSampler, float2(pixCoord) * constants.InvViewportSize, int2(1, 1));

    float viewZ = valuesUL.y;
    float leftZ = valuesUL.x;
    float topZ = valuesUL.z;
    float rightZ = valuesBR.z;
    float bottomZ = valuesBR.x;

    float4 edgesLRTB = XeGTAO_CalculateEdges(viewZ, leftZ, rightZ, topZ, bottomZ);
    outWorkingEdges[pixCoord] = XeGTAO_PackEdges(edgesLRTB);

    // Reconstruct normal from unbiased center depth. The depth bias below is only
    // used for horizon sampling stability and should not affect normal orientation.
    float3 normal = XeGTAO_ReconstructNormalFromDepth(pixCoord, viewZ, leftZ, rightZ, topZ, bottomZ, edgesLRTB, constants);

    viewZ *= 0.9992;

    float3 centerPos = XeGTAO_ComputeViewspacePosition(uv, viewZ, constants);
    float3 viewDir = normalize(-centerPos);

    float effectRadius = constants.EffectRadius * constants.RadiusMultiplier;
    float falloffRange = constants.EffectFalloffRange * effectRadius;
    float falloffFrom = effectRadius * (1.0 - constants.EffectFalloffRange);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float screenspaceRadius = effectRadius / (viewZ * constants.NdcToViewMulTimesPixelSize.x);
    float minS = 1.3 / screenspaceRadius;

    float visibility = saturate((10.0 - screenspaceRadius) / 100.0) * 0.5;

    [loop]
    for (int slice = 0; slice < sliceCount; ++slice)
    {
        float sliceK = (slice + localNoise.x) / sliceCount;
        float phi = sliceK * XE_GTAO_PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);

        float2 omega = float2(cosPhi, -sinPhi) * screenspaceRadius;
        float3 directionVec = float3(cosPhi, sinPhi, 0.0);
        float3 orthoDirection = directionVec - dot(directionVec, viewDir) * viewDir;
        float3 axis = normalize(cross(orthoDirection, viewDir));
        float3 projectedNormal = normal - axis * dot(normal, axis);

        float signNorm = sign(dot(orthoDirection, projectedNormal));
        float projectedNormalLen = length(projectedNormal);
        float cosNorm = saturate(dot(projectedNormal, viewDir) / projectedNormalLen);
        float n = signNorm * XeGTAO_FastAcos(cosNorm);

        float lowHorizonCos0 = cos(n + XE_GTAO_PI_HALF);
        float lowHorizonCos1 = cos(n - XE_GTAO_PI_HALF);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        [loop]
        for (int stepIdx = 0; stepIdx < stepsPerSlice; ++stepIdx)
        {
            float stepBaseNoise = (slice + stepIdx * stepsPerSlice) * 0.6180339887498948482;
            float stepNoise = frac(localNoise.y + stepBaseNoise);
            float s = (stepIdx + stepNoise) / stepsPerSlice;
            s = pow(s, constants.SampleDistributionPower);
            s += minS;

            float2 sampleOffset = s * omega;
            float sampleLen = length(sampleOffset);
            float mipLevel = clamp(log2(sampleLen) - constants.DepthMipSamplingOffset, 0.0, (float)XE_GTAO_DEPTH_MIP_LEVELS);

            sampleOffset = round(sampleOffset) * constants.InvViewportSize;

            float2 sampleUv0 = uv + sampleOffset;
            float2 sampleUv1 = uv - sampleOffset;

            // Screen-space AO has no information outside the viewport.
            // Mask those samples out to avoid edge-clamp occlusion artifacts.
            float2 sampleUv0Clamped = saturate(sampleUv0);
            float2 sampleUv1Clamped = saturate(sampleUv1);
            float2 inMin0 = step(0.0.xx, sampleUv0);
            float2 inMax0 = step(sampleUv0, 1.0.xx);
            float2 inMin1 = step(0.0.xx, sampleUv1);
            float2 inMax1 = step(sampleUv1, 1.0.xx);
            float inBounds0 = inMin0.x * inMin0.y * inMax0.x * inMax0.y;
            float inBounds1 = inMin1.x * inMin1.y * inMax1.x * inMax1.y;

            float sampleZ0 = sourceViewDepth.SampleLevel(pointSampler, sampleUv0Clamped, mipLevel).x;
            float sampleZ1 = sourceViewDepth.SampleLevel(pointSampler, sampleUv1Clamped, mipLevel).x;
            float3 samplePos0 = XeGTAO_ComputeViewspacePosition(sampleUv0, sampleZ0, constants);
            float3 samplePos1 = XeGTAO_ComputeViewspacePosition(sampleUv1, sampleZ1, constants);

            float3 delta0 = samplePos0 - centerPos;
            float3 delta1 = samplePos1 - centerPos;
            float sampleDist0 = length(delta0);
            float sampleDist1 = length(delta1);

            float3 horizonVec0 = delta0 / max(sampleDist0, 1e-6);
            float3 horizonVec1 = delta1 / max(sampleDist1, 1e-6);

            float falloffBase0 = length(float3(delta0.x, delta0.y, delta0.z * (1.0 + constants.ThinOccluderCompensation)));
            float falloffBase1 = length(float3(delta1.x, delta1.y, delta1.z * (1.0 + constants.ThinOccluderCompensation)));
            float weight0 = saturate(falloffBase0 * falloffMul + falloffAdd) * inBounds0;
            float weight1 = saturate(falloffBase1 * falloffMul + falloffAdd) * inBounds1;

            float shc0 = dot(horizonVec0, viewDir);
            float shc1 = dot(horizonVec1, viewDir);
            shc0 = lerp(lowHorizonCos0, shc0, weight0);
            shc1 = lerp(lowHorizonCos1, shc1, weight1);

            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }

        projectedNormalLen = lerp(projectedNormalLen, 1.0, 0.05);

        float h0 = -XeGTAO_FastAcos(horizonCos1);
        float h1 = XeGTAO_FastAcos(horizonCos0);
        float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += projectedNormalLen * (iarc0 + iarc1);
    }

    visibility /= sliceCount;
    visibility = pow(visibility, constants.FinalValuePower);
    visibility = max(0.03, visibility);
    XeGTAO_OutputWorkingAO(pixCoord, visibility, outWorkingAO);
}

groupshared float g_XeGTAOScratchDepths[8][8];
void XeGTAO_PrefilterDepths16x16(
    uint2 dispatchThreadID,
    uint2 groupThreadID,
    XeGTAOConstants constants,
    Texture2D<float> sourceDepthNdc,
    SamplerState pointSampler,
    RWTexture2D<float> outDepth0,
    RWTexture2D<float> outDepth1,
    RWTexture2D<float> outDepth2,
    RWTexture2D<float> outDepth3,
    RWTexture2D<float> outDepth4)
{
    uint2 mip0Size = constants.ViewportSize;
    uint2 mip1Size = (mip0Size + 1) / 2;
    uint2 mip2Size = (mip1Size + 1) / 2;
    uint2 mip3Size = (mip2Size + 1) / 2;
    uint2 mip4Size = (mip3Size + 1) / 2;

    uint2 baseCoord = dispatchThreadID;
    uint2 pixCoord = baseCoord * 2;

    float2 uv = float2(pixCoord) * constants.InvViewportSize;
    float4 depths4 = sourceDepthNdc.GatherRed(pointSampler, uv, int2(1, 1));

    float depth0 = clamp(XeGTAO_LinearizeDepth(depths4.w, constants), 0.0, 65504.0);
    float depth1 = clamp(XeGTAO_LinearizeDepth(depths4.z, constants), 0.0, 65504.0);
    float depth2 = clamp(XeGTAO_LinearizeDepth(depths4.x, constants), 0.0, 65504.0);
    float depth3 = clamp(XeGTAO_LinearizeDepth(depths4.y, constants), 0.0, 65504.0);

    if (pixCoord.x + 0 < mip0Size.x && pixCoord.y + 0 < mip0Size.y) { outDepth0[pixCoord + uint2(0, 0)] = depth0; }
    if (pixCoord.x + 1 < mip0Size.x && pixCoord.y + 0 < mip0Size.y) { outDepth0[pixCoord + uint2(1, 0)] = depth1; }
    if (pixCoord.x + 0 < mip0Size.x && pixCoord.y + 1 < mip0Size.y) { outDepth0[pixCoord + uint2(0, 1)] = depth2; }
    if (pixCoord.x + 1 < mip0Size.x && pixCoord.y + 1 < mip0Size.y) { outDepth0[pixCoord + uint2(1, 1)] = depth3; }

    float mip1Depth = XeGTAO_DepthMipFilter(depth0, depth1, depth2, depth3, constants);
    if (baseCoord.x < mip1Size.x && baseCoord.y < mip1Size.y)
    {
        outDepth1[baseCoord] = mip1Depth;
    }
    g_XeGTAOScratchDepths[groupThreadID.x][groupThreadID.y] = mip1Depth;
    GroupMemoryBarrierWithGroupSync();

    if (all((groupThreadID.xy % 2.xx) == 0))
    {
        float inTL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_XeGTAOScratchDepths[groupThreadID.x + 1][groupThreadID.y + 0];
        float inBL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 1];
        float inBR = g_XeGTAOScratchDepths[groupThreadID.x + 1][groupThreadID.y + 1];
        float mip2Depth = XeGTAO_DepthMipFilter(inTL, inTR, inBL, inBR, constants);
        uint2 outCoord = baseCoord / 2;
        if (outCoord.x < mip2Size.x && outCoord.y < mip2Size.y)
        {
            outDepth2[outCoord] = mip2Depth;
        }
        g_XeGTAOScratchDepths[groupThreadID.x][groupThreadID.y] = mip2Depth;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all((groupThreadID.xy % 4.xx) == 0))
    {
        float inTL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_XeGTAOScratchDepths[groupThreadID.x + 2][groupThreadID.y + 0];
        float inBL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 2];
        float inBR = g_XeGTAOScratchDepths[groupThreadID.x + 2][groupThreadID.y + 2];
        float mip3Depth = XeGTAO_DepthMipFilter(inTL, inTR, inBL, inBR, constants);
        uint2 outCoord = baseCoord / 4;
        if (outCoord.x < mip3Size.x && outCoord.y < mip3Size.y)
        {
            outDepth3[outCoord] = mip3Depth;
        }
        g_XeGTAOScratchDepths[groupThreadID.x][groupThreadID.y] = mip3Depth;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all((groupThreadID.xy % 8.xx) == 0))
    {
        float inTL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_XeGTAOScratchDepths[groupThreadID.x + 4][groupThreadID.y + 0];
        float inBL = g_XeGTAOScratchDepths[groupThreadID.x + 0][groupThreadID.y + 4];
        float inBR = g_XeGTAOScratchDepths[groupThreadID.x + 4][groupThreadID.y + 4];
        float mip4Depth = XeGTAO_DepthMipFilter(inTL, inTR, inBL, inBR, constants);
        uint2 outCoord = baseCoord / 8;
        if (outCoord.x < mip4Size.x && outCoord.y < mip4Size.y)
        {
            outDepth4[outCoord] = mip4Depth;
        }
    }
}

void XeGTAO_Denoise(
    uint2 pixCoordBase,
    XeGTAOConstants constants,
    Texture2D<uint> sourceAO,
    Texture2D<float> sourceEdges,
    SamplerState pointSampler,
    RWTexture2D<uint> outAO,
    bool finalApply)
{
    float blurAmount = finalApply ? constants.DenoiseBlurBeta : constants.DenoiseBlurBeta / 5.0;
    float2 uv = float2(pixCoordBase) * constants.InvViewportSize;

    float4 edgesQ0 = sourceEdges.GatherRed(pointSampler, uv, int2(0, 0));
    float4 edgesQ1 = sourceEdges.GatherRed(pointSampler, uv, int2(2, 0));
    float4 edgesQ2 = sourceEdges.GatherRed(pointSampler, uv, int2(1, 2));

    float4 aoQ0 = float4(sourceAO.GatherRed(pointSampler, uv, int2(0, 0))) / 255.0;
    float4 aoQ1 = float4(sourceAO.GatherRed(pointSampler, uv, int2(2, 0))) / 255.0;
    float4 aoQ2 = float4(sourceAO.GatherRed(pointSampler, uv, int2(0, 2))) / 255.0;
    float4 aoQ3 = float4(sourceAO.GatherRed(pointSampler, uv, int2(2, 2))) / 255.0;

    [unroll]
    for (int side = 0; side < 2; ++side)
    {
        uint2 pixCoord = uint2(pixCoordBase.x + side, pixCoordBase.y);
        if (pixCoord.x >= constants.ViewportSize.x || pixCoord.y >= constants.ViewportSize.y)
        {
            continue;
        }

        float4 edgesL = XeGTAO_UnpackEdges(side == 0 ? edgesQ0.x : edgesQ0.y);
        float4 edgesT = XeGTAO_UnpackEdges(side == 0 ? edgesQ0.z : edgesQ1.w);
        float4 edgesR = XeGTAO_UnpackEdges(side == 0 ? edgesQ1.x : edgesQ1.y);
        float4 edgesB = XeGTAO_UnpackEdges(side == 0 ? edgesQ2.w : edgesQ2.z);
        float4 edgesC = XeGTAO_UnpackEdges(side == 0 ? edgesQ0.y : edgesQ1.x);

        edgesC *= float4(edgesL.y, edgesR.x, edgesT.w, edgesB.z);

        float leak = (saturate(4.0 - 2.5 - dot(edgesC, 1.0.xxxx)) / (4.0 - 2.5)) * 0.5;
        edgesC = saturate(edgesC + leak);

        float centerAO = side == 0 ? aoQ0.y : aoQ1.x;
        float leftAO = side == 0 ? aoQ0.x : aoQ0.y;
        float topAO = side == 0 ? aoQ0.z : aoQ1.w;
        float rightAO = side == 0 ? aoQ1.x : aoQ1.y;
        float bottomAO = side == 0 ? aoQ2.z : aoQ3.w;

        float diagWeight = 0.85 * 0.5;
        float wTL = diagWeight * (edgesC.x * edgesL.z + edgesC.z * edgesT.x);
        float wTR = diagWeight * (edgesC.z * edgesT.y + edgesC.y * edgesR.z);
        float wBL = diagWeight * (edgesC.w * edgesB.x + edgesC.x * edgesL.w);
        float wBR = diagWeight * (edgesC.y * edgesR.w + edgesC.w * edgesB.y);

        float topLeftAO = side == 0 ? aoQ0.w : aoQ0.z;
        float topRightAO = side == 0 ? aoQ1.w : aoQ1.z;
        float bottomLeftAO = side == 0 ? aoQ2.w : aoQ2.z;
        float bottomRightAO = side == 0 ? aoQ3.w : aoQ3.z;

        float sumWeight = blurAmount;
        float sumAO = centerAO * sumWeight;

        sumAO += leftAO * edgesC.x;       sumWeight += edgesC.x;
        sumAO += rightAO * edgesC.y;      sumWeight += edgesC.y;
        sumAO += topAO * edgesC.z;        sumWeight += edgesC.z;
        sumAO += bottomAO * edgesC.w;     sumWeight += edgesC.w;
        sumAO += topLeftAO * wTL;         sumWeight += wTL;
        sumAO += topRightAO * wTR;        sumWeight += wTR;
        sumAO += bottomLeftAO * wBL;      sumWeight += wBL;
        sumAO += bottomRightAO * wBR;     sumWeight += wBR;

        float filteredAO = sumAO / sumWeight;
        if (finalApply)
        {
            filteredAO *= XE_GTAO_OCCLUSION_TERM_SCALE;
        }

        outAO[pixCoord] = uint(saturate(filteredAO) * 255.0 + 0.5);
    }
}

#endif


//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

#ifndef NYX_CULL_COMMON_SLANG
#define NYX_CULL_COMMON_SLANG

#define CULL_EPSILON 1.2e-07f
#define DEPTH_EPSILON (2.0 / float(1 << 24))
#define HZB_TARGET_FOOTPRINT_PIXELS 4

struct CullCommonContext
{
    uint ViewportWidth;
    uint ViewportHeight;
    uint HZBSourceWidth;
    uint HZBSourceHeight;
    float4 HZBSizeAndInv;
    float4 HZBUvScaleBias;
};

float4 BuildAabbCorner(float3 bboxMin, float3 bboxMax, uint cornerIndex)
{
    bool3 useMax = bool3((cornerIndex & 1u) != 0u, (cornerIndex & 2u) != 0u, (cornerIndex & 4u) != 0u);
    float3 selector = float3(
        useMax.x ? 1.0 : 0.0,
        useMax.y ? 1.0 : 0.0,
        useMax.z ? 1.0 : 0.0);
    return float4(lerp(bboxMin, bboxMax, selector), 1.0);
}

float4 ProjectToClip(float4 homogeneousPos, out bool validClip)
{
    validClip = homogeneousPos.w >= CULL_EPSILON;
    return float4(homogeneousPos.xyz * rcp(homogeneousPos.w), homogeneousPos.w);
}

uint ComputeHomogeneousClipMask(float4 homogeneousPos)
{
    uint mask = 0;
    mask |= homogeneousPos.x < -homogeneousPos.w ? 1u : 0u;
    mask |= homogeneousPos.x >  homogeneousPos.w ? 2u : 0u;
    mask |= homogeneousPos.y < -homogeneousPos.w ? 4u : 0u;
    mask |= homogeneousPos.y >  homogeneousPos.w ? 8u : 0u;
    mask |= homogeneousPos.z < 0.0 ? 16u : 0u;
    mask |= homogeneousPos.z > homogeneousPos.w ? 32u : 0u;
    mask |= homogeneousPos.w <= 0.0 ? 64u : 0u;
    return mask;
}

bool BBoxIntersectFrustum(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    out float4 clipMinOut,
    out float4 clipMaxOut,
    out bool clipValidOut)
{
    const float4x4 worldToClip = mul(viewProjMatrix, worldMatrix);

    bool validClip = false;
    float4 homogeneousPos = mul(worldToClip, BuildAabbCorner(bboxMin, bboxMax, 0u));
    float4 clipPos = ProjectToClip(homogeneousPos, validClip);

    uint rejectMask = ComputeHomogeneousClipMask(homogeneousPos);
    float4 clipMin = clipPos;
    float4 clipMax = clipPos;
    bool allCornersValid = validClip;

    [[unroll]]
    for (uint corner = 1u; corner < 8u; ++corner)
    {
        homogeneousPos = mul(worldToClip, BuildAabbCorner(bboxMin, bboxMax, corner));
        clipPos = ProjectToClip(homogeneousPos, validClip);
        rejectMask &= ComputeHomogeneousClipMask(homogeneousPos);

        clipMin = min(clipMin, clipPos);
        clipMax = max(clipMax, clipPos);
        allCornersValid = allCornersValid && validClip;
    }

    clipValidOut = allCornersValid;
    clipMinOut = float4(clamp(clipMin.xy, float2(-1.0, -1.0), float2(1.0, 1.0)), clipMin.zw);
    clipMaxOut = float4(clamp(clipMax.xy, float2(-1.0, -1.0), float2(1.0, 1.0)), clipMax.zw);

    return rejectMask == 0u;
}

bool LargeEnough(float4 clipMin, float4 clipMax, float threshold, CullCommonContext context)
{
    const float2 extentInPixels =
        (clipMax.xy - clipMin.xy) * 0.5 * float2(context.ViewportWidth, context.ViewportHeight);
    return any(extentInPixels >= float2(threshold, threshold));
}

int MipLevelForRect(int4 rectPixels, int desiredFootprintPixels)
{
    const int maxFootprintSpan = desiredFootprintPixels - 1;
    const int footprintMipBias = int(log2(float(desiredFootprintPixels))) - 1;
    const int2 rectSize = rectPixels.zw - rectPixels.xy;
    int2 dominantAxisMip = firstbithigh(rectSize);
    int mipLevel = max(max(dominantAxisMip.x, dominantAxisMip.y) - footprintMipBias, 0);

    int2 coarseMin = rectPixels.xy >> mipLevel;
    int2 coarseMax = rectPixels.zw >> mipLevel;
    mipLevel += any(coarseMax - coarseMin > maxFootprintSpan) ? 1 : 0;

    return mipLevel;
}

float4 SampleHZBRow4(
    Texture2D<float> hzb,
    SamplerState hzbSampler,
    float4 xCoords,
    float yCoord,
    float mipLevel)
{
    return float4(
        hzb.SampleLevel(hzbSampler, float2(xCoords.x, yCoord), mipLevel).r,
        hzb.SampleLevel(hzbSampler, float2(xCoords.y, yCoord), mipLevel).r,
        hzb.SampleLevel(hzbSampler, float2(xCoords.z, yCoord), mipLevel).r,
        hzb.SampleLevel(hzbSampler, float2(xCoords.w, yCoord), mipLevel).r);
}

bool HZBVisible(
    Texture2D<float> hzb,
    SamplerState hzbSampler,
    float4 clipMin,
    float4 clipMax,
    CullCommonContext context)
{
    if (clipMin.x > 1.0 || clipMin.y > 1.0 || clipMax.x < -1.0 || clipMax.y < -1.0)
        return false;

    // uvRect = (minX, minY, maxX, maxY), with Y inverted from clip to texture space.
    float4 uvRect = saturate(float4(clipMin.xy, clipMax.xy) * float2(0.5, -0.5).xyxy + 0.5).xwzy;
    uvRect = uvRect * context.HZBUvScaleBias.xyxy + context.HZBUvScaleBias.zwzw;

    int4 pixelsRect = int4(uvRect * float4(
            context.HZBSourceWidth,
            context.HZBSourceHeight,
            context.HZBSourceWidth,
            context.HZBSourceHeight) +
        float4(0.5, 0.5, -0.5, -0.5));

    int2 rectMin = min(pixelsRect.xy, pixelsRect.zw);
    int2 rectMax = max(pixelsRect.xy, pixelsRect.zw);
    pixelsRect = int4(rectMin, rectMax);

    int4 coarseRect = pixelsRect >> 1;
    int hzbMip = MipLevelForRect(coarseRect, HZB_TARGET_FOOTPRINT_PIXELS);
    coarseRect >>= hzbMip;

    // TexelSize = (1 / HZBSize) * 2^mip. Assumes power-of-two HZB dimensions.
    float2 texelSize = asfloat(0x7F000000 - asint(context.HZBSizeAndInv.xy) + (hzbMip << 23));

    float4 xCoords = (min(coarseRect.x + int4(0, 1, 2, 3), coarseRect.z) + 0.5) * texelSize.x;
    float4 yCoords = (min(coarseRect.y + int4(0, 1, 2, 3), coarseRect.w) + 0.5) * texelSize.y;

    float4 depthRow0 = SampleHZBRow4(hzb, hzbSampler, xCoords, yCoords.x, hzbMip);
    float4 depthRow1 = SampleHZBRow4(hzb, hzbSampler, xCoords, yCoords.y, hzbMip);
    float4 depthRow2 = SampleHZBRow4(hzb, hzbSampler, xCoords, yCoords.z, hzbMip);
    float4 depthRow3 = SampleHZBRow4(hzb, hzbSampler, xCoords, yCoords.w, hzbMip);
    float4 depthMinByCol = min(min(min(depthRow0, depthRow1), depthRow2), depthRow3);
    float minDepth = min(min(min(depthMinByCol.x, depthMinByCol.y), depthMinByCol.z), depthMinByCol.w);

    return clipMax.z + DEPTH_EPSILON >= minDepth;
}

float2 GetScreenPos(float4 clipPos, CullCommonContext context)
{
    float2 ndc = clipPos.xy / clipPos.w;
    return (ndc * 0.5 + 0.5) * float2(float(context.ViewportWidth), float(context.ViewportHeight));
}

bool IsTriangleOutsideFrustum(float4 h0, float4 h1, float4 h2)
{
    uint cullBits =
        ComputeHomogeneousClipMask(h0) &
        ComputeHomogeneousClipMask(h1) &
        ComputeHomogeneousClipMask(h2);
    return cullBits != 0;
}

bool IsTinyTriangle(float4 h0, float4 h1, float4 h2, CullCommonContext context)
{
    return false;

    //float2 a = GetScreenPos(h0, context);
    //float2 b = GetScreenPos(h1, context);
    //float2 c = GetScreenPos(h2, context);

    //float2 pixelMin = min(a, min(b, c));
    //float2 pixelMax = max(a, max(b, c));

    //const float epsilon = 1.0f / 256.0f;
    //pixelMin -= epsilon;
    //pixelMax += epsilon;

    //int2 ipMin = int2(round(pixelMin));
    //int2 ipMax = int2(round(pixelMax));

    //return (ipMin.x == ipMax.x) && (ipMin.y == ipMax.y);
}

bool TestForLod(
    float4x4 worldMatrix,
    float3 cameraPos,
    float error,
    float4 sphereLS,
    float lodScale,
    float pixelErrorThreshold,
    bool isPerspective)
{
    float3 axisX = float3(worldMatrix._m00, worldMatrix._m10, worldMatrix._m20);
    float3 axisY = float3(worldMatrix._m01, worldMatrix._m11, worldMatrix._m21);
    float3 axisZ = float3(worldMatrix._m02, worldMatrix._m12, worldMatrix._m22);
    float uniformScaleUpperBound = max(length(axisX), max(length(axisY), length(axisZ)));

    float4 sphereWS = float4(mul(worldMatrix, float4(sphereLS.xyz, 1.0)).xyz, uniformScaleUpperBound * sphereLS.w);
    float geometricErrorWS = error * uniformScaleUpperBound;

    const float lodFactor = 1.0;
    float cameraToSphereSurface = max(0.0, length(cameraPos - sphereWS.xyz) - sphereWS.w);
    float distanceScale = isPerspective ? cameraToSphereSurface : 1.0;
    float allowedErrorWS = distanceScale * lodScale * pixelErrorThreshold;
    return allowedErrorWS * lodFactor >= geometricErrorWS;
}

bool EvaluateBoundsVisibility(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    Texture2D<float> hzbTexture,
    SamplerState hzbSampler,
    CullCommonContext context)
{
    float4 clipMin;
    float4 clipMax;
    bool clipValid = false;

    bool inFrustum = BBoxIntersectFrustum(
        bboxMin,
        bboxMax,
        worldMatrix,
        viewProjMatrix,
        clipMin,
        clipMax,
        clipValid);

    if (!inFrustum)
        return false;
    if (!clipValid)
        return true;

    return LargeEnough(clipMin, clipMax, 1.0, context) &&
        HZBVisible(hzbTexture, hzbSampler, clipMin, clipMax, context);
}

bool EvaluateBoundsInFrustum(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    CullCommonContext context)
{
    float4 clipMin;
    float4 clipMax;
    bool clipValid = false;

    bool inFrustum = BBoxIntersectFrustum(
        bboxMin,
        bboxMax,
        worldMatrix,
        viewProjMatrix,
        clipMin,
        clipMax,
        clipValid);

    if (!inFrustum)
        return false;
    if (!clipValid)
        return true;

    return LargeEnough(clipMin, clipMax, 1.0, context);
}

#endif

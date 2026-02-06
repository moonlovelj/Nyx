#ifndef __CULLING_COMMON_HLSLI__
#define __CULLING_COMMON_HLSLI__

#include "Common.hlsli"

#define CULL_EPSILON 1.2e-07f
#define DEPTH_EPSILON (2.0 / float(1 << 24))
#define HZB_TARGET_FOOTPRINT_PIXELS 4

float4 BuildAabbCorner(float3 bboxMin, float3 bboxMax, uint cornerIndex)
{
    bool3 useMax = bool3((cornerIndex & 1u) != 0u, (cornerIndex & 2u) != 0u, (cornerIndex & 4u) != 0u);
    return float4(lerp(bboxMin, bboxMax, useMax), 1.0f);
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
    mask |= homogeneousPos.z < 0.0f ? 16u : 0u;
    mask |= homogeneousPos.z > homogeneousPos.w ? 32u : 0u;
    mask |= homogeneousPos.w <= 0.0f ? 64u : 0u;
    return mask;
}

bool BBoxIntersectFrustum(
    float3 bboxMin, float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    out float4 oClipmin, out float4 oClipmax, out bool oClipvalid)
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

    oClipvalid = allCornersValid;
    oClipmin = float4(clamp(clipMin.xy, float2(-1.0f, -1.0f), float2(1.0f, 1.0f)), clipMin.zw);
    oClipmax = float4(clamp(clipMax.xy, float2(-1.0f, -1.0f), float2(1.0f, 1.0f)), clipMax.zw);

    return rejectMask == 0u;
}

bool LargeEnough(float4 clipMin, float4 clipMax, float threshold)
{
    const float2 extentInPixels = (clipMax.xy - clipMin.xy) * 0.5f * float2(ViewportWidth, ViewportHeight);
    return any(extentInPixels >= float2(threshold, threshold));
}

int MipLevelForRect(int4 RectPixels, int DesiredFootprintPixels)
{
    const int maxFootprintSpan = DesiredFootprintPixels - 1;
    const int footprintMipBias = (int)log2((float)DesiredFootprintPixels) - 1;
    const int2 rectSize = RectPixels.zw - RectPixels.xy;
    int2 dominantAxisMip = firstbithigh(rectSize);
    int mipLevel = max(max(dominantAxisMip.x, dominantAxisMip.y) - footprintMipBias, 0);

    int2 coarseMin = RectPixels.xy >> mipLevel;
    int2 coarseMax = RectPixels.zw >> mipLevel;
    mipLevel += any(coarseMax - coarseMin > maxFootprintSpan) ? 1 : 0;

    return mipLevel;
}

float4 SampleHZBRow4(Texture2D<float> hzb, float4 xCoords, float yCoord, float mipLevel)
{
    return float4(
        hzb.SampleLevel(pointSampler, float2(xCoords.x, yCoord), mipLevel).r,
        hzb.SampleLevel(pointSampler, float2(xCoords.y, yCoord), mipLevel).r,
        hzb.SampleLevel(pointSampler, float2(xCoords.z, yCoord), mipLevel).r,
        hzb.SampleLevel(pointSampler, float2(xCoords.w, yCoord), mipLevel).r);
}

bool HZBVisible(
    Texture2D<float> hzb,
    float4 clipMin,
    float4 clipMax)
{
    if (clipMin.x > 1.0f || clipMin.y > 1.0f || clipMax.x < -1.0f || clipMax.y < -1.0f)
    {
        return false;
    }

    // uvRect = (minX, minY, maxX, maxY), with Y inverted from clip to texture space.
    float4 uvRect = saturate(float4(clipMin.xy, clipMax.xy) * float2(0.5f, -0.5f).xyxy + 0.5f).xwzy;

    int4 pixelsRect = int4(uvRect * float4(ViewportWidth, ViewportHeight, ViewportWidth, ViewportHeight) +
        float4(0.5f, 0.5f, -0.5f, -0.5f));
    pixelsRect.xy = min(pixelsRect.xy, pixelsRect.zw);
    pixelsRect.zw = max(pixelsRect.xy, pixelsRect.zw);

    int4 coarseRect = pixelsRect >> 1;
    int hzbMip = MipLevelForRect(coarseRect, HZB_TARGET_FOOTPRINT_PIXELS);
    coarseRect >>= hzbMip;

    // TexelSize = (1 / HZBSize) * 2^mip. Assumes power-of-two HZB dimensions.
    float2 texelSize = asfloat(0x7F000000 - asint(HZBSizeAndInv.xy) + (hzbMip << 23));

    float4 xCoords = (min(coarseRect.x + int4(0, 1, 2, 3), coarseRect.z) + 0.5f) * texelSize.x;
    float4 yCoords = (min(coarseRect.y + int4(0, 1, 2, 3), coarseRect.w) + 0.5f) * texelSize.y;

    float4 depthRow0 = SampleHZBRow4(hzb, xCoords, yCoords.x, hzbMip);
    float4 depthRow1 = SampleHZBRow4(hzb, xCoords, yCoords.y, hzbMip);
    float4 depthRow2 = SampleHZBRow4(hzb, xCoords, yCoords.z, hzbMip);
    float4 depthRow3 = SampleHZBRow4(hzb, xCoords, yCoords.w, hzbMip);
    float4 depthMinByCol = min(min(min(depthRow0, depthRow1), depthRow2), depthRow3);
    float minDepth = min(min(min(depthMinByCol.x, depthMinByCol.y), depthMinByCol.z), depthMinByCol.w);

    return clipMax.z + DEPTH_EPSILON >= minDepth;
}

bool TestForLod(float4x4 WorldMatrix, float3 CameraPos, float error, float4 sphereLS)
{
    float3 axisX = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 axisY = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 axisZ = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float uniformScaleUpperBound = max(length(axisX), max(length(axisY), length(axisZ)));

    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1.0f)).xyz, uniformScaleUpperBound * sphereLS.w);
    float allowedErrorWS = error * uniformScaleUpperBound;

    const float lodFactor = 1.0f;
    float cameraToSphereSurface = max(0.0f, length(CameraPos - sphereWS.xyz) - sphereWS.w);
    float projectedError = cameraToSphereSurface * ScreenErrorConstant;
    return projectedError * lodFactor >= allowedErrorWS;
}

bool EvaluateBoundsVisibility(
    float3 bboxMin,
    float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    Texture2D<float> hzbTexture)
{
    float4 clipMin;
    float4 clipMax;
    bool clipValid = false;

    bool inFrustum = BBoxIntersectFrustum(
        bboxMin, bboxMax,
        worldMatrix,
        viewProjMatrix,
        clipMin, clipMax,
        clipValid);

    if (!inFrustum)
    {
        return false;
    }

    if (!clipValid)
    {
        return true;
    }

    return LargeEnough(clipMin, clipMax, 1.0f) && HZBVisible(hzbTexture, clipMin, clipMax);
}

#endif

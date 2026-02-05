#ifndef __CULLING_COMMON_HLSLI__
#define __CULLING_COMMON_HLSLI__

#include "Common.hlsli"

#define FLT_MAX 3.402823466e+38
#define CULL_EPSILON 1.2e-07f
#define DEPTH_EPSILON (2.0 / float(1 << 24))

#define DESIRED_FOOTPRINT_PIXELS 4

bool IsSphereInFrustum(float4x4 WorldMatrix, float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));

    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    float4 sphereVS = float4(mul(ViewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);

    // Sphere: xyz = center, w = radius
    [unroll]
        for (int i = 0; i < 6; ++i)
        {
            float4 plane;
            if (i == 0)
                plane = ViewSpaceFrustumPlanes0;
            else if (i == 1)
                plane = ViewSpaceFrustumPlanes1;
            else if (i == 2)
                plane = ViewSpaceFrustumPlanes2;
            else if (i == 3)
                plane = ViewSpaceFrustumPlanes3;
            else if (i == 4)
                plane = ViewSpaceFrustumPlanes4;
            else
                plane = ViewSpaceFrustumPlanes5;
            float distance = dot(plane.xyz, sphereVS.xyz) + plane.w;
            if (distance < -sphereVS.w)
                return false;
        }
    return true;
}

float4 GetBoxCorner(float3 bboxMin, float3 bboxMax, int n)
{
    bool3 useMax = bool3((n & 1) != 0, (n & 2) != 0, (n & 4) != 0);
    return float4(lerp(bboxMin, bboxMax, useMax), 1);
}

float4 GetClip(float4 hPos, out bool valid)
{
    valid = hPos.w >= CULL_EPSILON;
    return float4(hPos.xyz / hPos.w, hPos.w);
}

uint GetCullBits(float4 hPos)
{
    uint cullBits = 0;
    cullBits |= hPos.x < -hPos.w ? 1 : 0;
    cullBits |= hPos.x > hPos.w ? 2 : 0;
    cullBits |= hPos.y < -hPos.w ? 4 : 0;
    cullBits |= hPos.y > hPos.w ? 8 : 0;
    cullBits |= hPos.z < 0 ? 16 : 0;
    cullBits |= hPos.z > hPos.w ? 32 : 0;
    cullBits |= hPos.w <= 0 ? 64 : 0;
    return cullBits;
}

bool BBoxIntersectFrustum(
    float3 bboxMin, float3 bboxMax,
    float4x4 worldMatrix,
    float4x4 viewProjMatrix,
    out float4 oClipmin, out float4 oClipmax, out bool oClipvalid)
{
    float4x4 worldViewProj = mul(viewProjMatrix, worldMatrix);
    bool valid;
    float4 hPos = mul(worldViewProj, GetBoxCorner(bboxMin, bboxMax, 0));
    float4 clip = GetClip(hPos, valid);
    uint bits = GetCullBits(hPos);
    float4 clipMin = clip;
    float4 clipMax = clip;
    bool clipValid = valid;

    [[unroll]]
    for (int n = 1; n < 8; n++)
    {
        hPos = mul(worldViewProj, GetBoxCorner(bboxMin, bboxMax, n));
        clip = GetClip(hPos, valid);
        bits &= GetCullBits(hPos);

        clipMin = min(clipMin, clip);
        clipMax = max(clipMax, clip);

        clipValid = clipValid && valid;
    }

    oClipvalid = clipValid;
    oClipmin = float4(clamp(clipMin.xy, float2(-1, -1), float2(1, 1)), clipMin.zw);
    oClipmax = float4(clamp(clipMax.xy, float2(-1, -1), float2(1, 1)), clipMax.zw);

    return bits == 0;
}

bool LargeEnough(float4 clipMin, float4 clipMax, float threshold)
{
    //return true;
    float2 rect = (clipMax.xy - clipMin.xy) * 0.5 * float2(ViewportWidth, ViewportHeight);
    float2 clipThreshold = float2(threshold, threshold);
    return any(rect >= clipThreshold);
}

float GetMipLevel(float2 texelRectSize)
{
    float maxDim = max(texelRectSize.x, texelRectSize.y);
    float logicalMip = floor(log2(max(maxDim, 1.0)));
    return max(logicalMip, 0.0);
}

float2 ProjectSphere(float x, float z, float r, float ResultScale)
{
    float t = sqrt(x * x + z * z - r * r);

    float A = (t * z + r * x);
    float B = (t * z - r * x);
    ResultScale /= (A * B);	// Divide by common denominator instead of dividing twice

    float Min = (t * x - r * z) * B;
    float Max = (t * x + r * z) * A;

    return float2(Min, Max) * ResultScale;
}

// [ Mara & Morgan 2013, "2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere" ]
float4 SphereToScreenRect(float3 Center, float Radius, float4x4 ViewToClip)
{
    float viewDist = -Center.z;
    float2 ExtentX = ProjectSphere(Center.x, viewDist, Radius, ViewToClip[0][0]) - ViewToClip[0][2];
    float2 ExtentY = ProjectSphere(Center.y, viewDist, Radius, ViewToClip[1][1]) - ViewToClip[1][2];

    return float4(ExtentX.x, ExtentY.x, ExtentX.y, ExtentY.y);
}

// From UE5 HZB code
// Rect is inclusive [Min.xy, Max.xy]
int MipLevelForRect(int4 RectPixels, int DesiredFootprintPixels)
{
    const int MaxPixelOffset = DesiredFootprintPixels - 1;
    const int MipOffset = (int)log2((float)DesiredFootprintPixels) - 1;

    // Calculate lowest mip level that allows us to cover footprint of the desired size in pixels.
    // Start by calculating separate x and y mip level requirements.
    // 2 pixels of mip k cover 2^(k+1) pixels of mip 0. To cover at least n pixels of mip 0 by two pixels of mip k we need k to be at least k = ceil( log2( n ) ) - 1.
    // For integer n>1: ceil( log2( n ) ) = floor( log2( n - 1 ) ) + 1.
    // So k = floor( log2( n - 1 )
    // For integer n>1: floor( log2( n ) ) = firstbithigh( n )
    // So k = firstbithigh( n - 1 )
    // As RectPixels min/max are both inclusive their difference is one less than number of pixels (n - 1), so applying firstbithigh to this difference gives the minimum required mip.
    // NOTE: firstbithigh is a FULL rate instruction on GCN while log2 is QUARTER rate instruction.
    int2 MipLevelXY = firstbithigh(RectPixels.zw - RectPixels.xy);

    // Mip level needs to be big enough to cover both x and y requirements. Go one extra level down for 4x4 sampling.
    // firstbithigh(0) = -1, so clamping with 0 here also handles the n=1 case where mip 0 footprint is just 1 pixel wide/tall.
    int MipLevel = max(max(MipLevelXY.x, MipLevelXY.y) - MipOffset, 0);

    // MipLevel now contains the minimum MipLevel that can cover a number of pixels equal to the size of the rectangle footprint, but the HZB footprint alignments are quantized to powers of two.
    // The quantization can translate down the start of the represented range by up to 2^k-1 pixels, which can decrease the number of usable pixels down to 2^(k+1) - 2^k-1.
    // Depending on the alignment of the rectangle this might require us to pick one level higher to cover all rectangle footprint pixels.
    // Note that testing one level higher is always enough as this guarantees 2^(k+2) - 2^k usable pixels after alignment, which is more than the 2^(k+1) required pixels.

    // Transform coordinates down to coordinates of selected mip level and if they are not within reach increase level by one.
    MipLevel += any((RectPixels.zw >> MipLevel) - (RectPixels.xy >> MipLevel) > MaxPixelOffset) ? 1 : 0;

    return MipLevel;
}

float4 LoadHZBRow4(Texture2D<float> hzb, float4 XCoords, float YCoord, float MipLevel)
{
    return float4(
        hzb.SampleLevel(pointSampler, float2(XCoords.x, YCoord), MipLevel).r,
        hzb.SampleLevel(pointSampler, float2(XCoords.y, YCoord), MipLevel).r,
        hzb.SampleLevel(pointSampler, float2(XCoords.z, YCoord), MipLevel).r,
        hzb.SampleLevel(pointSampler, float2(XCoords.w, YCoord), MipLevel).r);
}

bool HZBVisible(
    Texture2D<float> hzb,
    float4 clipMin,
    float4 clipMax)
{
    if (clipMin.x > 1 || clipMin.y > 1 || clipMax.x < -1 || clipMax.y < -1)
        return false;

    // uvRect minx miny maxx maxy
    float4 uvRect = saturate(float4(clipMin.xy, clipMax.xy) * float2(0.5, -0.5).xyxy + 0.5).xwzy;

    int4 pixelsRect = int4(uvRect * float4(ViewportWidth, ViewportHeight, ViewportWidth, ViewportHeight) + float4(0.5f, 0.5f, -0.5f, -0.5f));
    pixelsRect.xy = min(pixelsRect.xy, pixelsRect.zw);
    pixelsRect.zw = max(pixelsRect.xy, pixelsRect.zw);

    int4 hzbPixelRect = pixelsRect >> 1;
    int HZBLevel = MipLevelForRect(hzbPixelRect, DESIRED_FOOTPRINT_PIXELS);
    hzbPixelRect >>= HZBLevel;

    // TexelSize = (1 / HZBSize) * exp2(MipLevel);
    float2 TexelSize = asfloat(0x7F000000 - asint(HZBSizeAndInv.xy) + (HZBLevel << 23)); // Assumes HZB is po2

    float4 XCoords = (min(hzbPixelRect.x + int4(0, 1, 2, 3), hzbPixelRect.z) + 0.5f) * TexelSize.x;
    float4 YCoords = (min(hzbPixelRect.y + int4(0, 1, 2, 3), hzbPixelRect.w) + 0.5f) * TexelSize.y;

    float4 Depth0 = LoadHZBRow4(hzb, XCoords, YCoords.x, HZBLevel);
    float4 Depth1 = LoadHZBRow4(hzb, XCoords, YCoords.y, HZBLevel);
    float4 Depth2 = LoadHZBRow4(hzb, XCoords, YCoords.z, HZBLevel);
    float4 Depth3 = LoadHZBRow4(hzb, XCoords, YCoords.w, HZBLevel);
    float4 Depth = min(min(min(Depth0, Depth1), Depth2), Depth3);
    float MinDepth = min(min(min(Depth.x, Depth.y), Depth.z), Depth.w);

    return clipMax.z + DEPTH_EPSILON >= MinDepth;
}

bool TestForLod(float4x4 WorldMatrix, float3 CameraPos, float error, float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));

    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    float errorWS = error * sphereScale;

	const float lod_factor = 1.0;
    float d = max(0, length(CameraPos-sphereWS.xyz) - sphereWS.w);
    float e = d * ScreenErrorConstant; // 1px in mesh space
    bool lod_ok = e * lod_factor >= errorWS;
    return lod_ok;
}

#endif

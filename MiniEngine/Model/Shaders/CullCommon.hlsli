#ifndef __CULLING_COMMON_HLSLI__
#define __CULLING_COMMON_HLSLI__

#include "Common.hlsli"

#define FLT_MAX 3.402823466e+38

#define DESIRED_FOOTPRINT_PIXELS 4
#define TINY_NODE_PIXEL_DIAMETER 8.0f
#define TINY_CLUSTER_PIXEL_DIAMETER 4.0f

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

bool IsSphereTiny(float4x4 WorldMatrix, float4 sphereLS, float pixelThreshold)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));

    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    float4 sphereVS = float4(mul(ViewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);

    float viewZ = -sphereVS.z;
    if (viewZ <= 1e-6f || viewZ <= sphereVS.w)
        return false;

    float cotHalfFovY = ProjMatrix._m11;
    float screenErrorConstant = cotHalfFovY * ViewportHeight * 0.5f;

    float radiusPixels = sphereVS.w * screenErrorConstant / viewZ;
    return (radiusPixels * 2.0f) < pixelThreshold;
}

float GetMipLevel(float2 texelRectSize)
{
    float maxDim = max(texelRectSize.x, texelRectSize.y);
    float logicalMip = floor(log2(max(maxDim, 1.0)));
    return max(logicalMip, 0.0);
}

float4 ComputeAABBUVRect(float3 boxMin, float3 boxMax, float4x4 vpMatrix)
{
    float3 corners[8];
    corners[0] = float3(boxMin.x, boxMin.y, boxMin.z);
    corners[1] = float3(boxMax.x, boxMin.y, boxMin.z);
    corners[2] = float3(boxMin.x, boxMax.y, boxMin.z);
    corners[3] = float3(boxMax.x, boxMax.y, boxMin.z);
    corners[4] = float3(boxMin.x, boxMin.y, boxMax.z);
    corners[5] = float3(boxMax.x, boxMin.y, boxMax.z);
    corners[6] = float3(boxMin.x, boxMax.y, boxMax.z);
    corners[7] = float3(boxMax.x, boxMax.y, boxMax.z);

    float2 uvMin = float2(FLT_MAX, FLT_MAX);
    float2 uvMax = float2(-FLT_MAX, -FLT_MAX);

    uint validCount = 0;

    [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            float4 clip = mul(vpMatrix, float4(corners[i], 1.0));

            if (clip.w <= 0.0f)
                continue;

            float3 ndc = clip.xyz / clip.w;
            float2 uv = ndc.xy * 0.5f + 0.5f;
            uv.y = 1.0f - uv.y;

            uvMin = min(uvMin, uv);
            uvMax = max(uvMax, uv);
            validCount++;
        }

    if (validCount == 0)
    {
        return float4(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    if (uvMax.x < 0 || uvMin.x > 1 || uvMax.y < 0 || uvMin.y > 1)
    {
        return float4(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    float4 uvRect = float4(uvMin.x, uvMin.y, uvMax.x, uvMax.y);
    uvRect = saturate(uvRect);

    return uvRect;
}

float SampleHZBMinDepth(Texture2D<float> hzb, float4 texUVRect, float mip)
{
    float2 rectSize = float2(texUVRect.z - texUVRect.x, texUVRect.w - texUVRect.y);
    // 采用4x4采样网格，避免深度空洞产生的错误剔除
    const uint grid = 4;
    float2 step = rectSize / (grid - 1);
    float minDepth = FLT_MAX;
    float2 start = float2(texUVRect.x, texUVRect.y);

    [unroll]
        for (uint iy = 0; iy < grid; ++iy)
        {
            float v = start.y + step.y * iy;
            [unroll]
                for (uint ix = 0; ix < grid; ++ix)
                {
                    float u = start.x + step.x * ix;
                    float d = hzb.SampleLevel(pointSampler, float2(u, v), mip).r;
                    minDepth = min(minDepth, d);
                }
        }
    return minDepth;
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

bool IsSphereNotOccluded(
    Texture2D<float> hzb,
    float3 viewPos,
    float4x4 WorldMatrix,
    float4x4 viewMatrix,
    float4x4 projMatrix,
    float4x4 vpMatrix,
    float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));
    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    float4 sphereVS = float4(mul(viewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);

    return true;

   if (abs(sphereVS.z) < sphereVS.w + 0.1)
        return true;

    float4 ndcCullRect = SphereToScreenRect(sphereVS.xyz, sphereVS.w, projMatrix);

    if (ndcCullRect.z < -1 || ndcCullRect.x > 1 || ndcCullRect.w < -1 || ndcCullRect.y > 1)
        return false;

    return true;

    float4 uvRect = saturate(float4(ndcCullRect.xy, ndcCullRect.zw) * float2(0.5, -0.5).xyxy + 0.5).xwzy;
    int4 pixelsRect = int4(uvRect * float4(ViewportWidth, ViewportHeight, ViewportWidth, ViewportHeight) + float4(0.5f, 0.5f, -0.5f, -0.5f));
	pixelsRect.xy = min(pixelsRect.xy, pixelsRect.zw);
	pixelsRect.zw = max(pixelsRect.xy, pixelsRect.zw);

    int4 hzbPixelRect = pixelsRect >> 1;
    int HZBLevel = MipLevelForRect(hzbPixelRect, DESIRED_FOOTPRINT_PIXELS);
	hzbPixelRect >>= HZBLevel;

    // TexelSize = (1 / HZBSize) * exp2(MipLevel);
    float2 TexelSize = asfloat(0x7F000000 - asint(HZBSizeAndInv.xy) + (HZBLevel << 23));		// Assumes HZB is po2

    float4 XCoords = (min(hzbPixelRect.x + int4(0, 1, 2, 3), hzbPixelRect.z) + 0.5f) * TexelSize.x;
    float4 YCoords = (min(hzbPixelRect.y + int4(0, 1, 2, 3), hzbPixelRect.w) + 0.5f) * TexelSize.y;
    
    float4 Depth0 = LoadHZBRow4(hzb, XCoords, YCoords.x, HZBLevel);
    float4 Depth1 = LoadHZBRow4(hzb, XCoords, YCoords.y, HZBLevel);
    float4 Depth2 = LoadHZBRow4(hzb, XCoords, YCoords.z, HZBLevel);
    float4 Depth3 = LoadHZBRow4(hzb, XCoords, YCoords.w, HZBLevel);
    float4 Depth = min(min(min(Depth0, Depth1), Depth2), Depth3);
    float MinDepth = min(min(min(Depth.x, Depth.y), Depth.z), Depth.w);

    float closestZVS = sphereVS.z + sphereVS.w;
    float sphereClosestDepth = (projMatrix._m22 * closestZVS + projMatrix._m23) / -closestZVS;
    const float DepthBias = 0;//0.0001f;
    return sphereClosestDepth + DepthBias >= MinDepth;
}

bool TestForLod(float4x4 WorldMatrix, float3 CameraPos, float error, float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));

    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    //float4 sphereVS = float4(mul(ViewMatrix, float4(sphereWS.xyz, 1)).xyz, sphereWS.w);
    float errorWS = error * sphereScale;

	const float lod_factor = 1.0;
    float d = max(0, length(CameraPos-sphereWS.xyz) - sphereWS.w);
    float e = d * ScreenErrorConstant; // 1px in mesh space
    bool lod_ok = e * lod_factor >= errorWS;
    return lod_ok;
}

#endif

#include "CommonRS.hlsli"
#include "IBLPrecomputeCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

cbuffer CSConstants : register(b0)
{
    uint IBLCubeMapSize;
};

Texture2D<float4> IBLPanoramaTexture : register(t0);

RWTexture2DArray<float4> IBLCubeMap : register(u0);

SamplerState linearSampler : register(s0);

float2 ConvertDirToPanoramaUV(float3 dir)
{
    const float rotateAngle = -0.5 * PI;
    float cosine = cos(rotateAngle);
    float sinine = sin(rotateAngle);
    
    float2x2 rotation =
    {
        cosine, sinine,
        -sinine, cosine,
    };
    
    dir.xz = mul(rotation, dir.xz);
    
    float theta = atan2(dir.z, dir.x); // -pi ~ pi
    float phi = acos(dir.y); // 0 ~ pi
    
    float u_hdri = 1.0 - (theta + PI) / (2.0 * PI); // 0~1
    float v_hdri = phi / PI; // 0~1
    
    return float2(u_hdri, v_hdri);
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint3 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 Pixel = Gid.xy * uint2(8, 8) + GTid;
    if (Pixel.x < IBLCubeMapSize && Pixel.y < IBLCubeMapSize && Gid.z < 6)
    {
        float3 dir = ConvertCubePixelToDir(Pixel.x, Pixel.y, Gid.z, IBLCubeMapSize);
        dir.z = -dir.z;
        float2 uv = ConvertDirToPanoramaUV(dir);
        IBLCubeMap[uint3(Pixel, Gid.z)] = float4(IBLPanoramaTexture.SampleLevel(linearSampler, uv, 0).rgb, 1.f);
    }
}
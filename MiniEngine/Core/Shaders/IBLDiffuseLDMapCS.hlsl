
#include "CommonRS.hlsli"
#include "IBLPrecomputeCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

cbuffer CSConstants : register(b0)
{
    uint HDRITextureSize;
    uint HDRIMipCount;
    uint DiffuseLDMapSize;
};


TextureCube<float4> IBLHDRITexture          : register(t0);

RWTexture2DArray<float4> IBLDiffuseLDMapTexture : register(u0);

SamplerState cubeMapSampler : register(s0);

static const uint kSampleCount = 64;

float4 IntegrateDiffuseCube(float3 N)
{
    float3 accBrdf = 0;
    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 eta = Hammersley(i, kSampleCount);
        float3 L;
        float NdotL;
        float pdf;
        // see reference code in appendix 11
        ImportanceSampleCosDir(eta, N, L, NdotL, pdf);
        if (NdotL > 0)
        {
            float mipLevel = clamp(ComputeMipLevel(kSampleCount, pdf, HDRITextureSize) + 1, 0, HDRIMipCount - 1);            
	        accBrdf += IBLHDRITexture.SampleLevel(cubeMapSampler, L, mipLevel).rgb;
        }
            
    }
    return float4(accBrdf * (1.0 / kSampleCount), 1.0);
}

float4 IntegrateIrradiance(float3 N)
{
    float3 irradiance = 0;

   /* float deltaOmega = 4.0 * PI / (6.0 * HDRITextureSize * HDRITextureSize);
    for (int x = 0; x < HDRITextureSize; ++x)
    {
        for (int y = 0; y < HDRITextureSize; ++y)
	    {
            for (int z = 0; z < 6; ++z)
		    {
                float3 L = ConvertCubePixelToDir(x, y, z, HDRITextureSize);
                float NdotL = dot(N, L);
                if (NdotL > 0.0)
                {
                    irradiance += IBLHDRITexture.SampleLevel(cubeMapSampler, L, 0).rgb * NdotL * deltaOmega;
                }
            }
	    }
    }*/

    //float3 upVector = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    //float3 tangentX = normalize(cross(upVector, N));
    //float3 tangentY = cross(N, tangentX);

    float3 upVector = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangentX = normalize(cross(upVector, N));
    float3 tangentY = cross(N, tangentX);


    float sampleDelta = 0.005;
    float nrSamples = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // spherical to cartesian (in tangent space)
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            // tangent space to world
            float3 L = tangentSample.x * tangentX + tangentSample.y * tangentY + tangentSample.z * N;

            irradiance += IBLHDRITexture.SampleLevel(cubeMapSampler, L, 0).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }

    irradiance = irradiance * PI * PI * 1.0 / (nrSamples);

    return float4(irradiance, 1.0);
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint3 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 Pixel = Gid.xy * uint2(8, 8) + GTid;
    if (Pixel.x < DiffuseLDMapSize && Pixel.y < DiffuseLDMapSize)
    {
        float3 N = ConvertCubePixelToDir(Pixel.x, Pixel.y, Gid.z, DiffuseLDMapSize);
        float4 Acc = IntegrateDiffuseCube(N);
        IBLDiffuseLDMapTexture[uint3(Pixel, Gid.z)] = Acc;
    }
}
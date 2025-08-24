#include "CommonRS.hlsli"
#include "IBLPrecomputeCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

cbuffer CSConstants : register(b0)
{
    uint HDRITextureSize;
    uint HDRIMipCount;
    uint SpecularLDMapSize;
    float Roughness;
};


TextureCube<float4> IBLHDRITexture          : register(t0);

RWTexture2DArray<float4> IBLSpecularLDMap : register(u0);

SamplerState cubeMapSampler : register(s0);

static const uint kSampleCount = 1024;

float4 IntegrateCubeLDOnly(in float3 V, in float3 N, in float roughness)
{
    float3 accBrdf = 0;
    float  accBrdfWeight = 0;

    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 eta = Hammersley(i, kSampleCount);
        float3 L;
        float3 H;

        ImportanceSampleGGX(eta, roughness, V, N, H, L);

        float NdotL = saturate(dot(N, L));
        if (NdotL > 0)
        {
            // Use pre-filtered importance sampling 
            // (lower mipmap level for low-probability samples to reduce variance)
            // Reference: GPU Gems 3

            // Since we pre-integrate for normal direction: N == V, so NdotH == LdotH.
            // The BRDF pdf can be simplified from:
            //   pdf = D_GGX_Divide_Pi(NdotH, roughness) * NdotH / (4 * LdotH);
            // to:
            //   pdf = D_GGX_Divide_Pi(NdotH, roughness) / 4;

            // Mipmap level is clamped to avoid cubemap filtering issues.

            // OmegaS : solid angle of a sample
            // OmegaP : solid angle of a cubemap pixel
            float NdotH = saturate(dot(N, H));
            float LdotH = saturate(dot(L, H));

            float pdf = Specular_D_GGX(NdotH, roughness * roughness * roughness * roughness) * NdotH / (4 * LdotH);
            float omegaS = 1.0 / (kSampleCount * pdf);
            float omegaP = 4.0 * PI / (6.0 * HDRITextureSize * HDRITextureSize);
            float mipLevel = clamp(0.5 * log2(omegaS / omegaP), 0, HDRIMipCount-1);

            float4 Li = IBLHDRITexture.SampleLevel(cubeMapSampler, L, mipLevel);

            accBrdf += Li.rgb * NdotL;
            accBrdfWeight += NdotL;
        }
    }

    if (accBrdfWeight > 0.0)
    {
        accBrdf = accBrdf / max(1e-6, accBrdfWeight);
    }

    return float4(accBrdf, 1.0);
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint3 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 Pixel = Gid.xy * uint2(8, 8) + GTid;
    if (Pixel.x < SpecularLDMapSize && Pixel.y < SpecularLDMapSize)
    {
        float3 N = ConvertCubePixelToDir(Pixel.x, Pixel.y, Gid.z, SpecularLDMapSize);
        float4 Acc = IntegrateCubeLDOnly(N, N, max(0.04, Roughness));
        IBLSpecularLDMap[uint3(Pixel, Gid.z)] = Acc;
    }
}
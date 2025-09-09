#include "CommonRS.hlsli"
#include "IBLPrecomputeCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

cbuffer CSConstants : register(b0)
{
    uint HDRITextureSize;
    uint HDRIMipCount;
    uint SpecularLDNumMips;
    uint SpecularLDMipLevel;
};

TextureCube<float4> IBLHDRITexture          : register(t0);

RWTexture2DArray<float4> IBLSpecularLDMap : register(u0);

SamplerState cubeMapSampler : register(s0);

static const uint kSampleCount = 1024;

float ComputeRoughnessFromMip(float Mip, float CubemapMaxMip)
{
    float LevelFrom1x1 = CubemapMaxMip - 1.0f - Mip;
    return exp2((1.0f - LevelFrom1x1) / 1.2f);
}

float4 IntegrateCubeLDOnly(in float3 V, in float3 N, in float roughness)
{
    if (roughness < 0.01)
    {
        return float4(IBLHDRITexture.SampleLevel(cubeMapSampler, N, 0).rgb, 1.0f);
    }
    
    float3 accBrdf = 0;
    float  accBrdfWeight = 0;
    
    if (roughness > 0.99)
    {
        // Roughness=1, GGX is constant. Use cosine distribution instead
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
                accBrdfWeight += 1.0f;
            }
        }
    }
    else
    {
        for (uint i = 0; i < kSampleCount; ++i)
        {
            float2 eta = Hammersley(i, kSampleCount);
            //eta.y *= 0.995;
            float3 L;
            float3 H;
            ImportanceSampleGGX(eta, roughness, V, N, H, L);

            float NdotL = saturate(dot(N, L));
            if (NdotL > 0)
            {
                // OmegaS : solid angle of a sample
                // OmegaP : solid angle of a cubemap pixel
                float NdotH = saturate(dot(N, H));
                float LdotH = saturate(dot(L, H));

                // pdf = Specular_D_GGX(NdotH, roughness * roughness * roughness * roughness) * NdotH / (4 * LdotH)
                // but since V = N => VoH == NoH
                float pdf = Specular_D_GGX(NdotH, Pow4(roughness)) * 0.25f;
                float mipLevel = clamp(ComputeMipLevel(kSampleCount, pdf, HDRITextureSize) + 1, 0, HDRIMipCount - 1);
                float4 Li = IBLHDRITexture.SampleLevel(cubeMapSampler, L, mipLevel);
                accBrdf += Li.rgb * NdotL;
                accBrdfWeight += NdotL;
            }
        }
    }

    if (accBrdfWeight > 0.0)
    {
        accBrdf = accBrdf / (accBrdfWeight);
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
    uint SpecularLDMapSize = 1u << (SpecularLDNumMips - SpecularLDMipLevel - 1u);
    if (Pixel.x < SpecularLDMapSize && Pixel.y < SpecularLDMapSize)
    {
        float3 N = ConvertCubePixelToDir(Pixel.x, Pixel.y, Gid.z, SpecularLDMapSize);
        float Roughness = ComputeRoughnessFromMip(SpecularLDMipLevel, SpecularLDNumMips - 1);
        float4 Acc = IntegrateCubeLDOnly(N, N, Roughness);
        IBLSpecularLDMap[uint3(Pixel, Gid.z)] = Acc;
    }
}
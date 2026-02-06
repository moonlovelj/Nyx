#include "CommonRS.hlsli"
#include "IBL.hlsli"
#include "BSDF.hlsli"
#include "MonteCarlo.hlsli"

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

SamplerState CubeMapSampler : register(s0);

static const uint kSampleCount = 1024;

float4 IntegrateCubeLDOnly(in float3 V, in float3 N, in float Roughness)
{
    if (Roughness < 0.01f)
    {
        return float4(IBLHDRITexture.SampleLevel(CubeMapSampler, N, 0).rgb, 1.0f);
    }
    
    float3 AccBrdf = 0;
    float  AccBrdfWeight = 0;
    
    if (Roughness > 0.99f)
    {
        // Roughness=1, GGX is constant. Use cosine distribution instead
        for (uint i = 0; i < kSampleCount; ++i)
        {
            float2 eta = Hammersley(i, kSampleCount);
            float3 L;
            float NdotL;
            float PDF;

            ImportanceSampleCosDir(eta, N, L, NdotL, PDF);
            if (NdotL > 0)
            {
                float mipLevel = clamp(ComputeMipLevel(kSampleCount, PDF, HDRITextureSize) + 1, 0, HDRIMipCount - 1);
                AccBrdf += IBLHDRITexture.SampleLevel(CubeMapSampler, L, mipLevel).rgb;
                AccBrdfWeight += 1.0f;
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
            ImportanceSampleGGX(eta, Roughness, V, N, H, L);

            float NdotL = saturate(dot(N, L));
            if (NdotL > 0.f)
            {
                // OmegaS : solid angle of a sample
                // OmegaP : solid angle of a cubemap pixel
                float NdotH = saturate(dot(N, H));
                float LdotH = saturate(dot(L, H));

                // PDF = Specular_D_GGX(NdotH, roughness * roughness * roughness * roughness) * NdotH / (4 * LdotH)
                // but since V = N => VoH == NoH
                float PDF = Specular_D_GGX(Pow4(Roughness), NdotH) * 0.25f;
                float mipLevel = clamp(ComputeMipLevel(kSampleCount, PDF, HDRITextureSize) + 1, 0, HDRIMipCount - 1);
                float4 Li = IBLHDRITexture.SampleLevel(CubeMapSampler, L, mipLevel);
                AccBrdf += Li.rgb * NdotL;
                AccBrdfWeight += NdotL;
            }
        }
    }

    if (AccBrdfWeight > 0.0f)
    {
        AccBrdf = AccBrdf / (AccBrdfWeight);
    }

    return float4(AccBrdf, 1.0f);
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
        float Roughness = ComputeIBLRoughnessFromMip(SpecularLDMipLevel, SpecularLDNumMips - 1);
        float4 Acc = IntegrateCubeLDOnly(N, N, Roughness);
        IBLSpecularLDMap[uint3(Pixel, Gid.z)] = Acc;
    }
}

#include "CommonRS.hlsli"
#include "IBLPrecomputeCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

cbuffer CSConstants : register(b0)
{
    uint LutSize;
};

RWTexture2D<float4> IBLLutTexture : register(u0);

static const uint kSampleCount = 1024;

float G_SmithGGXCorrelated(float Roughness, float NdotL, float NdotV)
{
    float alphaSqr = Roughness * Roughness * Roughness * Roughness;
    float NdotL2 = NdotL * NdotL;
    float NdotV2 = NdotV * NdotV;
    float lambda_l = (-1 + sqrt(alphaSqr * (1 - NdotL2) / max(1e-6, NdotL2) + 1)) * 0.5f;
    float lambda_v = (-1 + sqrt(alphaSqr * (1 - NdotV2) / max(1e-6, NdotV2) + 1)) * 0.5f;
    return  1.0 / max(1.0 + lambda_v + lambda_l, 1e-6);
}

float4 IntegrateDFGOnly(in float3 V, in float3 N, in float roughness)
{
    float NdotV = saturate(dot(N, V));
    float4 acc = 0;
    float accWeight = 0;

    // Compute pre-integration
    //Referential referential = createReferential(N);
    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 u = Hammersley(i, kSampleCount);
        float3 L = 0;
        float3 H = 0;

        // See [Karis13] for implementation
        //importanceSampleGGX_G(u, V, N, referential, roughness, NdotH, LdotH, L, G);

        ImportanceSampleGGX(u, roughness, V, N, H, L);
        float NdotH = dot(N, H);
        float  LdotH = dot(H, L);
        float NdotL = saturate(dot(N, L));
        float G = G_SmithGGXCorrelated(roughness, NdotL, NdotV);

        // Specular GGX DFG preIntegration
        
        if (NdotL > 0 && G > 0.0)
        {
            float GVis = G * LdotH / max(1e-6, NdotH * NdotV);
            float Fc = pow(1 - LdotH, 5.0f);
            acc.x += (1 - Fc) * GVis;
            acc.y += Fc * GVis;
        }

        //// Diffuse Disney preIntegration
        //u = frac(u + 0.5);
        //float pdf;
        //// The pdf is not used because it cancels with other terms
        //// (The 1/PI from diffuse BRDF and the NdotL from Lambert¡¯s law).
        //ImportanceSampleCosDir(u, N, L, NdotL, pdf);
        //if (NdotL > 0)
        //{
        //    float LdotH = saturate(dot(L, normalize(V + L)));
        //    float NdotV = saturate(dot(N, V));
            
        //    acc.z += Diffuse_Burley(roughness, NdotL, NdotV, LdotH);
        //}

        //accWeight += 1.0;
    }

    return acc * (1.0f / kSampleCount);
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint2 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 Pixel = Gid.xy * uint2(8, 8) + GTid;
    if (Pixel.x < LutSize && Pixel.y < LutSize)
    {
        float NdotV = (Pixel.x + 0.5) / (float)LutSize;
        float Roughness = (Pixel.y + 0.5) / (float)LutSize;

        float3 N = float3(0, 0, 1);
        float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
        float4 Acc = IntegrateDFGOnly(V, N, Roughness);
        IBLLutTexture[Pixel] = Acc;
    }
}
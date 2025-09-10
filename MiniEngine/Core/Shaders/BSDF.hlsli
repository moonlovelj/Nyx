#ifndef __BSDF_HLSLI__
#define __BSDF_HLSLI__

#include "Math.hlsli"

// Shlick's approximation of Fresnel
float3 Fresnel_Shlick(float3 F0, float3 F90, float Cosine)
{
    return lerp(F0, F90, Pow5(1.0 - Cosine));
}

float Fresnel_Shlick(float F0, float F90, float Cosine)
{
    return lerp(F0, F90, Pow5(1.0 - Cosine));
}

// Burley's diffuse BRDF
float3 Diffuse_Burley(float3 DiffuseColor, float Roughness, float NdotV, float NdotL, float LdotH)
{
    float FD90 = 0.5 + 2.0 * Roughness * LdotH * LdotH;
    return DiffuseColor * INV_PI * Fresnel_Shlick(1.0f, FD90, NdotL).x * Fresnel_Shlick(1.0f, FD90, NdotV).x;
}

// GGX specular D (normal distribution)
float Specular_D_GGX(float AlphaSqr, float NdotH)
{
    float Lower = lerp(1, AlphaSqr, NdotH * NdotH);
    return AlphaSqr / (PI * Lower * Lower);
}

float G_SmithGGXCorrelated(float AlphaSqr, float NdotV, float NdotL)
{
    float NdotL2 = NdotL * NdotL;
    float NdotV2 = NdotV * NdotV;
    float Lambda_L = (-1 + sqrt(AlphaSqr * (1 - NdotL2) / NdotL2 + 1)) * 0.5f;
    float Lambda_V = (-1 + sqrt(AlphaSqr * (1 - NdotV2) / NdotV2 + 1)) * 0.5f;
    return  1.0 / (1.0 + Lambda_V + Lambda_L);
}

float V_SmithGGXCorrelated(float AlphaSqr, float NdotV, float NdotL)
{
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - AlphaSqr) + AlphaSqr);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - AlphaSqr) + AlphaSqr);
    return 0.5 / max(FLT_MIN, GGXV + GGXL);
}

// A microfacet based BRDF.
// alpha:    This is roughness squared as in the Disney PBR model by Burley et al.
// c_spec:   The F0 reflectance value - 0.04 for non-metals, or RGB for metals.  This is the specular albedo.
// NdotV, NdotL, LdotH, NdotH:  vector dot products
//  N - surface normal
//  V - normalized view vector
//  L - normalized direction to light
//  H - normalized half vector (L+V)/2 -- halfway between L and V
float3 Specular_BRDF(float3 SpecularColor, float AlphaSqr, float NdotV, float NdotL, float NdotH, float LdotH)
{
    // Normal Distribution term
    float ND = Specular_D_GGX(AlphaSqr, NdotH);

    // Geometric Visibility term
    //float GV = G_Schlick_Smith(Surface, Light);
    // float GV = G_Shlick_Smith_Hable(Surface, Light);
    float GV = V_SmithGGXCorrelated(AlphaSqr, NdotV, NdotL);

    // Fresnel term
    float3 F = Fresnel_Shlick(SpecularColor, 1.0f, LdotH);

    return ND * GV * F;
}

#endif
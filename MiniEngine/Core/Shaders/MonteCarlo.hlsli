#ifndef __MONTE_CARLO_HLSLI__
#define __MONTE_CARLO_HLSLI__

#include "Math.hlsli"

// i是样本索引 (0, 1, 2, ...), N是总样本数
// 返回一个在[0,1]^2空间中均匀分布的点
float2 Hammersley(uint i, uint N)
{
    // 第一维是均匀分布
    float dim1 = float(i) / float(N);

    // 第二维是Radical Inverse, base 2
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float dim2 = float(bits) * 2.3283064365386963e-10; // / 0x100000000

    return float2(dim1, dim2);
}

// [ Duff et al. 2017, "Building an Orthonormal Basis, Revisited" ]
// Discontinuity at TangentZ.z == 0
float3x3 GetTangentBasis(float3 TangentZ)
{
    const float Sign = TangentZ.z >= 0 ? 1 : -1;
    const float a = -rcp(Sign + TangentZ.z);
    const float b = TangentZ.x * TangentZ.y * a;

    float3 TangentX = { 1 + Sign * a * Pow2(TangentZ.x), Sign * b, -Sign * TangentZ.x };
    float3 TangentY = { b, Sign + a * Pow2(TangentZ.y), -TangentZ.y };

    return float3x3(TangentX, TangentY, TangentZ);
}

float3 TangentToWorld(float3 Vec, float3 TangentZ)
{
    return mul(Vec, GetTangentBasis(TangentZ));
}

float3 WorldToTangent(float3 Vec, float3 TangentZ)
{
    return mul(GetTangentBasis(TangentZ), Vec);
}

void ImportanceSampleGGX(float2 Xi, float Roughness, float3 V, float3 N, out float3 H, out float3 L)
{
    float a = Roughness * Roughness;
    float Phi = 2 * PI * Xi.x;
    float CosTheta = saturate(sqrt((1 - Xi.y) / (1 + (a * a - 1) * Xi.y)));
    float SinTheta = saturate(sqrt(1 - CosTheta * CosTheta));

    H.x = SinTheta * cos(Phi);
    H.y = SinTheta * sin(Phi);
    H.z = CosTheta;

    H = normalize(TangentToWorld(H, N));
    L = normalize(2 * dot(V, H) * H - V);
}

void ImportanceSampleCosDir(
    in float2 u,
    in float3 N,
    out float3 L,
    out float NdotL,
    out float PDF)
{
    float u1 = u.x;
    float u2 = u.y;

    float r = sqrt(u1);
    float phi = u2 * PI * 2;

    L = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0f - u1)));
    L = normalize(TangentToWorld(L, N));

    NdotL = dot(L, N);
    PDF = NdotL * INV_PI;
}

#endif
#ifndef __IBLPRECOMPUTECOMMON_HLSLI__
#define __IBLPRECOMPUTECOMMON_HLSLI__

#define PI				3.1415926535f
#define INV_PI          0.31830988618f
#define TWOPI			6.283185307f

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

float Pow5(float x)
{
    float xSq = x * x;
    return xSq * xSq * x;
}

float Pow4(float x)
{
    return x * x * x * x;
}

float Pow2(float x)
{
    return x * x;
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

void ImportanceSampleGGX(float2 Xi, float Roughness, float3 V, float3 N, out float3 H, out float3 L)
{
	float a = Roughness * Roughness;
	float Phi = 2 * PI * Xi.x;
	float CosTheta = saturate(sqrt((1 - Xi.y) / (1 + (a*a - 1) * Xi.y )));
	float SinTheta = saturate(sqrt(1 - CosTheta * CosTheta ));

	H.x = SinTheta * cos( Phi );
	H.y = SinTheta * sin( Phi );
	H.z = CosTheta;

    H = normalize(TangentToWorld(H, N));
    L = normalize(2 * dot(V, H) * H - V);
}

void ImportanceSampleCosDir(
    in float2 u,
    in float3 N,
    out float3 L,
    out float NdotL,
    out float pdf)
{
    float u1 = u.x;
    float u2 = u.y;

    float r = sqrt(u1);
    float phi = u2 * PI * 2;

    L = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0f - u1)));
    L = normalize(TangentToWorld(L, N));

    NdotL = dot(L, N);
    pdf = NdotL * INV_PI;
}

float Fresnel_Shlick(float F0, float F90, float cosine)
{
    return lerp(F0, F90, Pow5(1.0 - cosine));
}

// Burley's diffuse BRDF
float Diffuse_Burley(float roughness, float NdotL, float NdotV, float LdotH)
{
    float fd90 = 0.5 + 2.0 * roughness * LdotH * LdotH;
    return Fresnel_Shlick(1, fd90, NdotL).x * Fresnel_Shlick(1, fd90, NdotV).x;
}

float Specular_D_GGX(float NdotH, float alphaSqr)
{
    float lower = lerp(1, alphaSqr, NdotH * NdotH);
    return alphaSqr / (PI * lower * lower);
}

float3 ConvertCubePixelToDir(uint x, uint y, uint face, uint TextureSize)
{
    // 输入: x, y 像素坐标，width, height 每个面尺寸， d 面索引[0..5]
    // 输出: 方向向量 dir (float3)

    float u = ((float)x + 0.5) / TextureSize; // 0~1
    float v = ((float)y + 0.5) / TextureSize; // 0~1

    // 转到 [-1,1] 坐标，中心对齐
    float fx = 2.0 * u - 1.0;
    float fy = 2.0 * v - 1.0;

    float3 dir;
    switch (face)
    {
    case 0:
        dir = float3(1, -fy, -fx);
        break; // +X
    case 1:
        dir = float3(-1, -fy, fx);
        break; // -X
    case 2:
        dir = float3(fx, 1, fy);
        break; // +Y
    case 3:
        dir = float3(fx, -1, -fy);
        break; // -Y
    case 4:
        dir = float3(fx, -fy, 1);
        break; // +Z
    case 5:
        dir = float3(-fx, -fy, -1);
        break; // -Z
    }
    dir = normalize(dir);

    return dir;
}

float ComputeMipLevel(float sampleCount, float pdf, float hdriTextureSize)
{
    float omegaS = 1.0 / (sampleCount * pdf);
    float omegaP = 4.0 * PI / (6.0 * hdriTextureSize * hdriTextureSize);
    float mipLevel = 0.5 * log2(omegaS / omegaP);
    return mipLevel;
}

#endif
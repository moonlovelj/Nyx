#ifndef __LIGHTING_COMMON_HLSLI__
#define __LIGHTING_COMMON_HLSLI__

#include "Common.hlsli"
#include "LightGrid.hlsli"

#define FLT_MIN         1.175494351e-38F        // min positive value
#define FLT_MAX         3.402823466e+38F        // max value
#define PI				3.1415926535f
#define INV_PI          0.31830988618f
#define TWOPI			6.283185307f

// Numeric constants
static const float3 kDielectricSpecular = float3(0.04, 0.04, 0.04);


cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;
    float4x4 InverseViewProjMatrix;
    float4x4 SunShadowMatrix;
    float3 ViewerPos;
    float3 SunDirection;
    float3 SunColor;
    float4 ShadowTexelSize;
    
    float4 InvTileDim;
    uint4 TileCount;
    uint4 FirstLightIndex;

    uint ViewportWidth;
    uint ViewportHeight;

    uint FrameIndexMod2;

    uint IBLLutTextureSize;
    uint IBLSpecularLDMapMipCount;

};

// Common textures
TextureCube<float3> IBLDiffuseLDMap      : register(t10);
TextureCube<float3> IBLSpecularLDMap    : register(t11);
Texture2D<float4> IBLLut : register(t12);
Texture2D<float> texSSAO : register(t13);
Texture2D<float> texShadow : register(t14);
StructuredBuffer<LightData> lightBuffer : register(t15);
Texture2DArray<float> lightShadowArrayTex : register(t16);
ByteAddressBuffer lightGrid : register(t17);
ByteAddressBuffer lightGridBitMask : register(t18);


struct SurfaceProperties
{
    float3 N;
    float3 V;
    float3 c_diff;
    float3 c_spec;
    float roughness;
    float alpha; // roughness squared
    float alphaSqr; // alpha squared
    float NdotV;
};

struct LightProperties
{
    float3 L;
    float NdotL;
    float LdotH;
    float NdotH;
};

//
// Shader Math
//

float Pow5(float x)
{
    float xSq = x * x;
    return xSq * xSq * x;
}

// Shlick's approximation of Fresnel
float3 Fresnel_Shlick(float3 F0, float3 F90, float cosine)
{
    return lerp(F0, F90, Pow5(1.0 - cosine));
}

float Fresnel_Shlick(float F0, float F90, float cosine)
{
    return lerp(F0, F90, Pow5(1.0 - cosine));
}

// Burley's diffuse BRDF
float3 Diffuse_Burley(SurfaceProperties Surface, LightProperties Light)
{
    float fd90 = 0.5 + 2.0 * Surface.roughness * Light.LdotH * Light.LdotH;
    return Surface.c_diff * INV_PI * Fresnel_Shlick(1, fd90, Light.NdotL).x * Fresnel_Shlick(1, fd90, Surface.NdotV).x;
}

// GGX specular D (normal distribution)
float Specular_D_GGX(SurfaceProperties Surface, LightProperties Light)
{
    float lower = lerp(1, Surface.alphaSqr, Light.NdotH * Light.NdotH);
    return Surface.alphaSqr / max(1e-6, PI * lower * lower);
}

// Schlick-Smith specular geometric visibility function
float G_Schlick_Smith(SurfaceProperties Surface, LightProperties Light)
{
    return 1.0 / max(1e-6, lerp(Surface.NdotV, 1, Surface.alpha * 0.5) * lerp(Light.NdotL, 1, Surface.alpha * 0.5));
}

// Schlick-Smith specular visibility with Hable's LdotH approximation
float G_Shlick_Smith_Hable(SurfaceProperties Surface, LightProperties Light)
{
    return 1.0 / lerp(Light.LdotH * Light.LdotH, 1, Surface.alphaSqr * 0.25);
}

float G_SmithGGXCorrelated(SurfaceProperties Surface, LightProperties Light)
{
    float NdotL2 = Light.NdotL * Light.NdotL;
    float NdotV2 = Surface.NdotV * Surface.NdotV;
    float lambda_l = (-1 + sqrt(Surface.alphaSqr * (1 - NdotL2) / max(1e-6, NdotL2) + 1)) * 0.5f;
    float lambda_v = ( -1 + sqrt (Surface.alphaSqr * (1 - NdotV2 ) / max(1e-6, NdotV2) + 1) ) * 0.5f;
	return  1.0 / max(1.0 + lambda_v + lambda_l, 1e-6);
}

float V_SmithGGXCorrelated(SurfaceProperties Surface, LightProperties Light)
{
    float GGXV = Light.NdotL * sqrt(Surface.NdotV * Surface.NdotV * (1.0 - Surface.alphaSqr) + Surface.alphaSqr);
    float GGXL = Surface.NdotV * sqrt(Light.NdotL * Light.NdotL * (1.0 - Surface.alphaSqr) + Surface.alphaSqr);
    return 0.5 / max(1e-6, (GGXV + GGXL));
}


// A microfacet based BRDF.
// alpha:    This is roughness squared as in the Disney PBR model by Burley et al.
// c_spec:   The F0 reflectance value - 0.04 for non-metals, or RGB for metals.  This is the specular albedo.
// NdotV, NdotL, LdotH, NdotH:  vector dot products
//  N - surface normal
//  V - normalized view vector
//  L - normalized direction to light
//  H - normalized half vector (L+V)/2 -- halfway between L and V
float3 Specular_BRDF(SurfaceProperties Surface, LightProperties Light)
{
    // Normal Distribution term
    float ND = Specular_D_GGX(Surface, Light);

    // Geometric Visibility term
    //float GV = G_Schlick_Smith(Surface, Light);
    // float GV = G_Shlick_Smith_Hable(Surface, Light);
    float GV = V_SmithGGXCorrelated(Surface, Light);

    // Fresnel term
    float3 F = Fresnel_Shlick(Surface.c_spec, 1.0, Light.LdotH);

    return ND * GV * F;
}

float3 ShadeDirectionalLight(SurfaceProperties Surface, float3 L, float3 c_light)
{
    LightProperties Light;
    Light.L = L;

    // Half vector
    float3 H = normalize(L + Surface.V);

    // Pre-compute dot products
    Light.NdotL = saturate(dot(Surface.N, L));
    Light.LdotH = saturate(dot(L, H));
    Light.NdotH = saturate(dot(Surface.N, H));

    // Diffuse & specular factors
    float3 diffuse = Diffuse_Burley(Surface, Light);
    float3 specular = Specular_BRDF(Surface, Light);

    // Directional light
    return Light.NdotL * c_light * (diffuse + specular);
}

// diffuse retro-reflection Disney lobe
float3 getDiffuseDominantDir(float3 N, float3 V, float NdotV, float roughness)
{
    // 计算两个系数 a 和 b。
    // 这两个看起来很随意的“魔法数字”（1.02341, -1.51174, ...）
    // 表明这是一个经验公式。它们很可能是通过离线拟合一个更精确的物理模型
    // （比如Oren-Nayar）的结果而得到的。
    // 目标是用最简单的线性函数来近似一个复杂的物理行为。
    float a = 1.02341f * roughness - 1.51174f;
    float b = -0.511705f * roughness + 0.755868f;

    // 计算核心的插值因子 lerpFactor
    // - (NdotV * a + b): 这是一个依赖于视角和粗糙度的基础因子。
    // - * roughness: 再次乘以粗糙度，确保当 roughness 为 0 时，整个 lerpFactor 为 0。
    // - saturate(...): 保证因子在 [0, 1] 范围内，这是lerp函数所必需的。
    float lerpFactor = saturate((NdotV * a + b) * roughness);

    //【关键步骤】返回 N 和 V 之间的线性插值结果。
    // 这个结果就是我们最终用来采样辐照度图的“主导漫反射方向”。
    return lerp(N, V, lerpFactor);
}

float3 EvaluateIBLDiffuse(SurfaceProperties Surface)
{
    float3 dominantN = getDiffuseDominantDir(Surface.N, Surface.V, Surface.NdotV, Surface.roughness);
    float3 diffuseLighting = IBLDiffuseLDMap.SampleLevel(cubeMapSampler, dominantN, 0);

    float diffF = IBLLut.SampleLevel(linearSampler, float2(Surface.NdotV, Surface.roughness), 0).z;

    return Surface.c_diff * diffuseLighting * diffF;
}

// We have a better approximation of the off-specular peak,
// but due to other approximations we found this one performs better.
// N is the normal direction
// R is the mirror vector
// This approximation works fine for G Smith correlated and uncorrelated
float3 getSpecularDominantDir(float3 N, float3 R, float roughness)
{
    float smoothness = saturate(1 - roughness);
    float lerpFactor = smoothness * (sqrt(smoothness) + roughness);

    // The result is not normalized as we fetch in a cubemap
    return lerp(N, R, lerpFactor);
}

float3 EvaluateIBLSpecular(SurfaceProperties Surface)
{
    float3 R = reflect(-Surface.V, Surface.N);

    float3 dominantR = getSpecularDominantDir(Surface.N, R, Surface.roughness);

    // Rebuild the function
    // L · D · (f0 · Gv · (1 - Fc) + Gv · Fc) · cosTheta / (4 · NdotL · NdotV)
    float NdotV = max(Surface.NdotV, 0.5f / IBLLutTextureSize);

    float mipLevel = Surface.roughness * (IBLSpecularLDMapMipCount-1.0);
    float3 preLD = IBLSpecularLDMap.SampleLevel(cubeMapSampler, dominantR, mipLevel).rgb;

    // Sample pre-integrated DFG
    // Fc = (1 - H · L)^5
    // PreIntegratedDFG.r = Gv · (1 - Fc)
    // PreIntegratedDFG.g = Gv · Fc
    float2 preDFG = IBLLut.SampleLevel(linearSampler, float2(NdotV, Surface.roughness), 0).xy;

    //return preLD;
    // LD · (f0 · Gv · (1 - Fc) + Gv · Fc · f90)
    return preLD * (Surface.c_spec * preDFG.x + 1 * preDFG.y);
}



float GetDirectionalShadow( float3 ShadowCoord, Texture2D<float> texShadow )
{
#ifdef SINGLE_SAMPLE
    float result = texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy, ShadowCoord.z );
#else
    const float Dilation = 2.0;
    float d1 = Dilation * ShadowTexelSize.x * 0.125;
    float d2 = Dilation * ShadowTexelSize.x * 0.875;
    float d3 = Dilation * ShadowTexelSize.x * 0.625;
    float d4 = Dilation * ShadowTexelSize.x * 0.375;
    float result = (
        2.0 * texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy, ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d2,  d1), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d1, -d2), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d2, -d1), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d1,  d2), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d4,  d3), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d3, -d4), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d4, -d3), ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d3,  d4), ShadowCoord.z )
        ) / 10.0;
#endif
    return result * result;
}

float GetShadowConeLight(uint lightIndex, float3 shadowCoord)
{
    float result = lightShadowArrayTex.SampleCmpLevelZero(
        shadowSampler, float3(shadowCoord.xy, lightIndex), shadowCoord.z);
    return result * result;
}

float3 ApplyLightCommon(
    SurfaceProperties Surface, 
    float3	L,		// World-space vector from point to light
    float3	c_light		// Radiance of directional light
    )
{
    LightProperties Light;
    Light.L = L;

    // Half vector
    float3 H = normalize(L + Surface.V);

    // Pre-compute dot products
    Light.NdotL = saturate(dot(Surface.N, L));
    Light.LdotH = saturate(dot(L, H));
    Light.NdotH = saturate(dot(Surface.N, H));

    // Diffuse & specular factors
    float3 diffuse = Diffuse_Burley(Surface, Light);
    float3 specular = Specular_BRDF(Surface, Light);

    return Light.NdotL * c_light * (diffuse + specular);
}

// float3 ApplyDirectionalLight(
//     float3	diffuseColor,	// Diffuse albedo
//     float3	specularColor,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float	gloss,			// Specular power
//     float3	normal,			// World-space normal
//     float3	viewDir,		// World-space vector from eye to point
//     float3	lightDir,		// World-space vector from point to light
//     float3	lightColor,		// Radiance of directional light
//     float3	shadowCoord,	// Shadow coordinate (Shadow map UV & light-relative Z)
// 	Texture2D<float> ShadowMap
//     )
// {
//     float shadow = GetDirectionalShadow(shadowCoord, ShadowMap);
//
//     return shadow * ApplyLightCommon(
//         diffuseColor,
//         specularColor,
//         specularMask,
//         gloss,
//         normal,
//         viewDir,
//         lightDir,
//         lightColor
//         );
// }

float3 ApplyPointLight(
    SurfaceProperties Surface,
    float3	worldPos,		// World-space fragment position
    float3	lightPos,		// World-space light position
    float	lightRadiusSq,
    float3	lightColor		// Radiance of directional light
    )
{
    float3 lightDir = lightPos - worldPos;
    float lightDistSq = dot(lightDir, lightDir);
    float invLightDist = rsqrt(lightDistSq);
    lightDir *= invLightDist;

    // modify 1/d^2 * R^2 to fall off at a fixed radius
    // (R/d)^2 - d/R = [(1/d^2) - (1/R^2)*(d/R)] * R^2
    float distanceFalloff = lightRadiusSq * (invLightDist * invLightDist);
    distanceFalloff = max(0, distanceFalloff - rsqrt(distanceFalloff));

    return distanceFalloff * ApplyLightCommon(
        Surface,
        lightDir,
        lightColor
        );
}

float3 ApplyConeLight(
    SurfaceProperties Surface,
    float3	worldPos,		// World-space fragment position
    float3	lightPos,		// World-space light position
    float	lightRadiusSq,
    float3	lightColor,		// Radiance of directional light
    float3	coneDir,
    float2	coneAngles
    )
{
    float3 lightDir = lightPos - worldPos;
    float lightDistSq = dot(lightDir, lightDir);
    float invLightDist = rsqrt(lightDistSq);
    lightDir *= invLightDist;

    // modify 1/d^2 * R^2 to fall off at a fixed radius
    // (R/d)^2 - d/R = [(1/d^2) - (1/R^2)*(d/R)] * R^2
    float distanceFalloff = lightRadiusSq * (invLightDist * invLightDist);
    distanceFalloff = max(0, distanceFalloff - rsqrt(distanceFalloff));

    float coneFalloff = dot(-lightDir, coneDir);
    coneFalloff = saturate((coneFalloff - coneAngles.y) * coneAngles.x);

    return (coneFalloff * distanceFalloff) * ApplyLightCommon(
        Surface,
        lightDir,
        lightColor
        );
}

float3 ApplyConeShadowedLight(
    SurfaceProperties Surface,
    float3	worldPos,		// World-space fragment position
    float3	lightPos,		// World-space light position
    float	lightRadiusSq,
    float3	lightColor,		// Radiance of directional light
    float3	coneDir,
    float2	coneAngles,
    float4x4 shadowTextureMatrix,
    uint	lightIndex
    )
{
    float4 shadowCoord = mul(shadowTextureMatrix, float4(worldPos, 1.0));
    shadowCoord.xyz *= rcp(shadowCoord.w);
    float shadow = GetShadowConeLight(lightIndex, shadowCoord.xyz);

    return shadow * ApplyConeLight(
        Surface,
        worldPos,
        lightPos,
        lightRadiusSq,
        lightColor,
        coneDir,
        coneAngles
        );
}

void ShadeLights(inout float3 colorSum, 
    SurfaceProperties Surface,
    uint2 pixelPos,
    float3 worldPos
    )
{
    uint2 tilePos = GetTilePos(pixelPos, InvTileDim.xy);
    uint tileIndex = GetTileIndex(tilePos, TileCount.x);
    uint tileOffset = GetTileOffset(tileIndex);
    uint tileLightCount = lightGrid.Load(tileOffset + 0);
    uint tileLightCountSphere = (tileLightCount >> 0) & 0xff;
    uint tileLightCountCone = (tileLightCount >> 8) & 0xff;
    uint tileLightCountConeShadowed = (tileLightCount >> 16) & 0xff;

    uint tileLightLoadOffset = tileOffset + 4;

#define POINT_LIGHT_ARGS \
    Surface, \
    worldPos, \
    lightData.pos, \
    lightData.radiusSq, \
    lightData.color

#define CONE_LIGHT_ARGS \
    POINT_LIGHT_ARGS, \
    lightData.coneDir, \
    lightData.coneAngles

#define SHADOWED_LIGHT_ARGS \
    CONE_LIGHT_ARGS, \
    lightData.shadowTextureMatrix, \
    lightIndex
        
    // sphere
    uint n;
    for (n = 0; n < tileLightCountSphere; n++, tileLightLoadOffset += 4)
    {
        uint lightIndex = lightGrid.Load(tileLightLoadOffset);
        LightData lightData = lightBuffer[lightIndex];
        colorSum += ApplyPointLight(POINT_LIGHT_ARGS);
    }

    // cone
    for (n = 0; n < tileLightCountCone; n++, tileLightLoadOffset += 4)
    {
        uint lightIndex = lightGrid.Load(tileLightLoadOffset);
        LightData lightData = lightBuffer[lightIndex];
        colorSum += ApplyConeLight(CONE_LIGHT_ARGS);
    }

    // cone w/ shadow map
    for (n = 0; n < tileLightCountConeShadowed; n++, tileLightLoadOffset += 4)
    {
        uint lightIndex = lightGrid.Load(tileLightLoadOffset);
        LightData lightData = lightBuffer[lightIndex];
        colorSum += ApplyConeShadowedLight(SHADOWED_LIGHT_ARGS);
    }
}

#endif

#ifndef __LIGHTING_COMMON_HLSLI__
#define __LIGHTING_COMMON_HLSLI__

#include "Common.hlsli"
#include "../../Core/Shaders/Math.hlsli"
#include "../../Core/Shaders/BSDF.hlsli"
#include "../../Core/Shaders/IBL.hlsli"
#include "LightGrid.hlsli"
#include "PCSS.hlsli"
#include "CommonResources.hlsli"

#define PCSS_SHADOW     1

static const float3 kDielectricSpecular = float3(0.04, 0.04, 0.04);

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
    float3 diffuse = Diffuse_Burley(Surface.c_diff, Surface.roughness, Surface.NdotV, Light.NdotL, Light.LdotH);
    float3 specular = Specular_BRDF(Surface.c_spec, Surface.alphaSqr, Surface.NdotV, Light.NdotL, Light.NdotH, Light.LdotH);

    // Directional light
    return Light.NdotL * c_light * (diffuse + specular);
}

// diffuse retro-reflection Disney lobe
float3 GetDiffuseDominantDir(float3 N, float3 V, float NdotV, float Roughness)
{
    float a = 1.02341f * Roughness - 1.51174f;
    float b = -0.511705f * Roughness + 0.755868f;
    float LerpFactor = saturate((NdotV * a + b) * Roughness);
    return lerp(N, V, LerpFactor);
}

float3 EvaluateIBLDiffuse(SurfaceProperties Surface)
{
	Texture2D<float4> IBLLut = GetIBLLutSRV();
	TextureCube<float3> IBLDiffuseLDMap = GetIBLDiffuseLDSRV();
    float3 DominantN = GetDiffuseDominantDir(Surface.N, Surface.V, Surface.NdotV, Surface.roughness);
    float3 DiffuseLighting = IBLDiffuseLDMap.SampleLevel(cubeMapSampler, DominantN, 0);
    float DiffF = IBLLut.SampleLevel(linearSampler, float2(Surface.NdotV, Surface.roughness), 0).z;
    return Surface.c_diff * DiffuseLighting * DiffF;

    // Lambertian diffuse
    //float3 diffuseLighting = IBLDiffuseLDMap.SampleLevel(cubeMapSampler, Surface.N, 0);
    //return Surface.c_diff * diffuseLighting; // PI和cos重要性采样抵消了
    //return Surface.c_diff * INV_PI * diffuseLighting;
}

float3 EvaluateIBLSpecular(SurfaceProperties Surface)
{
    TextureCube<float3> IBLSpecularLDMap = GetIBLSpecularLDSRV();
    Texture2D<float4> IBLLut = GetIBLLutSRV();
    float3 R = reflect(-Surface.V, Surface.N);
    R = GetOffSpecularPeakReflectionDir(Surface.N, R, Surface.roughness);

    // Rebuild the function
    // L · D · (f0 · Gv · (1 - Fc) + Gv · Fc) · cosTheta / (4 · NdotL · NdotV)
    float MipLevel = ComputeIBLMipFromRoughness(Surface.roughness, IBLSpecularLDMapMipCount - 1.0);
    float3 PreLD = IBLSpecularLDMap.SampleLevel(cubeMapSampler, R, MipLevel).rgb;

    // Sample pre-integrated DFG
    // Fc = (1 - H · L)^5
    // PreIntegratedDFG.r = Gv · (1 - Fc)
    // PreIntegratedDFG.g = Gv · Fc
    float2 PreDFG = IBLLut.SampleLevel(linearSampler, float2(Surface.NdotV, Surface.roughness), 0).xy;

    // LD · (f0 · Gv · (1 - Fc) + Gv · Fc · f90)
    return PreLD * (Surface.c_spec * PreDFG.x + saturate(50.0f * Surface.c_spec.g) * PreDFG.y);
}

float GetDirectionalShadow(float2 ScreenUV, float3 ShadowCoord)
{
    Texture2D<float> texShadow = GetShadowMapSRV();
#ifdef SINGLE_SAMPLE
    float result = texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy, ShadowCoord.z );
#elif PCSS_SHADOW
    float result =  PCSS(texShadow, shadowSampler, pointSampler, ScreenUV, ShadowCoord, ShadowTexelSize);
    return result;
#else
    const float Dilation = 2.0;
    float d1 = Dilation * ShadowTexelSize.x * 0.125;
    float d2 = Dilation * ShadowTexelSize.x * 0.875;
    float d3 = Dilation * ShadowTexelSize.x * 0.625;
    float d4 = Dilation * ShadowTexelSize.x * 0.375;
    float shadowBias = 1e-3;
    float result = (
        2.0 * texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy, ShadowCoord.z ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d2,  d1), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d1, -d2), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d2, -d1), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d1,  d2), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d4,  d3), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2(-d3, -d4), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d4, -d3), ShadowCoord.z + shadowBias ) +
        texShadow.SampleCmpLevelZero( shadowSampler, ShadowCoord.xy + float2( d3,  d4), ShadowCoord.z + shadowBias )
        ) / 10.0;
#endif
    return result * result;
}

float GetShadowConeLight(uint lightIndex, float3 shadowCoord)
{
    Texture2DArray<float> lightShadowArrayTex = GetLightShadowArraySRV();
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
    float3 diffuse = Diffuse_Burley(Surface.c_diff, Surface.roughness, Surface.NdotV, Light.NdotL, Light.LdotH);
    float3 specular = Specular_BRDF(Surface.c_spec, Surface.alphaSqr, Surface.NdotV, Light.NdotL, Light.NdotH, Light.LdotH);

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
    ByteAddressBuffer lightGrid = GetLightGridSRV();
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
        
    StructuredBuffer<LightData> lightBuffer = GetLightBufferSRV();
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

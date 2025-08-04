
#include "LightGrid.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

#define FLT_MIN         1.175494351e-38F        // min positive value
#define FLT_MAX         3.402823466e+38F        // max value
#define PI				3.1415926535f
#define TWOPI			6.283185307f

// Numeric constants
static const float3 kDielectricSpecular = float3(0.04, 0.04, 0.04);


cbuffer CSConstants : register(b0)
{
    float3 SunDirection;
    float3 SunColor;
    float4 ShadowTexelSize;
    float4x4 SunShadowMatrix;

    float3 ViewerPos;
    
    float4 InvTileDim;
    uint4 TileCount;
    uint4 FirstLightIndex;

    uint ViewportWidth;
    uint ViewportHeight;

    uint FrameIndexMod2;
};

Texture2D<float4> gBufferA : register(t0);
Texture2D<float4> gBufferB : register(t1);
Texture2D<float4> gBufferC : register(t2);
Texture2D<float4> gBufferD : register(t3);

Texture2D<float> texSSAO : register(t4);
Texture2D<float> texShadow : register(t5);

StructuredBuffer<LightData> lightBuffer : register(t6);
ByteAddressBuffer lightGrid : register(t7);
ByteAddressBuffer lightGridBitMask : register(t8);
Texture2DArray<float> lightShadowArrayTex : register(t9);

SamplerComparisonState shadowSampler : register(s0);
SamplerState cubeMapSampler : register(s1);

RWTexture2D<float4> sceneColor : register(u0);

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
    return Surface.c_diff * Fresnel_Shlick(1, fd90, Light.NdotL).x * Fresnel_Shlick(1, fd90, Surface.NdotV).x;
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
    float GV = G_Shlick_Smith_Hable(Surface, Light);

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

// // Diffuse irradiance
// float3 Diffuse_IBL(SurfaceProperties Surface)
// {
//     // Assumption:  L = N
//
//     //return Surface.c_diff * irradianceIBLTexture.Sample(defaultSampler, Surface.N);
//
//     // This is nicer but more expensive, and specular can often drown out the diffuse anyway
//     float LdotH = saturate(dot(Surface.N, normalize(Surface.N + Surface.V)));
//     float fd90 = 0.5 + 2.0 * Surface.roughness * LdotH * LdotH;
//     float3 DiffuseBurley = Surface.c_diff * Fresnel_Shlick(1, fd90, Surface.NdotV);
//     return DiffuseBurley * irradianceIBLTexture.Sample(defaultSampler, Surface.N);
// }
//
// // Approximate specular IBL by sampling lower mips according to roughness.  Then modulate by Fresnel. 
// float3 Specular_IBL(SurfaceProperties Surface)
// {
//     float lod = Surface.roughness * IBLRange + IBLBias;
//     float3 specular = Fresnel_Shlick(Surface.c_spec, 1, Surface.NdotV);
//     return specular * radianceIBLTexture.SampleLevel(cubeMapSampler, reflect(-Surface.V, Surface.N), lod);
// }

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

// float3 ApplyLightCommon(
//     float3	diffuseColor,	// Diffuse albedo
//     float3	specularColor,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float	gloss,			// Specular power
//     float3	normal,			// World-space normal
//     float3	viewDir,		// World-space vector from eye to point
//     float3	lightDir,		// World-space vector from point to light
//     float3	lightColor		// Radiance of directional light
//     )
// {
//     float3 halfVec = normalize(lightDir - viewDir);
//     float nDotH = saturate(dot(halfVec, normal));
//
//     FSchlick( diffuseColor, specularColor, lightDir, halfVec );
//
//     float specularFactor = specularMask * pow(nDotH, gloss) * (gloss + 2) / 8;
//
//     float nDotL = saturate(dot(normal, lightDir));
//
//     return nDotL * lightColor * (diffuseColor + specularFactor * specularColor);
// }

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

// float3 ApplyPointLight(
//     float3	diffuseColor,	// Diffuse albedo
//     float3	specularColor,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float	gloss,			// Specular power
//     float3	normal,			// World-space normal
//     float3	viewDir,		// World-space vector from eye to point
//     float3	worldPos,		// World-space fragment position
//     float3	lightPos,		// World-space light position
//     float	lightRadiusSq,
//     float3	lightColor		// Radiance of directional light
//     )
// {
//     float3 lightDir = lightPos - worldPos;
//     float lightDistSq = dot(lightDir, lightDir);
//     float invLightDist = rsqrt(lightDistSq);
//     lightDir *= invLightDist;
//
//     // modify 1/d^2 * R^2 to fall off at a fixed radius
//     // (R/d)^2 - d/R = [(1/d^2) - (1/R^2)*(d/R)] * R^2
//     float distanceFalloff = lightRadiusSq * (invLightDist * invLightDist);
//     distanceFalloff = max(0, distanceFalloff - rsqrt(distanceFalloff));
//
//     return distanceFalloff * ApplyLightCommon(
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
//
// float3 ApplyConeLight(
//     float3	diffuseColor,	// Diffuse albedo
//     float3	specularColor,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float	gloss,			// Specular power
//     float3	normal,			// World-space normal
//     float3	viewDir,		// World-space vector from eye to point
//     float3	worldPos,		// World-space fragment position
//     float3	lightPos,		// World-space light position
//     float	lightRadiusSq,
//     float3	lightColor,		// Radiance of directional light
//     float3	coneDir,
//     float2	coneAngles
//     )
// {
//     float3 lightDir = lightPos - worldPos;
//     float lightDistSq = dot(lightDir, lightDir);
//     float invLightDist = rsqrt(lightDistSq);
//     lightDir *= invLightDist;
//
//     // modify 1/d^2 * R^2 to fall off at a fixed radius
//     // (R/d)^2 - d/R = [(1/d^2) - (1/R^2)*(d/R)] * R^2
//     float distanceFalloff = lightRadiusSq * (invLightDist * invLightDist);
//     distanceFalloff = max(0, distanceFalloff - rsqrt(distanceFalloff));
//
//     float coneFalloff = dot(-lightDir, coneDir);
//     coneFalloff = saturate((coneFalloff - coneAngles.y) * coneAngles.x);
//
//     return (coneFalloff * distanceFalloff) * ApplyLightCommon(
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
//
// float3 ApplyConeShadowedLight(
//     float3	diffuseColor,	// Diffuse albedo
//     float3	specularColor,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float	gloss,			// Specular power
//     float3	normal,			// World-space normal
//     float3	viewDir,		// World-space vector from eye to point
//     float3	worldPos,		// World-space fragment position
//     float3	lightPos,		// World-space light position
//     float	lightRadiusSq,
//     float3	lightColor,		// Radiance of directional light
//     float3	coneDir,
//     float2	coneAngles,
//     float4x4 shadowTextureMatrix,
//     uint	lightIndex
//     )
// {
//     float4 shadowCoord = mul(shadowTextureMatrix, float4(worldPos, 1.0));
//     shadowCoord.xyz *= rcp(shadowCoord.w);
//     float shadow = GetShadowConeLight(lightIndex, shadowCoord.xyz);
//
//     return shadow * ApplyConeLight(
//         diffuseColor,
//         specularColor,
//         specularMask,
//         gloss,
//         normal,
//         viewDir,
//         worldPos,
//         lightPos,
//         lightRadiusSq,
//         lightColor,
//         coneDir,
//         coneAngles
//         );
// }

// void ShadeLights(inout float3 colorSum, uint2 pixelPos,
//     float3	diffuseAlbedo,	// Diffuse albedo
//     float3	specularAlbedo,	// Specular albedo
//     float	specularMask,	// Where is it shiny or dingy?
//     float gloss,
//     float3 normal,
//     float3 viewDir,
//     float3 worldPos
//     )
// {
//     uint2 tilePos = GetTilePos(pixelPos, InvTileDim.xy);
//     uint tileIndex = GetTileIndex(tilePos, TileCount.x);
//     uint tileOffset = GetTileOffset(tileIndex);
//     uint tileLightCount = lightGrid.Load(tileOffset + 0);
//     uint tileLightCountSphere = (tileLightCount >> 0) & 0xff;
//     uint tileLightCountCone = (tileLightCount >> 8) & 0xff;
//     uint tileLightCountConeShadowed = (tileLightCount >> 16) & 0xff;
//
//     uint tileLightLoadOffset = tileOffset + 4;
//
// #define POINT_LIGHT_ARGS \
//     diffuseAlbedo, \
//     specularAlbedo, \
//     specularMask, \
//     gloss, \
//     normal, \
//     viewDir, \
//     worldPos, \
//     lightData.pos, \
//     lightData.radiusSq, \
//     lightData.color
//
// #define CONE_LIGHT_ARGS \
//     POINT_LIGHT_ARGS, \
//     lightData.coneDir, \
//     lightData.coneAngles
//
// #define SHADOWED_LIGHT_ARGS \
//     CONE_LIGHT_ARGS, \
//     lightData.shadowTextureMatrix, \
//     lightIndex
//         
//     // sphere
//     uint n;
//     for (n = 0; n < tileLightCountSphere; n++, tileLightLoadOffset += 4)
//     {
//         uint lightIndex = lightGrid.Load(tileLightLoadOffset);
//         LightData lightData = lightBuffer[lightIndex];
//         colorSum += ApplyPointLight(POINT_LIGHT_ARGS);
//     }
//
//     // cone
//     for (n = 0; n < tileLightCountCone; n++, tileLightLoadOffset += 4)
//     {
//         uint lightIndex = lightGrid.Load(tileLightLoadOffset);
//         LightData lightData = lightBuffer[lightIndex];
//         colorSum += ApplyConeLight(CONE_LIGHT_ARGS);
//     }
//
//     // cone w/ shadow map
//     for (n = 0; n < tileLightCountConeShadowed; n++, tileLightLoadOffset += 4)
//     {
//         uint lightIndex = lightGrid.Load(tileLightLoadOffset);
//         LightData lightData = lightBuffer[lightIndex];
//         colorSum += ApplyConeShadowedLight(SHADOWED_LIGHT_ARGS);
//     }
// }

#define _RootSig \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 10))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))," \
    "StaticSampler(s0," \
        "addressU = TEXTURE_ADDRESS_CLAMP," \
        "addressV = TEXTURE_ADDRESS_CLAMP," \
        "addressW = TEXTURE_ADDRESS_CLAMP," \
        "comparisonFunc = COMPARISON_GREATER_EQUAL," \
        "filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)," \
    "StaticSampler(s1, maxAnisotropy = 8)"

[RootSignature(_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint2 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 DTid = Gid * uint2(8, 8) + GTid;
    if (DTid.x < ViewportWidth && DTid.y < ViewportHeight)
    {
        float4 colorAccum = sceneColor[DTid];
        float3 posW = gBufferA[DTid].xyz;
        float3 normal = gBufferB[DTid].xyz;
        float3 baseColor = gBufferC[DTid].xyz;
        float3 metallicRoughnessOcclusion = gBufferC[DTid].xyz;

        SurfaceProperties Surface;
        Surface.N = normal;
        Surface.V = normalize(ViewerPos - posW);
        Surface.NdotV = saturate(dot(Surface.N, Surface.V));
        Surface.c_diff = baseColor.rgb * (1 - kDielectricSpecular) * (1 - metallicRoughnessOcclusion.x) * metallicRoughnessOcclusion.z;
        Surface.c_spec = lerp(kDielectricSpecular, baseColor.rgb, metallicRoughnessOcclusion.x) * metallicRoughnessOcclusion.z;
        Surface.roughness = metallicRoughnessOcclusion.y;
        Surface.alpha = metallicRoughnessOcclusion.y * metallicRoughnessOcclusion.y;
        Surface.alphaSqr = Surface.alpha * Surface.alpha;

        float4 shadowCoord = mul(SunShadowMatrix, float4(posW, 1.0));
        shadowCoord.xyz *= rcp(shadowCoord.w);
        float sunShadow = GetDirectionalShadow(shadowCoord.xyz, texShadow);
        colorAccum.rgb += ShadeDirectionalLight(Surface, SunDirection, sunShadow * SunColor);
        
        // float ssao = texSSAO[DTid];
        //
        // Surface.c_diff *= ssao;
        // Surface.c_spec *= ssao;

        // Add IBL
        // colorAccum.rgb += Diffuse_IBL(Surface);
        // colorAccum.rgb += Specular_IBL(Surface);
        
        sceneColor[DTid] = colorAccum;
    }
}

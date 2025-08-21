
#include "LightGrid.hlsli"
#include "LightingCommon.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

Texture2D<float4> gBufferA : register(t0);
Texture2D<float4> gBufferB : register(t1);
Texture2D<float4> gBufferC : register(t2);
Texture2D<float4> gBufferD : register(t3);

//SamplerState defaultSampler : register(s0);
//SamplerComparisonState shadowSampler : register(s1);
//SamplerState cubeMapSampler : register(s2);

RWTexture2D<float4> sceneColor : register(u0);

#define _RootSig \
    "RootFlags(0), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors = 10))," \
    "DescriptorTable(SRV(t10, numDescriptors = 10))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))," \
    "StaticSampler(s10, maxAnisotropy = 8)," \
    "StaticSampler(s11," \
        "addressU = TEXTURE_ADDRESS_CLAMP," \
        "addressV = TEXTURE_ADDRESS_CLAMP," \
        "addressW = TEXTURE_ADDRESS_CLAMP," \
        "comparisonFunc = COMPARISON_GREATER_EQUAL," \
        "filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)," \
    "StaticSampler(s12, maxAnisotropy = 8)"

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
        float3 metallicRoughnessOcclusion = gBufferD[DTid].xyz;

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

        ShadeLights(colorAccum.rgb, Surface, DTid, posW);
        
        float ssao = texSSAO[DTid];
        // Add IBL
        //colorAccum.rgb += Diffuse_IBL(Surface) * ssao;
        colorAccum.rgb += EvaluateIBLDiffuse(Surface) * ssao;

        //colorAccum.rgb += Specular_IBL(Surface) * ssao;
        colorAccum.rgb += EvaluateIBLSpecular(Surface) * ssao;
        
        sceneColor[DTid] = colorAccum;
    }
}


#include "LightGrid.hlsli"
#include "LightingCommon.hlsli"
#include "Common.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

float3 ConvertPixelToWorldPos(uint2 Pixel, Texture2D<float> sceneDepth)
{
    float NDCX = (Pixel.x + 0.5) / ViewportWidth * 2.0 - 1.0;
    float NDCY = 1.0 - (Pixel.y + 0.5) / ViewportHeight * 2.0;
    float SceneDepth = sceneDepth[Pixel];
    float4 Result = mul(InverseViewProjMatrix, float4(NDCX, NDCY, SceneDepth, 1.0));

    return Result.xyz / Result.w;
}

[RootSignature(Renderer_RootSig)]
[numthreads(8, 8, 1)]
void main(
    uint2 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 DTid = Gid * uint2(8, 8) + GTid;
    if (DTid.x < ViewportWidth && DTid.y < ViewportHeight)
    {
        RWTexture2D<float4> sceneColor = GetSceneColorUAV();
        Texture2D<float4> gBufferD = GetGBufferDSRV();
        uint viewMode = gBufferD[DTid].a;
        if (viewMode == VIEW_MODE_LIT)
        {
            Texture2D<float4> gBufferA = GetGBufferASRV();
            Texture2D<float4> gBufferB = GetGBufferBSRV();
            Texture2D<float4> gBufferC = GetGBufferCSRV();
            Texture2D<float> gSceneDepth = GetSceneDepthSRV();

            float bShading = gBufferA[DTid].w;
            if (bShading > 1e-6)
            {
                float2 ScreenUV = (float2(0.5, 0.5) + DTid) / float2(ViewportWidth, ViewportHeight);
                float4 colorAccum = sceneColor[DTid];
                float3 posW = ConvertPixelToWorldPos(DTid, gSceneDepth);
                float3 normal = gBufferA[DTid].xyz;
                float3 baseColor = gBufferB[DTid].xyz;
                float3 metallicRoughnessOcclusion = gBufferC[DTid].xyz;
                float ssao = GetSSAOSRV()[DTid];

                SurfaceProperties Surface;
                Surface.N = normal;
                Surface.V = normalize(ViewerPos - posW);
                Surface.NdotV = saturate(dot(Surface.N, Surface.V));
                Surface.c_diff = baseColor.rgb * (1 - metallicRoughnessOcclusion.x) * metallicRoughnessOcclusion.z;
                Surface.c_spec = lerp(kDielectricSpecular, baseColor.rgb, metallicRoughnessOcclusion.x) * metallicRoughnessOcclusion.z;
                Surface.roughness = metallicRoughnessOcclusion.y;
                Surface.alpha = metallicRoughnessOcclusion.y * metallicRoughnessOcclusion.y;
                Surface.alphaSqr = Surface.alpha * Surface.alpha;

                float4 shadowCoord = mul(SunShadowMatrix, float4(posW, 1.0));
                shadowCoord.xyz *= rcp(shadowCoord.w);
                // TODO: shadows have artifacts
                float sunShadow = GetDirectionalShadow(ScreenUV, shadowCoord.xyz);
                // TODO: specular aliasing, especially when roughness is near 0
                colorAccum.rgb += ShadeDirectionalLight(Surface, SunDirection, sunShadow * SunIntensity);

                ShadeLights(colorAccum.rgb, Surface, DTid, posW);

                // Add IBL and modulate by runtime SSAO.
                colorAccum.rgb += EvaluateIBLDiffuse(Surface) * ssao;
                colorAccum.rgb += EvaluateIBLSpecular(Surface) * ssao;

                sceneColor[DTid] = colorAccum;
            }
        }
        else
        {
            sceneColor[DTid] = float4(gBufferD[DTid].rgb, 1.0f);
        }
    }
}

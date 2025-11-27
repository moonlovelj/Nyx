
#include "LightGrid.hlsli"
#include "LightingCommon.hlsli"
#include "Common.hlsli"

// outdated warning about for-loop variable scope
#pragma warning (disable: 3078)

Texture2D<float4> gBufferA : register(t10);
Texture2D<float4> gBufferB : register(t11);
Texture2D<float4> gBufferC : register(t12);
Texture2D<float4> gBufferD : register(t13);
Texture2D<float> gSceneDepth : register(t14);

RWTexture2D<float4> sceneColor : register(u0);

float3 ConvertPixelToWorldPos(uint2 Pixel)
{
    float NDCX = (Pixel.x + 0.5) / ViewportWidth * 2.0 - 1.0;
    float NDCY = 1.0 - (Pixel.y + 0.5) / ViewportHeight * 2.0;
    float SceneDepth = gSceneDepth[Pixel];
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
        uint viewMode = gBufferD[DTid].a;
        if (viewMode == VIEW_MODE_LIT)
        {
            float bShading = gBufferA[DTid].w;
            if (bShading > 1e-6)
            {
                float2 ScreenUV = (float2(0.5, 0.5) + DTid) / float2(ViewportWidth, ViewportHeight);
                float4 colorAccum = sceneColor[DTid];
                float3 posW = ConvertPixelToWorldPos(DTid);
                float3 normal = gBufferA[DTid].xyz;
                float3 baseColor = gBufferB[DTid].xyz;
                float3 metallicRoughnessOcclusion = gBufferC[DTid].xyz;

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
            // TODO 阴影有瑕疵
                float sunShadow = GetDirectionalShadow(ScreenUV, shadowCoord.xyz, texShadow);
            // TODO 高光有锯齿，尤其是粗糙度接近0时
                colorAccum.rgb += ShadeDirectionalLight(Surface, SunDirection, sunShadow * SunIntensity);

                ShadeLights(colorAccum.rgb, Surface, DTid, posW);

            //TODO ssao在球面有严重条纹瑕疵，待修复
            //float ssao = texSSAO[DTid];
            
            // Add IBL
                colorAccum.rgb += EvaluateIBLDiffuse(Surface);
                colorAccum.rgb += EvaluateIBLSpecular(Surface);

                sceneColor[DTid] = colorAccum;
            }
        }
        else
        {
            sceneColor[DTid] = float4(gBufferD[DTid].rgb, 1.0f);
        }
    }
}

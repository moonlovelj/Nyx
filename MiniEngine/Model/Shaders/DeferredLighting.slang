//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

#include "CommonResources.slang"
#include "ViewCommon.h.slang"
#include "VirtualShadowMap.h.slang"
#include "ViewMode.h.slang"

struct DeferredLightingConstants
{
    float4x4 InverseViewProjMatrix;
    float4x4 SunShadowMatrix;
    float3 ViewerPos;
    float3 SunDirection;
    float3 SunIntensity;
    float4 ShadowTexelSize;
    float4 InvTileDim;
    uint ViewportWidth;
    uint ViewportHeight;
    uint TileCountX;
    uint IBLSpecularLDMapMipCount;
    uint VsmViewId;
};

ConstantBuffer<DeferredLightingConstants> g_DeferredLighting;
StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
ByteAddressBuffer g_VsmPageRequestMask;
StructuredBuffer<uint> g_VsmPageTable;

#define InverseViewProjMatrix g_DeferredLighting.InverseViewProjMatrix
#define SunShadowMatrix g_DeferredLighting.SunShadowMatrix
#define ViewerPos g_DeferredLighting.ViewerPos
#define SunDirection g_DeferredLighting.SunDirection
#define SunIntensity g_DeferredLighting.SunIntensity
#define ShadowTexelSize g_DeferredLighting.ShadowTexelSize
#define InvTileDim g_DeferredLighting.InvTileDim
#define ViewportWidth g_DeferredLighting.ViewportWidth
#define ViewportHeight g_DeferredLighting.ViewportHeight
#define TileCountX g_DeferredLighting.TileCountX
#define IBLSpecularLDMapMipCount g_DeferredLighting.IBLSpecularLDMapMipCount

#include "LightingCommon.h.slang"

float3 VisualizeDirectionalVsmAddress(uint2 pixelPosition)
{
    if (g_DeferredLighting.VsmViewId == VSM_INVALID_VIEW_ID)
        return float3(1.0f, 0.0f, 1.0f);

    VsmShadowView shadowView = g_VsmShadowViews[g_DeferredLighting.VsmViewId];
    if (shadowView.AddressType != VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
        return float3(1.0f, 0.0f, 1.0f);

    Texture2D<float> sceneDepth = GetSceneDepthSRV();
    float depth = sceneDepth[pixelPosition];
    if (depth == 0.0f)
        return float3(0.15f);

    float3 worldPosition = ReconstructWorldPosition(
        pixelPosition,
        depth,
        uint2(ViewportWidth, ViewportHeight),
        InverseViewProjMatrix);
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    float3 addressColor = float3(0.0f, 0.5f, 1.0f);
    bool pageBorder = any(address.TexelInPage < uint2(2u)) ||
        any(address.TexelInPage >= uint2(VSM_PAGE_SIZE - 2u));

    if (pageBorder)
        addressColor = float3(0.0f);

    if (address.InsideWindow != 0u)
    {
        // Yellow means a visible receiver mapped to a page that the request pass missed.
        if (!IsVsmPageRequested(g_VsmPageRequestMask, shadowView, address.PageTableCoord))
        {
            addressColor = float3(1.0f, 1.0f, 0.0f);
        }
        // Magenta means the request could not obtain a physical page.
        else if (GetVsmPhysicalPageIndex(g_VsmPageTable, shadowView, address.PageTableCoord) ==
                 VSM_INVALID_PHYSICAL_PAGE)
        {
            addressColor = float3(1.0f, 0.0f, 1.0f);
        }
    }
    else
    {
        addressColor = float3(1.0f, 0.0f, 0.0f);
    }

    return addressColor;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(
    uint2 Gid : SV_GroupID,
    uint2 GTid : SV_GroupThreadID,
    uint GI : SV_GroupIndex)
{
    uint2 DTid = Gid * uint2(8, 8) + GTid;
    if (DTid.x < ViewportWidth && DTid.y < ViewportHeight)
    {
        RWTexture2D<float4> sceneColor = GetSceneColorUAV();
        Texture2D<float4> gBufferD = GetGBufferDSRV();
        uint viewMode = (uint)gBufferD[DTid].a;
        if (viewMode == VIEW_MODE_SHOW_VSM_ADDRESS)
        {
            sceneColor[DTid] = float4(VisualizeDirectionalVsmAddress(DTid), 1.0f);
        }
        else if (viewMode == VIEW_MODE_LIT)
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
                float3 posW = ReconstructWorldPosition(
                    DTid,
                    gSceneDepth[DTid],
                    uint2(ViewportWidth, ViewportHeight),
                    InverseViewProjMatrix);
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
                const float roughness = clamp(metallicRoughnessOcclusion.y, 0.04f, 1.0f);
                Surface.roughness = roughness;
                Surface.alpha = roughness * roughness;
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

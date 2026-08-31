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
#include "VsmSampling.h.slang"
#include "ViewMode.h.slang"

struct DeferredLightingConstants
{
    float4x4 InverseViewProjMatrix;
    float3 ViewerPos;
    float3 SunDirection;
    float3 SunIntensity;
    float4 InvTileDim;
    uint ViewportWidth;
    uint ViewportHeight;
    uint TileCountX;
    uint IBLSpecularLDMapMipCount;
    uint VsmClipmapId;
    uint EnableSunVsmSampling;
};

ConstantBuffer<DeferredLightingConstants> g_DeferredLighting;
ByteAddressBuffer g_VsmPageRequestMask;

#define InverseViewProjMatrix g_DeferredLighting.InverseViewProjMatrix
#define ViewerPos g_DeferredLighting.ViewerPos
#define SunDirection g_DeferredLighting.SunDirection
#define SunIntensity g_DeferredLighting.SunIntensity
#define InvTileDim g_DeferredLighting.InvTileDim
#define ViewportWidth g_DeferredLighting.ViewportWidth
#define ViewportHeight g_DeferredLighting.ViewportHeight
#define TileCountX g_DeferredLighting.TileCountX
#define IBLSpecularLDMapMipCount g_DeferredLighting.IBLSpecularLDMapMipCount

#include "LightingCommon.h.slang"

float3 GetVsmPhysicalPageStateColor(VsmPhysicalPageMetadata metadata)
{
    bool isRendered = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_RENDERED) != 0u;
    bool isDirty = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_DIRTY) != 0u;

    if (!isRendered)
        return float3(0.1f, 0.35f, 1.0f); // Blue: waiting for the first render.

    if (isDirty)
        return float3(1.0f, 0.35f, 0.0f); // Orange: stale, but history is available.

    return float3(0.1f, 0.9f, 0.15f); // Green: valid cached depth.
}

bool TryReconstructVsmReceiverWorldPosition(uint2 pixelPosition, out float3 worldPosition)
{
    worldPosition = float3(0.0f);
    float depth = GetSceneDepthSRV()[pixelPosition];
    if (depth == 0.0f)
        return false;

    worldPosition = ReconstructWorldPosition(
        pixelPosition,
        depth,
        uint2(ViewportWidth, ViewportHeight),
        InverseViewProjMatrix);
    return true;
}

float3 VisualizeDirectionalVsmPageState(uint2 pixelPosition)
{
    if (g_DeferredLighting.VsmClipmapId == VSM_INVALID_VIEW_ID)
        return float3(1.0f, 0.0f, 1.0f);

    float3 worldPosition;
    if (!TryReconstructVsmReceiverWorldPosition(pixelPosition, worldPosition))
        return float3(0.15f);

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[g_DeferredLighting.VsmClipmapId];
    uint level = SelectResidentDirectionalVsmClipmapLevel(worldPosition, clipmap);
    VsmShadowView shadowView = g_VsmShadowViews[clipmap.FirstViewId + level];
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    float3 addressColor = float3(0.0f);
    bool pageBorder = any(address.TexelInPage < uint2(2u)) ||
        any(address.TexelInPage >= uint2(VSM_PAGE_SIZE - 2u));

    if (address.Valid != 0u)
    {
        // Yellow means a visible receiver mapped to a page that the request pass missed.
        if (!IsVsmPageRequested(g_VsmPageRequestMask, shadowView, address.PageTableCoord))
        {
            addressColor = float3(1.0f, 1.0f, 0.0f);
        }
        // Magenta means the request could not obtain a physical page.
        else
        {
            uint physicalPageIndex = GetVsmPhysicalPageIndex(g_VsmPageTable, shadowView, address.PageTableCoord);
            if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
            {
                addressColor = float3(1.0f, 0.0f, 1.0f);
            }
            else
            {
                VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadata[physicalPageIndex];
                bool metadataMatches = IsVsmPhysicalPageMetadataMatch(metadata, shadowView, address.VirtualPage);

                // White indicates that the page table and reverse mapping disagree.
                addressColor = !metadataMatches
                    ? float3(1.0f)
                    : pageBorder ? float3(0.0f) : GetVsmPhysicalPageStateColor(metadata);
            }
        }
    }
    else
    {
        addressColor = float3(1.0f, 0.0f, 0.0f);
    }

    return addressColor;
}

float3 VisualizeDirectionalVsmClipmapLevel(uint2 pixelPosition)
{
    if (g_DeferredLighting.VsmClipmapId == VSM_INVALID_VIEW_ID)
        return float3(1.0f, 0.0f, 1.0f);

    float3 worldPosition;
    if (!TryReconstructVsmReceiverWorldPosition(pixelPosition, worldPosition))
        return float3(0.15f);

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[g_DeferredLighting.VsmClipmapId];
    uint level = SelectResidentDirectionalVsmClipmapLevel(worldPosition, clipmap);
    VsmShadowView shadowView = g_VsmShadowViews[clipmap.FirstViewId + level];
    DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
    VsmVirtualAddress address = GetDirectionalVsmVirtualAddress(worldPosition, addressData);
    if (address.Valid == 0u)
        return float3(1.0f);

    bool pageBorder = any(address.TexelInPage < uint2(2u)) ||
        any(address.TexelInPage >= uint2(VSM_PAGE_SIZE - 2u));
    if (pageBorder)
        return float3(0.0f);

    const float goldenRatioConjugate = 0.61803398875f;
    float hue = frac(float(level) * goldenRatioConjugate);
    return HsvToRgb(float3(hue, 0.75f, 0.95f));
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
        if (viewMode == VIEW_MODE_SHOW_VSM_PAGE_STATE)
        {
            sceneColor[DTid] = float4(VisualizeDirectionalVsmPageState(DTid), 1.0f);
        }
        else if (viewMode == VIEW_MODE_SHOW_VSM_CLIPMAP_LEVEL)
        {
            sceneColor[DTid] = float4(VisualizeDirectionalVsmClipmapLevel(DTid), 1.0f);
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

                float sunShadow = 1.0f;
                if (g_DeferredLighting.EnableSunVsmSampling != 0u &&
                    g_DeferredLighting.VsmClipmapId != VSM_INVALID_VIEW_ID)
                {
                    sunShadow = SampleDirectionalVsmClipmapHard(posW, g_DeferredLighting.VsmClipmapId);
                }

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

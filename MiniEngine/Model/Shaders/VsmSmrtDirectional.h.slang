#ifndef NYX_VSM_SMRT_DIRECTIONAL_H_SLANG
#define NYX_VSM_SMRT_DIRECTIONAL_H_SLANG

#include "VsmSampling.h.slang"
#include "VsmSmrt.h.slang"

float SampleDirectionalVsmSmrt(
    float3 worldPosition,
    float3 geometricNormalWS,
    float3 viewerPositionWS,
    uint2 pixelPosition,
    uint clipmapId,
    VsmScreenRayTraceContext screenRayTraceContext)
{
    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    uint preferredLevel = SelectResidentDirectionalVsmClipmapLevel(worldPosition, clipmap);

    DirectionalVsmDepthQuery receiverQuery;
    float3 receiverPositionWS;
    if (!TryQueryDirectionalVsmReceiverClipmap(
            worldPosition,
            geometricNormalWS,
            clipmap,
            preferredLevel,
            receiverQuery,
            receiverPositionWS))
        return 1.0f;

    VsmShadowView receiverView = g_VsmShadowViews[receiverQuery.ViewId];
    DirectionalVsmAddressGpu receiverAddress =
        g_DirectionalVsmAddresses[receiverView.AddressDataIndex];
    float receiverBias = GetDirectionalVsmQueryReceiverBias(receiverQuery, geometricNormalWS);

    float screenOffset = 0.0f;
    if (g_VsmSampling.EnableScreenRayTrace != 0u)
    {
        screenOffset = TraceVsmScreenRay(
            worldPosition,
            g_VsmSampling.ScreenRayLengthScale,
            screenRayTraceContext);
    }

    float depthPerWorldUnit = receiverQuery.DepthPerWorldUnit;
    if (depthPerWorldUnit <= 0.0f)
    {
        return SampleDirectionalVsmClipmapHard(
            worldPosition,
            geometricNormalWS,
            clipmapId,
            screenRayTraceContext);
    }

    float fallbackBias = max(receiverBias, screenOffset * depthPerWorldUnit);
    float hardShadow = receiverQuery.ReferenceDepth + fallbackBias >= receiverQuery.ShadowDepth
        ? 1.0f : 0.0f;
    float biasWorld = receiverBias / depthPerWorldUnit;
    float startOffset = max(screenOffset, biasWorld);
    float3 rayOrigin = receiverPositionWS + receiverAddress.WorldToLightRow2.xyz * startOffset;

    float traceLength = max(
        distance(worldPosition, viewerPositionWS) * g_VsmSampling.SmrtRayLengthScale - startOffset,
        0.0f);
    if (traceLength <= 0.0f)
        return hardShadow;

    uint rayCount = min(max(g_VsmSampling.SmrtRayCount, 1u), 8u);
    uint sampleCount = min(max(g_VsmSampling.SmrtSamplesPerRay, 2u), 12u);
    uint frameIndex = g_VsmSampling.SmrtFreezeNoise != 0u ? 0u : g_VsmSampling.SmrtFrameIndex;
    float3 lightDirection = normalize(receiverAddress.WorldToLightRow2.xyz);
    float3 lightRight = normalize(receiverAddress.WorldToLightRow0.xyz);
    float3 lightUp = normalize(receiverAddress.WorldToLightRow1.xyz);
    float visibility = 0.0f;

    [loop]
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
    {
        float2 disk = SampleVsmSmrtDisk(pixelPosition, frameIndex, rayIndex);
        float3 rayDirection = normalize(
            lightDirection +
            (lightRight * disk.x + lightUp * disk.y) * clipmap.LightAngularRadiusTan);
        float sampleJitter = GetVsmSmrtRandom(pixelPosition, frameIndex, 37u + rayIndex);

        VsmSmrtIntersectionState state;
        ResetVsmSmrtIntersectionState(state);
        bool hit = false;
        bool allSamplesValid = true;

        [loop]
        for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
        {
            float sampleTime = GetVsmSmrtSampleTime(sampleIndex, sampleCount, sampleJitter);
            float3 samplePosition = rayOrigin + rayDirection * (traceLength * sampleTime);

            DirectionalVsmDepthQuery sampleQuery;
            if (!TryQueryDirectionalVsmClipmap(
                    samplePosition,
                    rayOrigin,
                    clipmap,
                    preferredLevel,
                    sampleQuery))
            {
                allSamplesValid = false;
                // Never infer a hit across a missing page or depth sample.
                ResetVsmSmrtIntersectionState(state);
                continue;
            }

            float depthToleranceWorld = GetVsmSmrtDepthUlp(sampleQuery.ReferenceDepth) *
                float(g_VsmSampling.MinReceiverBiasUlp) / sampleQuery.DepthPerWorldUnit;
            hit = ConsumeVsmSmrtDepthSample(
                state,
                sampleQuery.ReferenceRayDistance,
                sampleQuery.ShadowRayDistance,
                depthToleranceWorld,
                sampleIndex == 0u);
            if (hit)
                break;
        }

        visibility += hit ? 0.0f : allSamplesValid ? 1.0f : hardShadow;
    }

    return visibility / float(rayCount);
}

float SampleDirectionalVsmShadow(
    float3 worldPosition,
    float3 geometricNormalWS,
    float3 viewerPositionWS,
    uint2 pixelPosition,
    uint clipmapId,
    VsmScreenRayTraceContext screenRayTraceContext)
{
    if (clipmapId == VSM_INVALID_VIEW_ID)
        return 1.0f;

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    if (g_VsmSampling.EnableDirectionalSmrt != 0u && clipmap.LightAngularRadiusTan > 0.0f)
    {
        return SampleDirectionalVsmSmrt(
            worldPosition,
            geometricNormalWS,
            viewerPositionWS,
            pixelPosition,
            clipmapId,
            screenRayTraceContext);
    }

    return SampleDirectionalVsmClipmapHard(
        worldPosition,
        geometricNormalWS,
        clipmapId,
        screenRayTraceContext);
}

#endif

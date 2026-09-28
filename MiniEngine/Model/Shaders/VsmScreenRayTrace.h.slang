#ifndef NYX_VSM_SCREEN_RAY_TRACE_H_SLANG
#define NYX_VSM_SCREEN_RAY_TRACE_H_SLANG

#include "CommonResources.slang"

#define VSM_SCREEN_RAY_SAMPLE_COUNT 4u

struct VsmScreenRayTraceContext
{
    float4x4 ScreenViewProjMatrix;
    float3 RayDirectionWS;
    uint2 ViewportSize;
    float StartSceneDepth;
    float ScreenRayLengthMultiplier;
};

float TraceVsmScreenRay(
    float3 worldPosition,
    float rayLengthScale,
    VsmScreenRayTraceContext context)
{
    if (rayLengthScale <= 0.0f || any(context.ViewportSize == uint2(0u)))
        return 0.0f;

    float4 startClip = mul(context.ScreenViewProjMatrix, float4(worldPosition, 1.0f));
    // clip.w is linear view depth for perspective projection and 1 for orthographic projection.
    float rayLength = max(startClip.w, 0.0f) *
        context.ScreenRayLengthMultiplier * rayLengthScale;
    if (rayLength <= 0.0f)
        return 0.0f;

    float4 endClip = mul(
        context.ScreenViewProjMatrix,
        float4(worldPosition + context.RayDirectionWS * rayLength, 1.0f));
    if (endClip.w <= 0.0f)
        return rayLength;

    float3 startNdc = startClip.xyz / startClip.w;
    float3 endNdc = endClip.xyz / endClip.w;
    float step = 1.0f / float(VSM_SCREEN_RAY_SAMPLE_COUNT);
    Texture2D<float> sceneDepth = GetSceneDepthSRV();

    [unroll]
    for (uint sampleIndex = 0u; sampleIndex < VSM_SCREEN_RAY_SAMPLE_COUNT; ++sampleIndex)
    {
        float sampleTime = float(sampleIndex + 1u) * step;
        float3 sampleNdc = lerp(startNdc, endNdc, sampleTime);
        if (any(sampleNdc.xy < float2(-1.0f)) || any(sampleNdc.xy > float2(1.0f)) ||
            sampleNdc.z < 0.0f || sampleNdc.z > 1.0f)
        {
            return rayLength;
        }

        float2 sampleUv = float2(sampleNdc.x * 0.5f + 0.5f, 0.5f - sampleNdc.y * 0.5f);
        uint2 samplePixel = min(
            uint2(sampleUv * float2(context.ViewportSize)),
            context.ViewportSize - 1u);
        float sampleDepth = sceneDepth.Load(int3(samplePixel, 0));

        // Reverse-Z: a smaller ray depth is behind the scene depth.
        if (sampleDepth != context.StartSceneDepth && sampleNdc.z < sampleDepth)
        {
            return rayLength * max(0.0f, sampleTime - 1.5f * step);
        }
    }

    // No intersection: start the shadow comparison at the end of the short ray,
    // matching the screen-ray offset used by the VSM projection path.
    return rayLength;
}

#endif

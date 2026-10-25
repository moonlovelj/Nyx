#ifndef __HDR_DISPLAY_MAPPING_HLSLI__
#define __HDR_DISPLAY_MAPPING_HLSLI__

#include "ColorSpaceUtility.hlsli"

// Tone-mapped scene color is linear Rec.709 relative to paper white: 1 = paper white.
// The tone mapper has already compressed highlights to peak brightness. Only convert
// gamut, scale to absolute luminance, and PQ-encode here; do not tone map a second time.
float3 MapSceneToHDR10(float3 sceneColor, float paperWhiteRatio, float maxBrightness)
{
    float3 luminance = REC709toREC2020(max(sceneColor, 0.0)) * paperWhiteRatio;
    float maxLuminance = maxBrightness / 10000.0;
    return ApplyREC2084Curve(clamp(luminance, 0.0, maxLuminance));
}

#endif // __HDR_DISPLAY_MAPPING_HLSLI__

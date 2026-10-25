#ifndef __PBR_NEUTRAL_TONE_MAPPING_HLSLI__
#define __PBR_NEUTRAL_TONE_MAPPING_HLSLI__

static const float kPBRNeutralBlackOffset = 0.04;
static const float kPBRNeutralToeEnd = 0.08;
static const float kPBRNeutralToeScale = 6.25;
static const float kPBRNeutralCompressionStart = 0.8 - kPBRNeutralBlackOffset;

// Peak-aware extension of PBR Neutral in linear Rec.709. A ratio of 1 keeps
// the original SDR curve. HDR output is relative to paper white and can exceed 1.
float3 PBRNeutralToneMapping(float3 color, float peakBrightnessRatio = 1.0)
{
    peakBrightnessRatio = max(peakBrightnessRatio, 1.0);
    const float startCompression = kPBRNeutralCompressionStart * peakBrightnessRatio;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < kPBRNeutralToeEnd
        ? x - kPBRNeutralToeScale * x * x : kPBRNeutralBlackOffset;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = peakBrightnessRatio - startCompression;
    float newPeak = peakBrightnessRatio - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) / peakBrightnessRatio + 1.0);
    return lerp(color, newPeak * float3(1, 1, 1), g);
}

// Inverse for an achromatic value, used to calibrate the exposure meter.
// A display-linear gray of 0.18 requires an exposed input of 0.22.
float InversePBRNeutralGray(float gray, float peakBrightnessRatio = 1.0)
{
    peakBrightnessRatio = max(peakBrightnessRatio, 1.0);
    const float startCompression = kPBRNeutralCompressionStart * peakBrightnessRatio;
    gray = clamp(gray, 0.0, 0.99 * peakBrightnessRatio);
    if (gray < kPBRNeutralBlackOffset)
        return sqrt(gray / kPBRNeutralToeScale);
    if (gray < startCompression)
        return gray + kPBRNeutralBlackOffset;

    const float d = peakBrightnessRatio - startCompression;
    return kPBRNeutralBlackOffset + 2.0 * startCompression - peakBrightnessRatio
        + d * d / (peakBrightnessRatio - gray);
}

#endif // __PBR_NEUTRAL_TONE_MAPPING_HLSLI__

#ifndef NYX_VSM_SMRT_H_SLANG
#define NYX_VSM_SMRT_H_SLANG

struct VsmSmrtIntersectionState
{
    float PreviousGap;
    float PreviousTolerance;
    uint ValidSampleCount;
};

float GetVsmSmrtDepthUlp(float depth)
{
    return asfloat(asuint(depth) + 1u) - depth;
}

uint HashVsmSmrt(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float GetVsmSmrtRandom(uint2 pixelPosition, uint frameIndex, uint salt)
{
    uint seed = pixelPosition.x * 0x9e3779b9u +
        pixelPosition.y * 0x85ebca6bu +
        frameIndex * 0xc2b2ae35u +
        salt * 0x27d4eb2fu;
    return float(HashVsmSmrt(seed) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float2 SampleVsmSmrtDisk(uint2 pixelPosition, uint frameIndex, uint rayIndex)
{
    float radius = sqrt(GetVsmSmrtRandom(pixelPosition, frameIndex, rayIndex * 2u));
    float angle = 6.28318530718f * GetVsmSmrtRandom(pixelPosition, frameIndex, rayIndex * 2u + 1u);
    return radius * float2(cos(angle), sin(angle));
}

float GetVsmSmrtSampleTime(
    uint sampleIndex,
    uint sampleCount,
    float jitter)
{
    if (sampleIndex == 0u)
        return 1.0f;
    if (sampleIndex + 1u >= sampleCount)
        return 0.0f;

    float denominator = max(float(sampleCount - 1u), 1.0f);
    float normalized = (float(sampleIndex) + (jitter - 0.5f) * 0.5f) / denominator;
    float remaining = saturate(1.0f - normalized);
    return remaining * remaining;
}

void ResetVsmSmrtIntersectionState(out VsmSmrtIntersectionState state)
{
    state.PreviousGap = 0.0f;
    state.PreviousTolerance = 0.0f;
    state.ValidSampleCount = 0u;
}

bool ConsumeVsmSmrtDepthSample(
    inout VsmSmrtIntersectionState state,
    float referenceRayDistance,
    float shadowRayDistance,
    float depthToleranceWorld,
    bool allowInitialHit)
{
    float gap = shadowRayDistance - referenceRayDistance;
    if (state.ValidSampleCount == 0u)
    {
        state.PreviousGap = gap;
        state.PreviousTolerance = depthToleranceWorld;
        state.ValidSampleCount = 1u;
        // Only the actual ray endpoint may establish an initial hit without a crossing.
        return allowInitialHit && gap > depthToleranceWorld;
    }

    bool hit = state.PreviousGap < -state.PreviousTolerance && gap >= -depthToleranceWorld;
    state.PreviousGap = gap;
    state.PreviousTolerance = depthToleranceWorld;
    ++state.ValidSampleCount;
    return hit;
}

#endif

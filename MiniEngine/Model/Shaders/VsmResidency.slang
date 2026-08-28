#include "VirtualShadowMap.h.slang"

StructuredBuffer<DirectionalVsmClipmapGpu> g_DirectionalVsmClipmaps;
ByteAddressBuffer g_VsmPageManagementCounters;
RWStructuredBuffer<VsmResidencyStateGpu> g_VsmResidencyStates;

struct VsmResidencyConstants
{
    uint ClipmapCount;
    uint PhysicalPageCapacity;
    float TargetPoolLoad;
    float ResolutionDownLerpFactor;

    float ResolutionUpLerpFactor;
    uint RecoveryFrameCount;
};

ConstantBuffer<VsmResidencyConstants> g_VsmResidency;

[shader("compute")]
[numthreads(64, 1, 1)]
void commitResidencyStates(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint clipmapId = dispatchThreadId.x;
    if (clipmapId >= g_VsmResidency.ClipmapCount)
        return;

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    VsmResidencyStateGpu state = g_VsmResidencyStates[clipmap.ResidencyStateIndex];
    state.CurrentLodBias = clamp(state.NextLodBias, 0.0f, float(clipmap.LevelCount - 1u));
    g_VsmResidencyStates[clipmap.ResidencyStateIndex] = state;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void updateResidencyStates(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint clipmapId = dispatchThreadId.x;
    if (clipmapId >= g_VsmResidency.ClipmapCount)
        return;

    uint mappedPageCount =
        g_VsmPageManagementCounters.Load(VSM_REUSED_PAGE_COUNT_OFFSET) +
        g_VsmPageManagementCounters.Load(VSM_NEW_PAGE_COUNT_OFFSET);
    uint overflowPageCount = g_VsmPageManagementCounters.Load(VSM_OVERFLOW_PAGE_COUNT_OFFSET);

    DirectionalVsmClipmapGpu clipmap = g_DirectionalVsmClipmaps[clipmapId];
    VsmResidencyStateGpu state = g_VsmResidencyStates[clipmap.ResidencyStateIndex];
    float maxLodBias = float(clipmap.LevelCount - 1u);
    float currentLodBias = clamp(state.CurrentLodBias, 0.0f, maxLodBias);
    state.NextLodBias = currentLodBias;

    float requiredPageCount = float(mappedPageCount + overflowPageCount);
    float currentPoolLoad = requiredPageCount / float(g_VsmResidency.PhysicalPageCapacity);
    float loadRatio = max(currentPoolLoad / g_VsmResidency.TargetPoolLoad, 1e-6f);
    float targetLodBias = clamp(currentLodBias + log2(loadRatio), 0.0f, maxLodBias);
    bool overBudget = overflowPageCount != 0u || currentPoolLoad > g_VsmResidency.TargetPoolLoad;

    if (overBudget)
    {
        state.NextLodBias = lerp(currentLodBias, targetLodBias, g_VsmResidency.ResolutionDownLerpFactor);
        state.StableFrameCount = 0u;
    }
    else if (currentLodBias > 0.0f)
    {
        ++state.StableFrameCount;
        if (state.StableFrameCount >= g_VsmResidency.RecoveryFrameCount)
        {
            state.NextLodBias = lerp(currentLodBias, targetLodBias, g_VsmResidency.ResolutionUpLerpFactor);
        }
    }
    else
    {
        state.StableFrameCount = 0u;
    }

    g_VsmResidencyStates[clipmap.ResidencyStateIndex] = state;
}

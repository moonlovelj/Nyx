#include "VirtualShadowMap.h.slang"

#if (VSM_PHYSICAL_PAGE_CAPACITY & (VSM_PHYSICAL_PAGE_CAPACITY - 1u)) != 0u
#error VSM physical page capacity must be a power of two.
#endif

#if VSM_PHYSICAL_PAGE_CAPACITY > 4096u
#error VSM eviction sort supports at most 4096 physical pages.
#endif

#define VSM_EVICTION_SORT_THREAD_COUNT 512u

RWStructuredBuffer<VsmEvictionCandidate> g_VsmEvictionCandidates;
RWByteAddressBuffer g_VsmPageManagementCounters;

groupshared VsmEvictionCandidate s_EvictionCandidates[VSM_PHYSICAL_PAGE_CAPACITY];

bool IsVsmEvictionCandidateGreater(VsmEvictionCandidate lhs, VsmEvictionCandidate rhs)
{
    if (lhs.EvictionAgeKey != rhs.EvictionAgeKey)
        return lhs.EvictionAgeKey > rhs.EvictionAgeKey;

    return lhs.PhysicalPageIndex < rhs.PhysicalPageIndex;
}

void CompareAndSwapVsmEvictionCandidates(uint index, uint partnerIndex, bool ascending)
{
    VsmEvictionCandidate lhs = s_EvictionCandidates[index];
    VsmEvictionCandidate rhs = s_EvictionCandidates[partnerIndex];
    bool shouldSwap = ascending
        ? IsVsmEvictionCandidateGreater(lhs, rhs)
        : IsVsmEvictionCandidateGreater(rhs, lhs);

    if (shouldSwap)
    {
        s_EvictionCandidates[index] = rhs;
        s_EvictionCandidates[partnerIndex] = lhs;
    }
}

[shader("compute")]
[numthreads(VSM_EVICTION_SORT_THREAD_COUNT, 1, 1)]
void sortEvictionCandidates(uint groupThreadId : SV_GroupIndex)
{
    uint candidateCount = min(g_VsmPageManagementCounters.Load(VSM_EVICTION_CANDIDATE_COUNT_OFFSET), VSM_PHYSICAL_PAGE_CAPACITY);
    if (candidateCount <= 1u)
        return;

    uint sortCount = 1u << uint(firstbithigh(candidateCount - 1u) + 1);

    for (uint index = groupThreadId; index < sortCount; index += VSM_EVICTION_SORT_THREAD_COUNT)
    {
        VsmEvictionCandidate candidate;
        if (index < candidateCount)
        {
            candidate = g_VsmEvictionCandidates[index];
        }
        else
        {
            candidate.PhysicalPageIndex = VSM_INVALID_PHYSICAL_PAGE;
            candidate.EvictionAgeKey = 0u;
        }

        s_EvictionCandidates[index] = candidate;
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint sequenceSize = 2u; sequenceSize <= sortCount; sequenceSize <<= 1u)
    {
        for (uint comparisonDistance = sequenceSize >> 1u; comparisonDistance > 0u; comparisonDistance >>= 1u)
        {
            uint pairCount = sortCount >> 1u;
            for (uint pairIndex = groupThreadId; pairIndex < pairCount; pairIndex += VSM_EVICTION_SORT_THREAD_COUNT)
            {
                uint pairOffset = pairIndex & (comparisonDistance - 1u);
                uint packedBlockBase = pairIndex & ~(comparisonDistance - 1u);
                uint index = (packedBlockBase << 1u) | pairOffset;
                uint partnerIndex = index + comparisonDistance;
                bool ascending = (index & sequenceSize) != 0u;
                CompareAndSwapVsmEvictionCandidates(index, partnerIndex, ascending);
            }

            GroupMemoryBarrierWithGroupSync();
        }
    }

    for (uint index = groupThreadId; index < candidateCount; index += VSM_EVICTION_SORT_THREAD_COUNT)
        g_VsmEvictionCandidates[index] = s_EvictionCandidates[index];
}

#ifndef __WAVE_UTIL_HLSLi__
#define __WAVE_UTIL_HLSLi__
/**
 * High-performance scalar wavefront atomic adder (fixed-size version)
 * @param Dest           Destination variable (usually a global counter)
 * @param bCondition     Whether the current lane meets the add condition (e.g. bVisible)
 * @param ValuePerThread Fixed increment per lane (usually 1)
 * @param OutOffset      [out] Unique base offset assigned to each lane that meets the condition
 */
#define WaveInterlockedAddScalar(Dest, bCondition, ValuePerThread, OutOffset) \
{ \
    /* Count how many lanes in the wave satisfy the condition (bCondition == true) */ \
    /* Hardware reads the active mask and counts set bits; very fast */ \
    uint _WaveCount = WaveActiveCountBits(bCondition); \
    \
    uint _WaveBaseOffset = 0; \
    \
    /* First lane performs one atomic on behalf of the wave */ \
    if (WaveIsFirstLane()) \
    { \
        if (_WaveCount > 0) \
        { \
            InterlockedAdd(Dest, _WaveCount * (ValuePerThread), _WaveBaseOffset); \
        } \
    } \
    \
    /* Broadcast the global base offset to all lanes in the wave */ \
    _WaveBaseOffset = WaveReadLaneFirst(_WaveBaseOffset); \
    \
    /* Compute this lane's rank among lanes that satisfy the condition (0, 1, 2...) */ \
    /* WavePrefixCountBits(bCondition) is equivalent to a prefix sum but uses bit ops */ \
    uint _LaneOffset = WavePrefixCountBits(bCondition); \
    \
    /* Final offset = global base offset + (rank * fixed increment) */ \
    OutOffset = _WaveBaseOffset + (_LaneOffset * (ValuePerThread)); \
}

/**
 * Wave atomic adder
 * @param Dest          Destination variable (usually groupshared or RWBuffer)
 * @param ValueToAdd    Value to add from this lane (usually 1 in Nanite)
 * @param OutOffset     [out] Unique base offset assigned to this lane
 */
#define WaveInterlockedAdd(Dest, ValueToAdd, OutOffset) \
{ \
    /* Sum the values to add across all active lanes in the wave */ \
    uint _WaveTotal = WaveActiveSum(ValueToAdd); \
    uint _WaveBaseOffset; \
    \
    /* First lane performs one atomic on behalf of the wave */ \
    /* This turns contention from 32/64 lanes into one lane */ \
    if (WaveIsFirstLane()) \
    { \
        InterlockedAdd(Dest, _WaveTotal, _WaveBaseOffset); \
    } \
    \
    /* Broadcast the base offset obtained by the first lane to all other lanes */ \
    _WaveBaseOffset = WaveReadLaneFirst(_WaveBaseOffset); \
    \
    /* Compute this lane's relative offset within the wave */ \
    /* WavePrefixSum computes the sum of ValueToAdd for all prior lanes */ \
    uint _LaneOffset = WavePrefixSum(ValueToAdd); \
    \
    /* Final offset = global base offset + wave-relative offset */ \
    OutOffset = _WaveBaseOffset + _LaneOffset; \
}

#endif


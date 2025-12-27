#ifndef __WAVE_UTIL_HLSLi__
#define __WAVE_UTIL_HLSLi__
/**
 * 高性能标量波前原子累加器 (定长版本)
 * @param Dest           目标变量 (通常是全局计数器)
 * @param bCondition     当前线程是否满足增加条件 (例如: bVisible)
 * @param ValuePerThread 每个线程固定增加的数值 (通常为 1)
 * @param OutOffset      [输出] 每个满足条件的线程分配到的唯一起始偏移量
 */
#define WaveInterlockedAddScalar(Dest, bCondition, ValuePerThread, OutOffset) \
{ \
    /* 1. 统计当前 Wave 中有多少个线程满足条件 (bCondition == true) */ \
    /* 硬件会直接读取活跃掩码并统计位 1 的个数，速度极快 */ \
    uint _WaveCount = WaveActiveCountBits(bCondition); \
    \
    uint _WaveBaseOffset = 0; \
    \
    /* 2. 由 Wave 内的第一个线程代表整个 Wave 执行一次原子操作 */ \
    if (WaveIsFirstLane()) \
    { \
        if (_WaveCount > 0) \
        { \
            InterlockedAdd(Dest, _WaveCount * (ValuePerThread), _WaveBaseOffset); \
        } \
    } \
    \
    /* 3. 将全局起始偏移量广播给 Wave 内的所有线程 */ \
    _WaveBaseOffset = WaveReadLaneFirst(_WaveBaseOffset); \
    \
    /* 4. 计算当前线程在满足条件的线程中的“排名” (0, 1, 2...) */ \
    /* WavePrefixCountBits(bCondition) 等同于执行前缀和，但它是基于位操作的 */ \
    uint _LaneOffset = WavePrefixCountBits(bCondition); \
    \
    /* 5. 最终偏移量 = 全局起始偏移量 + (排名 * 固定增量) */ \
    OutOffset = _WaveBaseOffset + (_LaneOffset * (ValuePerThread)); \
}

/**
 * Wave原子累加器
 * @param Dest          目标变量 (通常是 groupshared 或 RWBuffer)
 * @param ValueToAdd    当前线程想要增加的值 (在 Nanite 中通常是 1)
 * @param OutOffset     [输出] 当前线程分配到的唯一起始偏移量
 */
#define WaveInterlockedAdd(Dest, ValueToAdd, OutOffset) \
{ \
    /* 计算当前整个 Wave 内所有活跃线程想要增加的总和 */ \
    uint _WaveTotal = WaveActiveSum(ValueToAdd); \
    uint _WaveBaseOffset; \
    \
    /* 由 Wave 内的第一个线程代表整个 Wave 执行一次原子操作 */ \
    /* 这样 32/64 个线程的竞争就变成了 1 个线程的竞争 */ \
    if (WaveIsFirstLane()) \
    { \
        InterlockedAdd(Dest, _WaveTotal, _WaveBaseOffset); \
    } \
    \
    /* 将第一个线程拿到的全局起始偏移量广播给 Wave 内的所有其他线程 */ \
    _WaveBaseOffset = WaveReadLaneFirst(_WaveBaseOffset); \
    \
    /* 计算当前线程在 Wave 内部的相对偏移量 */ \
    /* WavePrefixSum 会计算当前位之前的所有线程的 ValueToAdd 之和 */ \
    uint _LaneOffset = WavePrefixSum(ValueToAdd); \
    \
    /* 最终偏移量 = 全局起始偏移量 + 波内相对偏移量 */ \
    OutOffset = _WaveBaseOffset + _LaneOffset; \
}

#endif


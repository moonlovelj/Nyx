#pragma once

#include "RenderGraph.h"

#include <d3d12.h>
#include <optional>

namespace RenderGraph
{
    struct TransientMemoryStats
    {
        uint64_t AllocatedBytes = 0;
        uint64_t PeakAllocatedBytes = 0;
        uint32_t CachedLayoutCount = 0;
        uint32_t PendingLayoutCount = 0;
        uint32_t LayoutCreationCount = 0;
        uint32_t LayoutReuseCount = 0;
    };

    // Pure translation helpers used by the D3D12 executor and its unit tests.
    // Undefined has no native state and returns std::nullopt.
    std::optional<D3D12_RESOURCE_STATES> TryGetD3D12ResourceState(ResourceState state);

    // D3D12 permits combinations of read-only states.  This accepts an exact
    // state or a read-only actual state that is a superset of the declaration.
    bool IsD3D12ResourceStateCompatible(
        D3D12_RESOURCE_STATES actual,
        D3D12_RESOURCE_STATES declared);

    // Runtime telemetry and an optional cache trim point. In-flight layouts are
    // never destroyed by TrimTransientMemory().
    TransientMemoryStats GetTransientMemoryStats();
    void TrimTransientMemory();
} // namespace RenderGraph

#pragma once

#include "Renderer.h"
#include "StructsIO.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>

namespace DrawCommandManager
{
    void Cleanup();
    void FinalizeIndirectCommands();
    void AddDrawItem(const DrawItem& item);
    GpuBuffer& GetPotentialDrawItemsGPU();
    GpuBuffer& GetInstanceCulledDrawGPU();
    StructuredBuffer& GetTaskQueueStateGPU();
    ByteAddressBuffer& GetTaskQueueGPU();
    StructuredBuffer& GetVisibleMeshletBufferGPU();
    StructuredBuffer& GetIndirectDispatchMeshGPU();
    ByteAddressBuffer& GetMeshletBatchGPU();
    ByteAddressBuffer& GetCandidateMeshletGPU();
    uint32_t GetNumPotentialDrawItems();
} // namespace DrawCommandManager

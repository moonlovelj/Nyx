#pragma once
#include "../Core/GpuBuffer.h"
#include "Renderer.h"
#include "Model.h"
#include "MeshletStructs.h"

namespace GeometryStreaming
{
	extern StructuredBuffer m_HierarchyNodesGPU;
	extern std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	extern StructuredBuffer m_GroupDataLocationGPU;
	extern std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;
	extern ByteAddressBuffer m_GeometryStreamingRequestMaskGPU;

	void Initialize(const std::vector<Renderer::HierarchyNode>& nodes, uint32_t maxGroupSize, uint32_t numPages);
	void PinRootPages(Model* model);
	void Shutdown();

	void Update(uint32_t frameIndex); 
	void EnqueueAsyncLoad(uint32_t pageIdx);
	void OnPageIOComplete(uint32_t pageIdx, std::vector<uint8_t>&& pageData);
	void SyncMemoryAndAddressTable(uint32_t frameIndex);
	void ImmediateEvict(uint32_t currentFrame, Renderer::GroupDataLocation* pLocationTable);
}

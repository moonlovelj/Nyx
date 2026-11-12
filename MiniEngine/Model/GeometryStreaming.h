#pragma once
#include "../Core/GpuBuffer.h"
#include "Renderer.h"
#include "Model.h"
#include "MeshletStructs.h"

namespace GeometryStreaming
{
	extern StructuredBuffer m_HierarchyNodesGPU;
    extern StructuredBuffer m_GroupNodeIndicesGPU;
	extern std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	extern StructuredBuffer m_GroupDataLocationGPU;
	extern std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;
	extern ByteAddressBuffer m_GeometryStreamingRequestMaskGPU;

	bool Initialize(const Model& model);
	bool PinRootPages(Model* model);
	void Shutdown();

	void Update(uint32_t frameIndex); 
	// Called when the main view records a fresh visibility cull.
	void NotifyCullFrame();
	uint32_t GetUploadVersion();
}

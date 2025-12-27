#pragma once
#include "../Core/GpuBuffer.h"
#include "Renderer.h"
#include "MeshletStructs.h"

namespace GeometryStreaming
{
	extern StructuredBuffer m_HierarchyNodesGPU;
	extern std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	extern UploadBuffer m_GroupDataLocationCPU;
	extern StructuredBuffer m_GroupDataLocationGPU;
	extern std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;

	void Initialize(const std::vector<Renderer::HierarchyNode>& nodes, uint32_t maxGroupSize);
	// 非流送情况下，加载所有几何数据到 GPU
	void LoadAllGeometries(
		const std::wstring& filePath,
		uint64_t geometryBlobOffsetInFile,
		const std::vector<Renderer::PageMetadata>& pages,
		const std::vector <Renderer::GroupMetadata>& groups);
	void Shutdown();


}

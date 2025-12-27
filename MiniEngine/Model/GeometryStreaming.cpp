#include "GeometryStreaming.h"
#include "../Core/GpuBuffer.h"
#include "Model.h"
#include "Renderer.h"

namespace GeometryStreaming
{
	uint32_t kMaxChunks = 1;
	StructuredBuffer m_HierarchyNodesGPU;
	std::vector<ByteAddressBuffer> m_GeometryChunksGPU;
	UploadBuffer m_GroupDataLocationCPU;
	StructuredBuffer m_GroupDataLocationGPU;
	std::vector<Renderer::GroupDataLocation> m_GroupDataLocations;
}

void GeometryStreaming::Initialize(const std::vector<Renderer::HierarchyNode>& nodes, uint32_t maxGroupSize)
{
	for (uint32_t i = 0; i < kMaxChunks; ++i)
	{
		ByteAddressBuffer chunk;
		chunk.Create(L"Geometry Chunk ", Renderer::kChunkSizeInBytes / 4, 4);
		m_GeometryChunksGPU.push_back(std::move(chunk));
		Renderer::SetBindlessResourceDescriptor(
			SRV_GEOMETRY_CHUNK_DATA_BUFFER,
			m_GeometryChunksGPU[i].GetSRV()
		);
	}

	uint32_t numNodes = static_cast<uint32_t>(nodes.size());
	m_HierarchyNodesGPU.Create(L"Hierarchy Nodes GPU", numNodes, sizeof(Renderer::HierarchyNode), nodes.data());

	Renderer::SetBindlessResourceDescriptor(
		SRV_HIERARCHY_NODES_BUFFER,
		m_HierarchyNodesGPU.GetSRV()
	);

	uint32_t groupCount = std::max(1u, maxGroupSize);
	m_GroupDataLocationCPU.Create(L"Group Data Location CPU", groupCount * sizeof(Renderer::GroupDataLocation));
	m_GroupDataLocationGPU.Create(L"Group Data Location GPU", groupCount, sizeof(Renderer::GroupDataLocation), nullptr);
	Renderer::SetBindlessResourceDescriptor(
		SRV_GROUP_DATA_LOCATION_BUFFER,
		m_GroupDataLocationGPU.GetSRV()
	);

	m_GroupDataLocations.resize(groupCount, { INVALID_CHUNK_INDEX , 0});
}

void GeometryStreaming::LoadAllGeometries(
	const std::wstring& filePath,
	uint64_t geometryBlobOffsetInFile,
	const std::vector<Renderer::PageMetadata>& pages,
	const std::vector <Renderer::GroupMetadata>& groups)
{
	ASSERT(Renderer::kPageSizeInBytes * pages.size() <= Renderer::kChunkSizeInBytes, "Geometry blob exceeds chunk size.");

	if (pages.empty() || groups.empty())
		return;

	std::ifstream fs(filePath, std::ios::in | std::ios::binary);
	if (!fs)
	{
		Utility::Print("GeometryStreaming: failed to open file.\n");
		return;
	}

	fs.seekg(geometryBlobOffsetInFile, std::ios::beg);
	std::vector<uint8_t> readBuffer(Renderer::kPageSizeInBytes * pages.size());
	fs.read(reinterpret_cast<char*>(readBuffer.data()), readBuffer.size());
	if (fs.fail())
	{
		Utility::Printf("GeometryStreaming: failed to read geometry blob from file.\n");
		const std::streamsize got = fs.gcount();
		Utility::Printf(
			"GeometryStreaming: read failed. eof=%d bad=%d fail=%d, expected=%lld, got=%lld\n",
			fs.eof() ? 1 : 0,
			fs.bad() ? 1 : 0,
			fs.fail() ? 1 : 0,
			static_cast<uint64_t>(readBuffer.size()),
			static_cast<uint64_t>(got)
		);

		if (errno != 0)
		{
			char errBuf[256] = {};
			::strerror_s(errBuf, sizeof(errBuf), errno);
			Utility::Printf("errno=%d, msg=%s\n", errno, errBuf);
		}
		return;
	}

	Renderer::GroupDataLocation* groupDataLocationsCPU = (Renderer::GroupDataLocation*)m_GroupDataLocationCPU.Map();
	const uint32_t chunkIndex = 0;
	for (uint32_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex)
	{
		const Renderer::PageMetadata& page = pages[pageIndex];
		const uint32_t startGroupIndex = page.StartGroupIndex;
		const uint32_t groupCount = page.GroupCount;
		for (uint32_t groupOffset = 0; groupOffset < groupCount; ++groupOffset)
		{
			const uint32_t groupIndex = startGroupIndex + groupOffset;
			const Renderer::GroupMetadata& groupMeta = groups[groupIndex];

			// 更新组数据位置
			groupDataLocationsCPU[groupIndex].ChunkIndex = chunkIndex;
			groupDataLocationsCPU[groupIndex].ByteOffset = Renderer::kPageSizeInBytes * pageIndex + groupMeta.OffsetInPage;
		}
	}
	m_GroupDataLocationCPU.Unmap();

	GraphicsContext& gfx = GraphicsContext::Begin(L"GeometryStreaming::LoadAllGeometries");
	gfx.WriteBuffer(
		m_GeometryChunksGPU[chunkIndex],
		0,
		readBuffer.data(),
		readBuffer.size()
	);

	gfx.TransitionResource(m_GeometryChunksGPU[chunkIndex], D3D12_RESOURCE_STATE_GENERIC_READ);

	gfx.CopyBuffer(
		m_GroupDataLocationGPU,
		m_GroupDataLocationCPU
	);
	
	gfx.TransitionResource(
		m_GroupDataLocationGPU,
		D3D12_RESOURCE_STATE_GENERIC_READ
	);

	gfx.Finish(true);
}

void GeometryStreaming::Shutdown()
{
	m_HierarchyNodesGPU.Destroy();
	for (auto& chunk : m_GeometryChunksGPU)
	{
		chunk.Destroy();
	}
	m_GeometryChunksGPU.clear();

	m_GroupDataLocationCPU.Destroy();
	m_GroupDataLocationGPU.Destroy();
}

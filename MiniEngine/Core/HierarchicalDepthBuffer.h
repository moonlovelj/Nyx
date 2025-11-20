#pragma once

#include "PixelBuffer.h"
#include "Color.h"
#include "GpuBuffer.h"

class EsramAllocator;
class DepthBuffer;

class HierarchicalDepthBuffer : public PixelBuffer
{
public:
	HierarchicalDepthBuffer()
		: m_NumMipMaps(0), m_ClearColor(0.0f, 0.0f, 0.0f, 0.0f)
	{
		m_SRVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		for (int i = 0; i < _countof(m_UAVHandle); ++i)
			m_UAVHandle[i].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
	}

	// Create a color buffer.  If an address is supplied, memory will not be allocated.
	// The vmem address allows you to alias buffers (which can be especially useful for
	// reusing ESRAM across a frame.)
	// 注意这里的Width和Height是原始深度缓冲区的尺寸，而不是HZB的尺寸。HZB的尺寸和Mip数量在内部自动计算
	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height,
		DXGI_FORMAT Format, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	// Create a color buffer.  Memory will be allocated in ESRAM (on Xbox One).  On Windows,
	// this functions the same as Create() without a video address.
	// 注意这里的Width和Height是原始深度缓冲区的尺寸，而不是HZB的尺寸。HZB的尺寸和Mip数量在内部自动计算
	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height,
		DXGI_FORMAT Format, EsramAllocator& Allocator);

	// Get pre-created CPU-visible descriptor handles
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV(void) const { return m_SRVHandle; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV(void) const { return m_UAVHandle[0]; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV(uint32_t MipLevel) const { return m_UAVHandle[MipLevel]; }

	// 这里真正生成HZB的数据
	void GenerateHZB(CommandContext& Context, const DepthBuffer& SrcDepthBuffer);

protected:

	D3D12_RESOURCE_FLAGS CombineResourceFlags(void) const
	{
		return D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}

	void CreateTextureResourceInternal(ID3D12Device* Device, const std::wstring& Name,
		const D3D12_RESOURCE_DESC& ResourceDesc, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	void CreateDerivedViews(ID3D12Device* Device, DXGI_FORMAT Format, uint32_t NumMips);

	Color m_ClearColor;
	D3D12_CPU_DESCRIPTOR_HANDLE m_SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_UAVHandle[12];
	uint32_t m_NumMipMaps; // number of texture sublevels
};

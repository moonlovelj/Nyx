#include "pch.h"
#include "HierarchicalDepthBuffer.h"
#include "GraphicsCommon.h"
#include "CommandContext.h"
#include "EsramAllocator.h"
#include "DepthBuffer.h"

using namespace Graphics;

void HierarchicalDepthBuffer::CreateTextureResourceInternal(ID3D12Device* Device, const std::wstring& Name,
	const D3D12_RESOURCE_DESC& ResourceDesc, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr)
{
	Destroy();

	(void)VidMemPtr;

	{
		CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		ASSERT_SUCCEEDED(Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
			&ResourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, MY_IID_PPV_ARGS(&m_pResource)));
	}

	m_UsageState = D3D12_RESOURCE_STATE_COMMON;
	m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;

#ifndef RELEASE
	m_pResource->SetName(Name.c_str());
#else
	(Name);
#endif
}

void HierarchicalDepthBuffer::CreateDerivedViews(ID3D12Device* Device, DXGI_FORMAT Format, uint32_t NumMips)
{
	m_NumMipMaps = NumMips;

	D3D12_RENDER_TARGET_VIEW_DESC RTVDesc = {};
	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};

	UAVDesc.Format = GetUAVFormat(Format);
	SRVDesc.Format = Format;
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	{
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		UAVDesc.Texture2D.MipSlice = 0;

		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MipLevels = NumMips;
		SRVDesc.Texture2D.MostDetailedMip = 0;
	}

	if (m_SRVHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		m_SRVHandle = Graphics::AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	ID3D12Resource* Resource = m_pResource.Get();

	// Create the shader resource view
	Device->CreateShaderResourceView(Resource, &SRVDesc, m_SRVHandle);

	// Create the UAVs for each mip level (RWTexture2D)
	for (uint32_t i = 0; i < NumMips; ++i)
	{
		if (m_UAVHandle[i].ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			m_UAVHandle[i] = Graphics::AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		Device->CreateUnorderedAccessView(Resource, nullptr, &UAVDesc, m_UAVHandle[i]);

		UAVDesc.Texture2D.MipSlice++;
	}
}

void HierarchicalDepthBuffer::Create(const std::wstring& Name, uint32_t Width, uint32_t Height,
	DXGI_FORMAT Format, D3D12_GPU_VIRTUAL_ADDRESS VidMem)
{
	const uint32_t HZBWidth = std::max(Math::AlignPowerOfTwo(Width) >> 1, 1u);
	const uint32_t HZBHeight = std::max(Math::AlignPowerOfTwo(Height) >> 1, 1u);
	const int32_t NumMips = Math::Log2(std::max(HZBWidth, HZBHeight)) + 1;

	D3D12_RESOURCE_FLAGS Flags = CombineResourceFlags();
	D3D12_RESOURCE_DESC ResourceDesc = DescribeTex2D(HZBWidth, HZBHeight, 1, NumMips, Format, Flags);

	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.SampleDesc.Quality = 0;

	D3D12_CLEAR_VALUE ClearValue = {};
	ClearValue.Format = Format;
	ClearValue.Color[0] = m_ClearColor.R();
	ClearValue.Color[1] = m_ClearColor.G();
	ClearValue.Color[2] = m_ClearColor.B();
	ClearValue.Color[3] = m_ClearColor.A();

	CreateTextureResourceInternal(Graphics::g_Device, Name, ResourceDesc, VidMem);
	CreateDerivedViews(Graphics::g_Device, Format, NumMips);
}

void HierarchicalDepthBuffer::Create(const std::wstring& Name, uint32_t Width, uint32_t Height,
	DXGI_FORMAT Format, EsramAllocator&)
{
	Create(Name, Width, Height, Format);
}

void HierarchicalDepthBuffer::GenerateHZB(CommandContext& BaseContext, const DepthBuffer& SrcDepthBuffer)
{
	const uint32_t HZBWidth = std::max(Math::AlignPowerOfTwo(SrcDepthBuffer.GetWidth()) >> 1, 1u);
	const uint32_t HZBHeight = std::max(Math::AlignPowerOfTwo(SrcDepthBuffer.GetHeight()) >> 1, 1u);
	ASSERT(HZBWidth == m_Width && HZBHeight == m_Height,
		"Hierarchical depth buffer size does not match source depth buffer size.");

	ScopedTimer _prof(L"GenerateHZB", BaseContext);

	ComputeContext& Context = BaseContext.GetComputeContext();
	Context.SetRootSignature(Graphics::g_CommonRS);
	Context.TransitionResource(*this, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	Context.SetDynamicDescriptor(1, 0, SrcDepthBuffer.GetDepthSRV());

	uint32_t LastLevelWidth = SrcDepthBuffer.GetWidth();
	uint32_t LastLevelHeight = SrcDepthBuffer.GetHeight();

	for (uint32_t TopMip = 0; TopMip < m_NumMipMaps; )
	{
		const uint32_t DstWidth = std::max(m_Width >> TopMip, 1u);
		const uint32_t DstHeight = std::max(m_Height >> TopMip, 1u);

		Context.SetPipelineState(Graphics::g_GenerateHZBPSO);

		uint32_t NumMips;
		_BitScanReverse((unsigned long*)&NumMips, DstWidth | DstHeight);
		NumMips = std::min(NumMips + 1u, 4u);

		if (TopMip > 0)
		{
			Context.SetDynamicDescriptor(1, 0, m_SRVHandle);
			Context.SetConstants(0, TopMip-1, NumMips, LastLevelWidth, LastLevelHeight);
		}
		else
		{
			Context.SetConstants(0, TopMip, NumMips, LastLevelWidth, LastLevelHeight);
		}

		Context.SetDynamicDescriptors(2, 0, NumMips, m_UAVHandle + TopMip);
		Context.Dispatch2D(DstWidth, DstHeight);
		Context.InsertUAVBarrier(*this);
		LastLevelWidth = DstWidth;
		LastLevelHeight = DstHeight;
		TopMip += NumMips;
	}

	Context.TransitionResource(*this, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

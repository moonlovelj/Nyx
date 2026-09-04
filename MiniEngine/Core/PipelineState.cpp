//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard

#include "pch.h"
#include "GraphicsCore.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "Hash.h"
#include <map>
#include <thread>
#include <mutex>

using Math::IsAligned;
using namespace Graphics;
using Microsoft::WRL::ComPtr;
using namespace std;

static map< size_t, ComPtr<ID3D12PipelineState> > s_GraphicsPSOHashMap;
static map< size_t, ComPtr<ID3D12PipelineState> > s_ComputePSOHashMap;
static map< size_t, ComPtr<ID3D12PipelineState> > s_MeshShaderPSOHashMap;

void PSO::DestroyAll(void)
{
    s_GraphicsPSOHashMap.clear();
    s_ComputePSOHashMap.clear();
	s_MeshShaderPSOHashMap.clear();
}


GraphicsPSO::GraphicsPSO(const wchar_t* Name)
    : PSO(Name)
{
    ZeroMemory(&m_PSODesc, sizeof(m_PSODesc));
    m_PSODesc.NodeMask = 1;
    m_PSODesc.SampleMask = 0xFFFFFFFFu;
    m_PSODesc.SampleDesc.Count = 1;
    m_PSODesc.InputLayout.NumElements = 0;
}

void GraphicsPSO::SetBlendState( const D3D12_BLEND_DESC& BlendDesc )
{
    m_PSODesc.BlendState = BlendDesc;
}

void GraphicsPSO::SetRasterizerState( const D3D12_RASTERIZER_DESC& RasterizerDesc )
{
    m_PSODesc.RasterizerState = RasterizerDesc;
}

void GraphicsPSO::SetDepthStencilState( const D3D12_DEPTH_STENCIL_DESC& DepthStencilDesc )
{
    m_PSODesc.DepthStencilState = DepthStencilDesc;
}

void GraphicsPSO::SetSampleMask( UINT SampleMask )
{
    m_PSODesc.SampleMask = SampleMask;
}

void GraphicsPSO::SetPrimitiveTopologyType( D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType )
{
    ASSERT(TopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, "Can't draw with undefined topology");
    m_PSODesc.PrimitiveTopologyType = TopologyType;
}

void GraphicsPSO::SetPrimitiveRestart( D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBProps )
{
    m_PSODesc.IBStripCutValue = IBProps;
}

void GraphicsPSO::SetDepthTargetFormat(DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality )
{
    SetRenderTargetFormats(0, nullptr, DSVFormat, MsaaCount, MsaaQuality );
}

void GraphicsPSO::SetRenderTargetFormat( DXGI_FORMAT RTVFormat, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality )
{
    SetRenderTargetFormats(1, &RTVFormat, DSVFormat, MsaaCount, MsaaQuality );
}

void GraphicsPSO::SetRenderTargetFormats( UINT NumRTVs, const DXGI_FORMAT* RTVFormats, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality )
{
    ASSERT(NumRTVs == 0 || RTVFormats != nullptr, "Null format array conflicts with non-zero length");
    for (UINT i = 0; i < NumRTVs; ++i)
    {
        ASSERT(RTVFormats[i] != DXGI_FORMAT_UNKNOWN);
        m_PSODesc.RTVFormats[i] = RTVFormats[i];
    }
    for (UINT i = NumRTVs; i < m_PSODesc.NumRenderTargets; ++i)
        m_PSODesc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    m_PSODesc.NumRenderTargets = NumRTVs;
    m_PSODesc.DSVFormat = DSVFormat;
    m_PSODesc.SampleDesc.Count = MsaaCount;
    m_PSODesc.SampleDesc.Quality = MsaaQuality;
}

void GraphicsPSO::SetInputLayout( UINT NumElements, const D3D12_INPUT_ELEMENT_DESC* pInputElementDescs )
{
    m_PSODesc.InputLayout.NumElements = NumElements;

    if (NumElements > 0)
    {
        D3D12_INPUT_ELEMENT_DESC* NewElements = (D3D12_INPUT_ELEMENT_DESC*)malloc(sizeof(D3D12_INPUT_ELEMENT_DESC) * NumElements);
        memcpy(NewElements, pInputElementDescs, NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC));
        m_InputLayouts.reset((const D3D12_INPUT_ELEMENT_DESC*)NewElements);
    }
    else
        m_InputLayouts = nullptr;
}

void GraphicsPSO::Finalize()
{
    // Make sure the root signature is finalized first
    m_PSODesc.pRootSignature = m_RootSignature->GetSignature();
    ASSERT(m_PSODesc.pRootSignature != nullptr);

    m_PSODesc.InputLayout.pInputElementDescs = nullptr;
    size_t HashCode = Utility::HashState(&m_PSODesc);
    HashCode = Utility::HashState(m_InputLayouts.get(), m_PSODesc.InputLayout.NumElements, HashCode);
    m_PSODesc.InputLayout.pInputElementDescs = m_InputLayouts.get();

    ID3D12PipelineState** PSORef = nullptr;
    bool firstCompile = false;
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_GraphicsPSOHashMap.find(HashCode);

        // Reserve space so the next inquiry will find that someone got here first.
        if (iter == s_GraphicsPSOHashMap.end())
        {
            firstCompile = true;
            PSORef = s_GraphicsPSOHashMap[HashCode].GetAddressOf();
        }
        else
            PSORef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {
        ASSERT(m_PSODesc.DepthStencilState.DepthEnable != (m_PSODesc.DSVFormat == DXGI_FORMAT_UNKNOWN));
        ASSERT_SUCCEEDED( g_Device->CreateGraphicsPipelineState(&m_PSODesc, MY_IID_PPV_ARGS(&m_PSO)) );
        s_GraphicsPSOHashMap[HashCode].Attach(m_PSO);
        m_PSO->SetName(m_Name);
    }
    else
    {
        while (*PSORef == nullptr)
            this_thread::yield();
        m_PSO = *PSORef;
    }
}

void ComputePSO::Finalize()
{
    // Make sure the root signature is finalized first
    m_PSODesc.pRootSignature = m_RootSignature->GetSignature();
    ASSERT(m_PSODesc.pRootSignature != nullptr);

    size_t HashCode = Utility::HashState(&m_PSODesc);

    ID3D12PipelineState** PSORef = nullptr;
    bool firstCompile = false;
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_ComputePSOHashMap.find(HashCode);

        // Reserve space so the next inquiry will find that someone got here first.
        if (iter == s_ComputePSOHashMap.end())
        {
            firstCompile = true;
            PSORef = s_ComputePSOHashMap[HashCode].GetAddressOf();
        }
        else
            PSORef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {
        ASSERT_SUCCEEDED( g_Device->CreateComputePipelineState(&m_PSODesc, MY_IID_PPV_ARGS(&m_PSO)) );
        s_ComputePSOHashMap[HashCode].Attach(m_PSO);
        m_PSO->SetName(m_Name);
    }
    else
    {
        while (*PSORef == nullptr)
            this_thread::yield();
        m_PSO = *PSORef;
    }
}

ComputePSO::ComputePSO(const wchar_t* Name)
    : PSO(Name)
{
    ZeroMemory(&m_PSODesc, sizeof(m_PSODesc));
    m_PSODesc.NodeMask = 1;
}


MeshShaderPSO::MeshShaderPSO(LPCWSTR Name) : PSO(Name)
{
	m_RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	m_DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	m_BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	m_SampleMask = UINT_MAX;
	m_PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	m_RTVFormats = {}; // NumRenderTargets = 0
	m_DSVFormat = DXGI_FORMAT_UNKNOWN;
	m_SampleDesc = { 1, 0 };

	m_MS = { nullptr, 0 };
	m_PS = { nullptr, 0 };
	m_AS = { nullptr, 0 };
}

void MeshShaderPSO::SetBlendState(const D3D12_BLEND_DESC& BlendDesc)
{
	m_BlendState = BlendDesc;
}

void MeshShaderPSO::SetRasterizerState(const D3D12_RASTERIZER_DESC& RasterizerDesc)
{
	m_RasterizerState = RasterizerDesc;
}

void MeshShaderPSO::SetDepthStencilState(const D3D12_DEPTH_STENCIL_DESC& DepthStencilDesc)
{
	m_DepthStencilState = DepthStencilDesc;
}

void MeshShaderPSO::SetSampleMask(UINT SampleMask)
{
	m_SampleMask = SampleMask;
}

void MeshShaderPSO::SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType)
{
	m_PrimitiveTopologyType = TopologyType;
}

void MeshShaderPSO::SetRenderTargetFormat(DXGI_FORMAT RTVFormat, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality)
{
	SetRenderTargetFormats(1, &RTVFormat, DSVFormat, MsaaCount, MsaaQuality);
}

void MeshShaderPSO::SetRenderTargetFormats(UINT NumRTVs, const DXGI_FORMAT* RTVFormats, DXGI_FORMAT DSVFormat, UINT MsaaCount, UINT MsaaQuality)
{
	ASSERT(NumRTVs <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
	m_RTVFormats.NumRenderTargets = NumRTVs;
	for (UINT i = 0; i < NumRTVs; ++i)
	{
		m_RTVFormats.RTFormats[i] = RTVFormats[i];
	}
	for (UINT i = NumRTVs; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
	{
		m_RTVFormats.RTFormats[i] = DXGI_FORMAT_UNKNOWN;
	}
	m_DSVFormat = DSVFormat;
	m_SampleDesc.Count = MsaaCount;
	m_SampleDesc.Quality = MsaaQuality;
}

void MeshShaderPSO::SetMeshShader(const void* Binary, size_t Size)
{
	m_MS = { Binary, Size };
}

void MeshShaderPSO::SetPixelShader(const void* Binary, size_t Size)
{
	m_PS = { Binary, Size };
}

void MeshShaderPSO::SetAmplificationShader(const void* Binary, size_t Size)
{
	m_AS = { Binary, Size };
}

void MeshShaderPSO::Finalize()
{
	ASSERT(m_RootSignature != nullptr, "Root Signature not set for MeshShaderPSO");
	ASSERT(m_MS.pShaderBytecode != nullptr, "Mesh Shader not set");

	struct MeshShaderPsoStream
	{
		CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
        CD3DX12_PIPELINE_STATE_STREAM_AS pAS;
		CD3DX12_PIPELINE_STATE_STREAM_MS pMS;
		CD3DX12_PIPELINE_STATE_STREAM_PS pPS;
		CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER pRasterizer;
		CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL pDepthStencil;
		CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC pBlend;
		CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK pSampleMask;
		CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY pTopology;
		CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
		CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
		CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC SampleDesc;
	} stream;

	stream.pRootSignature = m_RootSignature->GetSignature();
	stream.pAS = m_AS;
	stream.pMS = m_MS;
	stream.pPS = m_PS;
	stream.pRasterizer = CD3DX12_RASTERIZER_DESC(m_RasterizerState);
	stream.pDepthStencil = CD3DX12_DEPTH_STENCIL_DESC(m_DepthStencilState);
	stream.pBlend = CD3DX12_BLEND_DESC(m_BlendState);
	stream.pSampleMask = m_SampleMask;
	stream.pTopology = m_PrimitiveTopologyType;
	stream.RTVFormats = m_RTVFormats;
	stream.DSVFormat = m_DSVFormat;
	stream.SampleDesc = m_SampleDesc;

	D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
	streamDesc.SizeInBytes = sizeof(stream);
	streamDesc.pPipelineStateSubobjectStream = &stream;

    size_t HashCode = Utility::HashState(&stream);

    ID3D12PipelineState** PSORef = nullptr;
    bool firstCompile = false;
    {
        static mutex s_HashMapMutex;
        lock_guard<mutex> CS(s_HashMapMutex);
        auto iter = s_MeshShaderPSOHashMap.find(HashCode);

        // Reserve space so the next inquiry will find that someone got here first.
        if (iter == s_MeshShaderPSOHashMap.end())
        {
            firstCompile = true;
            PSORef = s_MeshShaderPSOHashMap[HashCode].GetAddressOf();
        }
        else
            PSORef = iter->second.GetAddressOf();
    }

    if (firstCompile)
    {
        ASSERT_SUCCEEDED(g_Device->CreatePipelineState(&streamDesc, MY_IID_PPV_ARGS(&m_PSO)));
        s_MeshShaderPSOHashMap[HashCode].Attach(m_PSO);
        m_PSO->SetName(m_Name);
    }
    else
    {
        while (*PSORef == nullptr)
            this_thread::yield();
        m_PSO = *PSORef;
    }
}

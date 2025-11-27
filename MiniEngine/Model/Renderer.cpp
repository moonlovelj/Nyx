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
// Author:   James Stanard
//

#include "Renderer.h"
#include "Model.h"
#include "TextureManager.h"
#include "ConstantBuffers.h"
#include "IBL.h"
#include "LightManager.h"
#include "../Core/RootSignature.h"
#include "../Core/PipelineState.h"
#include "../Core/GraphicsCommon.h"
#include "../Core/BufferManager.h"
#include "../Core/ShadowCamera.h"

#include "GPUDriven/ExecuteIndirect.h"
#include "GPUDriven/CommandBucketer.h"

#include "CompiledShaders/DefaultVS.h"
#include "CompiledShaders/DefaultSkinVS.h"
#include "CompiledShaders/DefaultPS.h"
#include "CompiledShaders/DefaultNoUV1VS.h"
#include "CompiledShaders/DefaultNoUV1SkinVS.h"
#include "CompiledShaders/DefaultNoUV1PS.h"
#include "CompiledShaders/DefaultNoTangentVS.h"
#include "CompiledShaders/DefaultNoTangentSkinVS.h"
#include "CompiledShaders/DefaultNoTangentPS.h"
#include "CompiledShaders/DefaultNoTangentNoUV1VS.h"
#include "CompiledShaders/DefaultNoTangentNoUV1SkinVS.h"
#include "CompiledShaders/DefaultNoTangentNoUV1PS.h"
#include "CompiledShaders/DepthOnlyVS.h"
#include "CompiledShaders/DepthOnlySkinVS.h"
#include "CompiledShaders/CutoutDepthVS.h"
#include "CompiledShaders/CutoutDepthSkinVS.h"
#include "CompiledShaders/CutoutDepthPS.h"
#include "CompiledShaders/SkyboxVS.h"
#include "CompiledShaders/SkyboxPS.h"
#include "CompiledShaders/GBufferPS.h"
#include "CompiledShaders/GBufferNoUV1PS.h"
#include "CompiledShaders/GBufferNoTangentPS.h"
#include "CompiledShaders/GBufferNoTangentNoUV1PS.h"
#include "CompiledShaders/FrustumCullCS.h"
#include "CompiledShaders/FillCullingResultCS.h"

#pragma warning(disable:4319) // '~': zero extending 'uint32_t' to 'uint64_t' of greater size

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer
{
    BoolVar SeparateZPass("Renderer/Separate Z Pass", true);
    BoolVar DeferredRendering("Renderer/Deferred Rendering", true);
    BoolVar UseCull("Renderer/Use Cull", true);
	BoolVar UseGPUFrustumCull("Renderer/Use GPU Frustum Cull", true);
    
	const char* ViewModeLabels[] = { "Lit", "MeshletLOD", "MeshletID", "MeshletTriangle"};
	EnumVar ViewMode("View/View Mode", 0, _countof(ViewModeLabels), ViewModeLabels);

    bool s_Initialized = false;

    DescriptorHeap s_TextureHeap;
    DescriptorHeap s_SamplerHeap;
    std::vector<GraphicsPSO> sm_PSOs;

    TextureRef s_RadianceCubeMap;
    TextureRef s_IrradianceCubeMap;
    float s_SpecularIBLRange;
    float s_SpecularIBLBias;
    uint32_t g_SSAOFullScreenID;
    uint32_t g_ShadowBufferID;

    RootSignature m_RootSig;
    GraphicsPSO m_SkyboxPSO(L"Renderer: Skybox PSO");
    GraphicsPSO m_DefaultPSO(L"Renderer: Default PSO"); // Not finalized.  Used as a template.

    DescriptorHandle m_CommonTextures;
    DescriptorHandle m_GPUDrivenBuffers;
	DescriptorHandle m_CommonUAVs;
	DescriptorHandle m_BindlessSRVs;
	DescriptorHandle m_BindlessUAVs;

    UploadBuffer m_IndirectArgsCounterBufferReset;
	ComputePSO m_FrustrumCullPSO(L"Renderer: Frustum Cull PSO");
	ComputePSO m_FillCullingResultPSO(L"Renderer: Fill Culling Result PSO");
}

void Renderer::Initialize(void)
{
    if (s_Initialized)
        return;

    SamplerDesc DefaultSamplerDesc;
    DefaultSamplerDesc.MaxAnisotropy = 8;

    SamplerDesc CubeMapSamplerDesc = DefaultSamplerDesc;
    //CubeMapSamplerDesc.MaxLOD = 6.0f;

    SamplerDesc LinearSamplerDesc;
    LinearSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    LinearSamplerDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    SamplerDesc PointSamplerDesc;
    PointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    PointSamplerDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    m_RootSig.Reset(kNumRootBindings, 5);
    m_RootSig.InitStaticSampler(10, DefaultSamplerDesc);
    m_RootSig.InitStaticSampler(11, SamplerShadowDesc);
    m_RootSig.InitStaticSampler(12, CubeMapSamplerDesc);
    m_RootSig.InitStaticSampler(13, LinearSamplerDesc);
    m_RootSig.InitStaticSampler(14, PointSamplerDesc);
    m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
    m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL);
    m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 20);
	m_RootSig[kCommonCBV].InitAsConstantBuffer(1);
	m_RootSig[kRootConstants].InitAsConstants(2, 4);
	m_RootSig[kRootConstants1].InitAsConstants(3, 12);
	m_RootSig[kViewModeConstants].InitAsConstants(4, 4);
	m_RootSig[kGPUDrivenSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 10);
    m_RootSig[kCommonSRV].InitAsBufferSRV(30);
	m_RootSig[kCommonUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 10);
    m_RootSig[kCommonUAV].InitAsBufferUAV(10);
    m_RootSig.Finalize(L"RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT 
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneDepthBuffer.GetFormat();

    //D3D12_INPUT_ELEMENT_DESC posOnly[] =
    //{
    //    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //};

    //D3D12_INPUT_ELEMENT_DESC posAndUV[] =
    //{
    //    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //};

    //D3D12_INPUT_ELEMENT_DESC skinPos[] =
    //{
    //    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //};

    //D3D12_INPUT_ELEMENT_DESC skinPosAndUV[] =
    //{
    //    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //    { "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    //};

    ASSERT(sm_PSOs.size() == 0);

    // Depth Only PSOs

    GraphicsPSO DepthOnlyPSO(L"Renderer: Depth Only PSO");
    DepthOnlyPSO.SetRootSignature(m_RootSig);
    DepthOnlyPSO.SetRasterizerState(RasterizerDefault);
    DepthOnlyPSO.SetBlendState(BlendDisable);
    DepthOnlyPSO.SetDepthStencilState(DepthStateReadWrite);
	//DepthOnlyPSO.SetInputLayout(_countof(posOnly), posOnly);
	DepthOnlyPSO.SetInputLayout(0, nullptr);
    DepthOnlyPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    DepthOnlyPSO.SetRenderTargetFormats(0, nullptr, DepthFormat);
    DepthOnlyPSO.SetVertexShader(g_pDepthOnlyVS, sizeof(g_pDepthOnlyVS));
    DepthOnlyPSO.Finalize();
    sm_PSOs.push_back(DepthOnlyPSO);

    GraphicsPSO CutoutDepthPSO(L"Renderer: Cutout Depth PSO");
    CutoutDepthPSO = DepthOnlyPSO;
    CutoutDepthPSO.SetInputLayout(0, nullptr);
    CutoutDepthPSO.SetRasterizerState(RasterizerTwoSided);
    CutoutDepthPSO.SetVertexShader(g_pCutoutDepthVS, sizeof(g_pCutoutDepthVS));
    CutoutDepthPSO.SetPixelShader(g_pCutoutDepthPS, sizeof(g_pCutoutDepthPS));
    CutoutDepthPSO.Finalize();
    sm_PSOs.push_back(CutoutDepthPSO);

    GraphicsPSO SkinDepthOnlyPSO = DepthOnlyPSO;
    SkinDepthOnlyPSO.SetInputLayout(0, nullptr);
    SkinDepthOnlyPSO.SetVertexShader(g_pDepthOnlySkinVS, sizeof(g_pDepthOnlySkinVS));
    SkinDepthOnlyPSO.Finalize();
    sm_PSOs.push_back(SkinDepthOnlyPSO);

    GraphicsPSO SkinCutoutDepthPSO = CutoutDepthPSO;
    SkinCutoutDepthPSO.SetInputLayout(0, nullptr);
    SkinCutoutDepthPSO.SetVertexShader(g_pCutoutDepthSkinVS, sizeof(g_pCutoutDepthSkinVS));
    SkinCutoutDepthPSO.Finalize();
    sm_PSOs.push_back(SkinCutoutDepthPSO);

    ASSERT(sm_PSOs.size() == 4);

    // Shadow PSOs

    DepthOnlyPSO.SetRasterizerState(RasterizerShadow);
    DepthOnlyPSO.SetRenderTargetFormats(0, nullptr, g_ShadowBuffer.GetFormat());
    DepthOnlyPSO.Finalize();
    sm_PSOs.push_back(DepthOnlyPSO);

    CutoutDepthPSO.SetRasterizerState(RasterizerShadowTwoSided);
    CutoutDepthPSO.SetRenderTargetFormats(0, nullptr, g_ShadowBuffer.GetFormat());
    CutoutDepthPSO.Finalize();
    sm_PSOs.push_back(CutoutDepthPSO);

    SkinDepthOnlyPSO.SetRasterizerState(RasterizerShadow);
    SkinDepthOnlyPSO.SetRenderTargetFormats(0, nullptr, g_ShadowBuffer.GetFormat());
    SkinDepthOnlyPSO.Finalize();
    sm_PSOs.push_back(SkinDepthOnlyPSO);

    SkinCutoutDepthPSO.SetRasterizerState(RasterizerShadowTwoSided);
    SkinCutoutDepthPSO.SetRenderTargetFormats(0, nullptr, g_ShadowBuffer.GetFormat());
    SkinCutoutDepthPSO.Finalize();
    sm_PSOs.push_back(SkinCutoutDepthPSO);

    ASSERT(sm_PSOs.size() == 8);

    // Default PSO

    m_DefaultPSO.SetRootSignature(m_RootSig);
    m_DefaultPSO.SetRasterizerState(RasterizerDefault);
    m_DefaultPSO.SetBlendState(BlendDisable);
    m_DefaultPSO.SetDepthStencilState(DepthStateReadWrite);
    m_DefaultPSO.SetInputLayout(0, nullptr);
    m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_DefaultPSO.SetRenderTargetFormats(1, &ColorFormat, DepthFormat);
    m_DefaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
    m_DefaultPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));

    // Skybox PSO

    m_SkyboxPSO = m_DefaultPSO;
    m_SkyboxPSO.SetDepthStencilState(DepthStateReadOnly);
    m_SkyboxPSO.SetInputLayout(0, nullptr);
    m_SkyboxPSO.SetVertexShader(g_pSkyboxVS, sizeof(g_pSkyboxVS));
    m_SkyboxPSO.SetPixelShader(g_pSkyboxPS, sizeof(g_pSkyboxPS));
    m_SkyboxPSO.Finalize();

    TextureManager::Initialize(L"");

    s_TextureHeap.Create(L"Scene Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);

    // Maybe only need 2 for wrap vs. clamp?  Currently we allocate 1 for 1 with textures
    s_SamplerHeap.Create(L"Scene Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

    Lighting::InitializeResources();

    // Allocate a descriptor table for the common textures
    m_CommonTextures = s_TextureHeap.Alloc(20);

    uint32_t DestCount = 9;
    uint32_t SourceCounts[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };

    D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
    {
        GetDefaultTexture(kBlackCubeMap),
        GetDefaultTexture(kBlackCubeMap),
        GetDefaultTexture(kBlackOpaque2D),
        g_SSAOFullScreen.GetSRV(),
        g_ShadowBuffer.GetSRV(),
        Lighting::m_LightBuffer.GetSRV(),
        Lighting::m_LightShadowArray.GetSRV(),
        Lighting::m_LightGrid.GetSRV(),
        Lighting::m_LightGridBitMask.GetSRV(),
    };

    g_Device->CopyDescriptors(1, &m_CommonTextures, &DestCount, DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	m_BindlessSRVs = s_TextureHeap.Alloc(256);
	m_BindlessUAVs = s_TextureHeap.Alloc(256);
    m_CommonUAVs = s_TextureHeap.Alloc(10);

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();

    GPUDriven::Initialize(&m_RootSig);

    m_IndirectArgsCounterBufferReset.Create(L"IndirectArgsCounterBufferReset", sizeof(uint32_t));
    uint32_t* pCounterBufferReset = (uint32_t*)m_IndirectArgsCounterBufferReset.Map();
    pCounterBufferReset[0] = 0;
    m_IndirectArgsCounterBufferReset.Unmap();

    m_GPUDrivenBuffers = s_TextureHeap.Alloc(10);

    m_FrustrumCullPSO.SetRootSignature(m_RootSig);
    m_FrustrumCullPSO.SetComputeShader(g_pFrustumCullCS, sizeof(g_pFrustumCullCS));
    m_FrustrumCullPSO.Finalize();

	m_FillCullingResultPSO.SetRootSignature(m_RootSig);
    m_FillCullingResultPSO.SetComputeShader(g_pFillCullingResultCS, sizeof(g_pFillCullingResultCS));
    m_FillCullingResultPSO.Finalize();

    s_Initialized = true;
}

void Renderer::UpdateGlobalDescriptors(void)
{
    if (g_SSAOFullScreenID == g_SSAOFullScreen.GetVersionID() &&
        g_ShadowBufferID == g_ShadowBuffer.GetVersionID())
    {
        return;
    }

    uint32_t DestCount = 2;
    uint32_t SourceCounts[] = { 1, 1 };

    D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
    {
        g_SSAOFullScreen.GetSRV(),
        g_ShadowBuffer.GetSRV(),
    };

    DescriptorHandle dest = m_CommonTextures + 3 * s_TextureHeap.GetDescriptorSize();

    g_Device->CopyDescriptors(1, &dest, &DestCount, DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();

}

void Renderer::SetIBLTextures()
{
    //s_RadianceCubeMap = specularIBL;
    //s_IrradianceCubeMap = diffuseIBL;

    //s_SpecularIBLRange = 0.0f;
    //if (s_RadianceCubeMap.IsValid())
    //{
    //    ID3D12Resource* texRes = const_cast<ID3D12Resource*>(s_RadianceCubeMap.Get()->GetResource());
    //    const D3D12_RESOURCE_DESC& texDesc = texRes->GetDesc();
    //    s_SpecularIBLRange = Max(0.0f, (float)texDesc.MipLevels - 1);
    //    s_SpecularIBLBias = Min(s_SpecularIBLBias, s_SpecularIBLRange);
    //}

    //uint32_t DestCount = 2;
    //uint32_t SourceCounts[] = { 1, 1 };

    //D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
    //{
    //    specularIBL.IsValid() ? specularIBL.GetSRV() : GetDefaultTexture(kBlackCubeMap),
    //    diffuseIBL.IsValid() ? diffuseIBL.GetSRV() : GetDefaultTexture(kBlackCubeMap)
    //};

    //g_Device->CopyDescriptors(1, &m_CommonTextures, &DestCount, DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    uint32_t DestCount = 3;
    uint32_t SourceCounts[] = { 1, 1, 1 };
    if (IBL::IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
        {
            Graphics::g_IBLDiffuseLDMap.GetSRV(),
            Graphics::g_IBLSpecularLDMap.GetSRV(),
            Graphics::g_IBLLut.GetSRV()
        };

        g_Device->CopyDescriptors(1, &m_CommonTextures, &DestCount, DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else
    {
        D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
        {
            Graphics::GetDefaultTexture(Graphics::kBlackCubeMap),
            Graphics::GetDefaultTexture(Graphics::kBlackCubeMap),
            Graphics::GetDefaultTexture(Graphics::kBlackTransparent2D)
        };

        g_Device->CopyDescriptors(1, &m_CommonTextures, &DestCount, DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

void Renderer::SetIBLBias(float LODBias)
{
    s_SpecularIBLBias = Min(LODBias, s_SpecularIBLRange);
}

void Renderer::Shutdown(void)
{
    s_RadianceCubeMap = nullptr;
    s_IrradianceCubeMap = nullptr;
    TextureManager::Shutdown();
    s_TextureHeap.Destroy();
    s_SamplerHeap.Destroy();
    m_IndirectArgsCounterBufferReset.Destroy();

    GPUDriven::Shutdown();
}

uint8_t Renderer::GetPSO(uint16_t psoFlags)
{
    using namespace PSOFlags;

    GraphicsPSO ColorPSO = m_DefaultPSO;

    uint16_t Requirements = kHasPosition | kHasNormal;
    ASSERT((psoFlags & Requirements) == Requirements);

    //std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
    //if (psoFlags & kHasPosition)
    //    vertexLayout.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT});
    //if (psoFlags & kHasNormal)
    //    vertexLayout.push_back({"NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT});
    //if (psoFlags & kHasTangent)
    //    vertexLayout.push_back({"TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT});
    //if (psoFlags & kHasUV0)
    //    vertexLayout.push_back({"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT});
    //else
    //    vertexLayout.push_back({"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       1, D3D12_APPEND_ALIGNED_ELEMENT});
    //if (psoFlags & kHasUV1)
    //    vertexLayout.push_back({"TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT});
    //if (psoFlags & kHasSkin)
    //{
    //    vertexLayout.push_back({ "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
    //    vertexLayout.push_back({ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
    //}

	//ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());
	ColorPSO.SetInputLayout(0, nullptr);

    if (psoFlags & kHasSkin)
    {
        if (psoFlags & kHasTangent)
        {
            if (psoFlags & kHasUV1)
            {
                ColorPSO.SetVertexShader(g_pDefaultSkinVS, sizeof(g_pDefaultSkinVS));
                ColorPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));
            }
            else
            {
                ColorPSO.SetVertexShader(g_pDefaultNoUV1SkinVS, sizeof(g_pDefaultNoUV1SkinVS));
                ColorPSO.SetPixelShader(g_pDefaultNoUV1PS, sizeof(g_pDefaultNoUV1PS));
            }
        }
        else
        {
            if (psoFlags & kHasUV1)
            {
                ColorPSO.SetVertexShader(g_pDefaultNoTangentSkinVS, sizeof(g_pDefaultNoTangentSkinVS));
                ColorPSO.SetPixelShader(g_pDefaultNoTangentPS, sizeof(g_pDefaultNoTangentPS));
            }
            else
            {
                ColorPSO.SetVertexShader(g_pDefaultNoTangentNoUV1SkinVS, sizeof(g_pDefaultNoTangentNoUV1SkinVS));
                ColorPSO.SetPixelShader(g_pDefaultNoTangentNoUV1PS, sizeof(g_pDefaultNoTangentNoUV1PS));
            }
        }
    }
    else
    {
        if (psoFlags & kHasTangent)
        {
            if (psoFlags & kHasUV1)
            {
                ColorPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
                ColorPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));
            }
            else
            {
                ColorPSO.SetVertexShader(g_pDefaultNoUV1VS, sizeof(g_pDefaultNoUV1VS));
                ColorPSO.SetPixelShader(g_pDefaultNoUV1PS, sizeof(g_pDefaultNoUV1PS));
            }
        }
        else
        {
            if (psoFlags & kHasUV1)
            {
                ColorPSO.SetVertexShader(g_pDefaultNoTangentVS, sizeof(g_pDefaultNoTangentVS));
                ColorPSO.SetPixelShader(g_pDefaultNoTangentPS, sizeof(g_pDefaultNoTangentPS));
            }
            else
            {
                ColorPSO.SetVertexShader(g_pDefaultNoTangentNoUV1VS, sizeof(g_pDefaultNoTangentNoUV1VS));
                ColorPSO.SetPixelShader(g_pDefaultNoTangentNoUV1PS, sizeof(g_pDefaultNoTangentNoUV1PS));
            }
        }
    }

    if (DeferredRendering)
    {
        if (!(psoFlags & kAlphaBlend))
        {
            if (psoFlags & kHasTangent)
            {
                if (psoFlags & kHasUV1)
                {
                    ColorPSO.SetPixelShader(g_pGBufferPS, sizeof(g_pGBufferPS));
                }
                else
                {
                    ColorPSO.SetPixelShader(g_pGBufferNoUV1PS, sizeof(g_pGBufferNoUV1PS));
                }
            }
            else
            {
                if (psoFlags & kHasUV1)
                {
                    ColorPSO.SetPixelShader(g_pGBufferNoTangentPS, sizeof(g_pGBufferNoTangentPS));
                }
                else
                {
                    ColorPSO.SetPixelShader(g_pGBufferNoTangentNoUV1PS, sizeof(g_pGBufferNoTangentNoUV1PS));
                }
            }

            DXGI_FORMAT RTVFormats[] = {
				g_SceneColorBuffer.GetFormat(),
                g_GBufferA.GetFormat(),
                g_GBufferB.GetFormat(),
                g_GBufferC.GetFormat(),
                g_GBufferD.GetFormat()
            };
            ColorPSO.SetRenderTargetFormats(5, RTVFormats, g_SceneDepthBuffer.GetFormat());
        }
    }

    if (psoFlags & kAlphaBlend)
    {
        ColorPSO.SetBlendState(BlendTraditional);
        ColorPSO.SetDepthStencilState(DepthStateReadOnly);
    }
    if (psoFlags & kTwoSided)
    {
        ColorPSO.SetRasterizerState(RasterizerTwoSided);
    }
    ColorPSO.Finalize();

    // Look for an existing PSO
    for (uint32_t i = 0; i < sm_PSOs.size(); ++i)
    {
        if (ColorPSO.GetPipelineStateObject() == sm_PSOs[i].GetPipelineStateObject())
        {
            return (uint8_t)i;
        }
    }

    // If not found, keep the new one, and return its index
    sm_PSOs.push_back(ColorPSO);

    // The returned PSO index has read-write depth.  The index+1 tests for equal depth.
    ColorPSO.SetDepthStencilState(DepthStateTestEqual);
    ColorPSO.Finalize();
#ifdef DEBUG
    for (uint32_t i = 0; i < sm_PSOs.size(); ++i)
        ASSERT(ColorPSO.GetPipelineStateObject() != sm_PSOs[i].GetPipelineStateObject());
#endif
    sm_PSOs.push_back(ColorPSO);

    ASSERT(sm_PSOs.size() <= 64, "Ran out of room for unique PSOs");

    return (uint8_t)sm_PSOs.size() - 2;
}

void Renderer::DrawSkybox( GraphicsContext& gfxContext, const Camera& Camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor )
{
    ScopedTimer _prof(L"Draw Skybox", gfxContext);

    __declspec(align(16)) struct SkyboxVSCB
    {
        Matrix4 ProjInverse;
        Matrix3 ViewInverse;
    } skyVSCB;
    skyVSCB.ProjInverse = Invert(Camera.GetProjMatrix());
    skyVSCB.ViewInverse = Invert(Camera.GetViewMatrix()).Get3x3();

    __declspec(align(16)) struct SkyboxPSCB
    {
        float TextureLevel;
    } skyPSCB;
    skyPSCB.TextureLevel = s_SpecularIBLBias;

    gfxContext.SetRootSignature(m_RootSig);
    gfxContext.SetPipelineState(m_SkyboxPSO);

    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
    gfxContext.SetViewportAndScissor(viewport, scissor);

    gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    gfxContext.SetDynamicConstantBufferView(kMeshConstants, sizeof(SkyboxVSCB), &skyVSCB);
    gfxContext.SetDynamicConstantBufferView(kMaterialConstants, sizeof(SkyboxPSCB), &skyPSCB);
    gfxContext.SetDescriptorTable(kCommonSRVs, m_CommonTextures);
    gfxContext.SetDynamicDescriptor(kCommonSRVs, 0, IBL::IsValid() ? Graphics::g_IBLCubeMap.GetSRV() : GetDefaultTexture(kBlackOpaque2D));
    gfxContext.Draw(3);
}

void Renderer::FrustrumCulling(GraphicsContext& gfxContext, 
    const GlobalConstants& inGlobals, const BaseCamera* camera,
    const D3D12_VIEWPORT& viewport, IndirectArgsBuffer& inArgsBuffer,
    ByteAddressBuffer& outputVisibleBuffer, uint32_t startCommandOffset, 
    uint32_t maxCommands, uint16_t psoIdx)
{
    ScopedTimer _prof(L"Renderer::FrustrumCulling", gfxContext);

    ComputeContext& context = gfxContext.GetComputeContext();
	auto& bucketer = GPUDriven::CommandBucketer::Get();

    context.TransitionResource(outputVisibleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource(inArgsBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	context.SetRootSignature(m_RootSig);
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetPipelineState(m_FrustrumCullPSO);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
	context.SetDescriptorTable(kGPUDrivenSRVs, m_GPUDrivenBuffers);
    context.SetBufferSRV(kCommonSRV, inArgsBuffer);
	context.SetDescriptorTable(kCommonUAVs, m_CommonUAVs);

    float screenErrorConstant = 1.f;
	auto* cameraProj = static_cast<const Camera*>(camera);
    if (cameraProj)
    {
        // (cotHalfFov * screenHeight) / 2.0;
		float cotHalfFov = 1.0f / std::tanf(0.5f * cameraProj->GetFOV());
        screenErrorConstant = cotHalfFov * viewport.Height * 0.5f;
    }

	uint32_t BindlessSRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(m_BindlessSRVs);
	uint32_t BindlessUAVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(m_BindlessUAVs);

	context.SetConstants(kRootConstants1, startCommandOffset, maxCommands, 
        screenErrorConstant, bucketer.GetPsoIdxToContinuousIdx(psoIdx));
	context.SetConstant(kRootConstants1, 4, (uint32_t)BindlessSRVsOffsets::kArgsVisibleFlagsBufferSRV + BindlessSRVDescriptorTable);
	context.SetConstant(kRootConstants1, 5, (uint32_t)BindlessSRVsOffsets::kCullingResultArgsBufferSRV + BindlessSRVDescriptorTable);
	context.SetConstant(kRootConstants1, 6, (uint32_t)BindlessUAVsOffsets::kArgsVisibleFlagsBufferUAV + BindlessUAVDescriptorTable);
	context.SetConstant(kRootConstants1, 7, (uint32_t)BindlessUAVsOffsets::kCullingResultArgsBufferUAV + BindlessUAVDescriptorTable);

	context.Dispatch1D(maxCommands);
}

void Renderer::FillCullingResult(GraphicsContext& gfxContext, const GlobalConstants& inGlobals, 
    IndirectArgsBuffer& inArgsBuffer, ByteAddressBuffer& visibleBuffer, StructuredBuffer& ResultBuffer,
    uint32_t startCommandOffset, uint32_t maxCommands, uint16_t psoIdx, CullingStage cullingStage)
{
	ScopedTimer _prof(L"Renderer::FillCullingResult", gfxContext);
	ComputeContext& context = gfxContext.GetComputeContext();
	auto& bucketer = GPUDriven::CommandBucketer::Get();

	context.TransitionResource(ResultBuffer.GetCounterBuffer(), D3D12_RESOURCE_STATE_COPY_DEST);
    context.CopyBufferRegion(ResultBuffer.GetCounterBuffer(), 0, m_IndirectArgsCounterBufferReset, 0, sizeof(UINT));
    context.TransitionResource(visibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context.TransitionResource(inArgsBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context.TransitionResource(ResultBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	context.SetRootSignature(m_RootSig);
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetPipelineState(m_FillCullingResultPSO);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
	context.SetDescriptorTable(kGPUDrivenSRVs, m_GPUDrivenBuffers);
	context.SetBufferSRV(kCommonSRV, inArgsBuffer);
	context.SetDescriptorTable(kCommonUAVs, m_CommonUAVs);

	uint32_t BindlessSRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(m_BindlessSRVs);
	uint32_t BindlessUAVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(m_BindlessUAVs);

	context.SetConstants(kRootConstants1, startCommandOffset, maxCommands,
		0.f, bucketer.GetPsoIdxToContinuousIdx(psoIdx));
	context.SetConstant(kRootConstants1, 4, (uint32_t)BindlessSRVsOffsets::kArgsVisibleFlagsBufferSRV + BindlessSRVDescriptorTable);
	context.SetConstant(kRootConstants1, 5, (uint32_t)BindlessSRVsOffsets::kCullingResultArgsBufferSRV + BindlessSRVDescriptorTable);
	context.SetConstant(kRootConstants1, 6, (uint32_t)BindlessUAVsOffsets::kArgsVisibleFlagsBufferUAV + BindlessUAVDescriptorTable);
	context.SetConstant(kRootConstants1, 7, (uint32_t)BindlessUAVsOffsets::kCullingResultArgsBufferUAV + BindlessUAVDescriptorTable);
    context.SetConstant(kRootConstants1, 8, (uint32_t)cullingStage);

    context.Dispatch1D(maxCommands);

	context.TransitionResource(ResultBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    context.TransitionResource(ResultBuffer.GetCounterBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

void MeshSorter::AddMesh( const Mesh& mesh, float distance,
    D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
    D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
    D3D12_GPU_VIRTUAL_ADDRESS bufferPtr,
    D3D12_GPU_VIRTUAL_ADDRESS meshJoints,
	const IndirectArgsBuffer& indirectArgsBuffer,
    uint32_t indirectArgsOffset)
{
    SortKey key;
    key.value = m_SortObjects.size();

	bool alphaBlend = (mesh.psoFlags & PSOFlags::kAlphaBlend) == PSOFlags::kAlphaBlend;
    bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;
    bool skinned = (mesh.psoFlags & PSOFlags::kHasSkin) == PSOFlags::kHasSkin;
    uint64_t depthPSO = (skinned ? 2 : 0) + (alphaTest ? 1 : 0);

    union float_or_int { float f; uint32_t u; } dist;
    dist.f = Max(distance, 0.0f);

	if (m_BatchType == kShadows)
	{
		if (alphaBlend)
			return;

		key.passID = kZPass;
		key.psoIdx = depthPSO + 4;
        key.key = dist.u;
		m_SortKeys.push_back(key.value);
		m_PassCounts[kZPass]++;
	}
    else if (mesh.psoFlags & PSOFlags::kAlphaBlend)
    {
        key.passID = kTransparent;
        key.psoIdx = mesh.pso;
        key.key = ~dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kTransparent]++;
    }
    else if (SeparateZPass || alphaTest)
    {
        key.passID = kZPass;
        key.psoIdx = depthPSO;
        key.key = dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kZPass]++;

        if (DeferredRendering)
        {
            key.passID = kGBuffer;
            key.psoIdx = mesh.pso + 1;
            key.key = dist.u;
            m_SortKeys.push_back(key.value);
            m_PassCounts[kGBuffer]++;
        }
        else
        {
            key.passID = kOpaque;
            key.psoIdx = mesh.pso + 1;
            key.key = dist.u;
            m_SortKeys.push_back(key.value);
            m_PassCounts[kOpaque]++;
        }
    }
    else
    {
        if (DeferredRendering)
        {
            key.passID = kGBuffer;
            key.psoIdx = mesh.pso;
            key.key = dist.u;
            m_SortKeys.push_back(key.value);
            m_PassCounts[kGBuffer]++;
        }
        else
        {
            key.passID = kOpaque;
            key.psoIdx = mesh.pso;
            key.key = dist.u;
            m_SortKeys.push_back(key.value);
            m_PassCounts[kOpaque]++;
        }
    }

    SortObject object = { &mesh, meshJoints, meshCBV, materialCBV, bufferPtr, indirectArgsBuffer, indirectArgsOffset};
    m_SortObjects.push_back(object);
}

void MeshSorter::Sort()
{
    struct { bool operator()(uint64_t a, uint64_t b) const { return a < b; } } Cmp;
    std::sort(m_SortKeys.begin(), m_SortKeys.end(), Cmp);
}

void MeshSorter::RenderMeshes(
    DrawPass pass,
    GraphicsContext& context,
    const GlobalConstants& inGlobals)
{
	ASSERT(m_DSV != nullptr);

	ScopedTimer _prof(L"MeshSorter::RenderMeshes", context);

	GlobalConstants globals = inGlobals;
	// Set common shader constants
	globals.ViewProjMatrix = m_Camera->GetViewProjMatrix();
    globals.ViewMatrix = m_Camera->GetViewMatrix();
	globals.ViewerPos = m_Camera->GetPosition();
    const Frustum& frustum = m_Camera->GetViewSpaceFrustum();

    //kNearPlane, kFarPlane, kLeftPlane, kRightPlane, kTopPlane, kBottomPlane

	globals.ViewSpaceFrustumPlanes[0] = frustum.GetFrustumPlane(Frustum::kNearPlane);
	globals.ViewSpaceFrustumPlanes[1] = frustum.GetFrustumPlane(Frustum::kFarPlane);
	globals.ViewSpaceFrustumPlanes[2] = frustum.GetFrustumPlane(Frustum::kLeftPlane);
	globals.ViewSpaceFrustumPlanes[3] = frustum.GetFrustumPlane(Frustum::kRightPlane);
	globals.ViewSpaceFrustumPlanes[4] = frustum.GetFrustumPlane(Frustum::kTopPlane);
	globals.ViewSpaceFrustumPlanes[5] = frustum.GetFrustumPlane(Frustum::kBottomPlane);

    Renderer::UpdateGlobalDescriptors();

	uint32_t DestCount = 5;
	uint32_t SourceCounts[] = { 1, 1, 1, 1, 1};
	D3D12_CPU_DESCRIPTOR_HANDLE SourceBuffers[] =
	{
        m_MeshletConstantsBufferSRV,
        m_MeshConstantsBufferSRV,
        m_MaterialConstantsBufferSRV,
        m_VertexBufferSRV,
        m_JointsBufferSRV
	};
	g_Device->CopyDescriptors(1, &m_GPUDrivenBuffers, &DestCount, DestCount, SourceBuffers, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);


    //context.SetRootSignature(m_RootSig);
    context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

	// Must set the Graphics / Compute root signature only * after * setting your descriptor heaps, as the correct heap pointers must be available when root signature is set.
    context.SetRootSignature(m_RootSig);
    // Set common textures
    context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);
    context.SetDescriptorTable(kGPUDrivenSRVs, m_GPUDrivenBuffers);
    context.SetConstants(kViewModeConstants, (uint32_t)ViewMode);

	if (m_BatchType == kShadows)
	{
		context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		context.ClearDepth(*m_DSV);
		context.SetDepthStencilTarget(m_DSV->GetDSV());

		if (m_Viewport.Width == 0)
		{
			m_Viewport.TopLeftX = 0.0f;
			m_Viewport.TopLeftY = 0.0f;
			m_Viewport.Width = (float)m_DSV->GetWidth();
			m_Viewport.Height = (float)m_DSV->GetHeight();
			m_Viewport.MaxDepth = 1.0f;
			m_Viewport.MinDepth = 0.0f;

			m_Scissor.left = 1;
			m_Scissor.right = m_DSV->GetWidth() - 2;
			m_Scissor.top = 1;
			m_Scissor.bottom = m_DSV->GetHeight() - 2;
		}
	}
	else
	{
		for (uint32_t i = 0; i < m_NumRTVs; ++i)
		{
			ASSERT(m_RTV[i] != nullptr);
			ASSERT(m_DSV->GetWidth() == m_RTV[i]->GetWidth());
			ASSERT(m_DSV->GetHeight() == m_RTV[i]->GetHeight());
		}

		if (m_Viewport.Width == 0)
		{
			m_Viewport.TopLeftX = 0.0f;
			m_Viewport.TopLeftY = 0.0f;
			m_Viewport.Width = (float)m_DSV->GetWidth();
			m_Viewport.Height = (float)m_DSV->GetHeight();
			m_Viewport.MaxDepth = 1.0f;
			m_Viewport.MinDepth = 0.0f;

			m_Scissor.left = 0;
			m_Scissor.right = m_DSV->GetWidth();
			m_Scissor.top = 0;
			m_Scissor.bottom = m_DSV->GetWidth();
		}
	}

    //for ( ; m_CurrentPass <= pass; m_CurrentPass = (DrawPass)(m_CurrentPass + 1))
    const uint32_t passCount = m_PassCounts[pass];
    if (passCount > 0)
    {
		if (m_BatchType == kDefault)
		{
			switch (pass)
			{
			case kZPass:
				context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				context.SetDepthStencilTarget(m_DSV->GetDSV());
				break;
			case kOpaque:
				if (SeparateZPass)
				{
					context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_READ);
					context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV_DepthReadOnly());
				}
				else
				{
					context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV());
				}
				break;
			case kGBuffer:
			    if (SeparateZPass)
			    {
			        context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_READ);
			        context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_RENDER_TARGET);

			        D3D12_CPU_DESCRIPTOR_HANDLE RTVs[] = {
			            g_SceneColorBuffer.GetRTV(),
			            g_GBufferA.GetRTV(),
			            g_GBufferB.GetRTV(),
			            g_GBufferC.GetRTV(),
			            g_GBufferD.GetRTV(),
			        };
			        context.SetRenderTargets(5, RTVs, m_DSV->GetDSV_DepthReadOnly());
			    }
			    else
			    {
			        context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			        context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        context.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_RENDER_TARGET);
			        D3D12_CPU_DESCRIPTOR_HANDLE RTVs[] = {
			            g_SceneColorBuffer.GetRTV(),
                        g_GBufferA.GetRTV(),
                        g_GBufferB.GetRTV(),
                        g_GBufferC.GetRTV(),
                        g_GBufferD.GetRTV(),
                    };
			        context.SetRenderTargets(5, RTVs, m_DSV->GetDSV());
			    }
			    break;
			case kTransparent:
				context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_READ);
				context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV_DepthReadOnly());
				break;
			}
		}

        context.SetViewportAndScissor(m_Viewport, m_Scissor);
        context.FlushResourceBarriers();

        context.SetIndexBuffer(m_IBV);
        
        const uint32_t lastDraw = m_CurrentDraw + passCount;

        while (m_CurrentDraw < lastDraw)
        {
            if (pass == kTransparent)
            {
				SortKey key;
				key.value = m_SortKeys[m_CurrentDraw];
				const SortObject& object = m_SortObjects[key.objectIdx];
				const Mesh& mesh = *object.mesh;

				context.SetPipelineState(sm_PSOs[key.psoIdx]);
				context.TransitionResource(const_cast<IndirectArgsBuffer&>(object.indirectArgsBuffer), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
				GPUDriven::DrawIndirect(context, const_cast<IndirectArgsBuffer&>(object.indirectArgsBuffer), mesh.numDraws, object.indirectArgsOffset);
                ++m_CurrentDraw;
            }
            else
            {
                auto& bucketer = GPUDriven::CommandBucketer::Get();
				if (pass == kZPass)
				{
                    if (m_BatchType == MeshSorter::kShadows)
                    {
                        if (bucketer.HasShadow())
                        {
							auto& args = bucketer.GetShadowArgsBuffer();
                            if (UseCull)
                            {
                                CullingStage cullingStage = CullingStage::kNoCulled;
                                if (UseGPUFrustumCull)
                                {
									cullingStage = CullingStage::kFrustrumCulled;
                                }

								for (const auto& run : bucketer.GetShadowRuns())
								{
									if (UseGPUFrustumCull)
									{
										FrustrumCulling(context, globals, m_Camera, m_Viewport, args, 
                                            bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
                                            (uint32_t)run.startCmd, run.count, run.psoIdx);
									}
								}

								for (const auto& run : bucketer.GetShadowRuns())
								{
									FillCullingResult(context, globals, args,
										bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
                                        bucketer.GetCullingResultArgsBuffer(run.psoIdx),
										(uint32_t)run.startCmd, run.count, run.psoIdx, cullingStage);
								}

								for (const auto& run : bucketer.GetShadowRuns())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, bucketer.GetCullingResultArgsBuffer(run.psoIdx), run.count, 0, &bucketer.GetCullingResultArgsBuffer(run.psoIdx).GetCounterBuffer(), 0);
								}
                            }
                            else
                            {
								for (const auto& run : bucketer.GetShadowRuns())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, args, run.count, (uint64_t)run.startCmd * sizeof(GPUDriven::IndirectCommand));
								}
                            }
                        }
                    }
                    else
                    {
                        if (bucketer.HasDepth())
                        {
							auto& args = bucketer.GetDepthArgsBuffer();
							if (UseCull)
							{
								CullingStage cullingStage = CullingStage::kNoCulled;
								if (UseGPUFrustumCull)
								{
									cullingStage = CullingStage::kFrustrumCulled;
								}

								for (const auto& run : bucketer.GetDepthRuns())
								{
									if (UseGPUFrustumCull)
									{
										FrustrumCulling(context, globals, m_Camera, m_Viewport, args,
											bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
											(uint32_t)run.startCmd, run.count, run.psoIdx);
									}
								}

								for (const auto& run : bucketer.GetDepthRuns())
								{
									FillCullingResult(context, globals, args,
										bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
										bucketer.GetCullingResultArgsBuffer(run.psoIdx),
										(uint32_t)run.startCmd, run.count, run.psoIdx, cullingStage);
								}

								for (const auto& run : bucketer.GetDepthRuns())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, bucketer.GetCullingResultArgsBuffer(run.psoIdx), run.count, 0, &bucketer.GetCullingResultArgsBuffer(run.psoIdx).GetCounterBuffer(), 0);
								}
							}
							else
							{
								for (const auto& run : bucketer.GetDepthRuns())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, args, run.count, (uint64_t)run.startCmd * sizeof(GPUDriven::IndirectCommand));
								}
							}
                        }
                    }	
				}
				else
				{
                    if (bucketer.HasColor())
                    {
                        auto& args = bucketer.GetColorArgsBuffer();

                        if (!SeparateZPass)
                        {
							if (UseCull)
							{
								CullingStage cullingStage = CullingStage::kNoCulled;
								if (UseGPUFrustumCull)
								{
									cullingStage = CullingStage::kFrustrumCulled;
								}

								for (const auto& run : bucketer.GetColorRunsRW())
								{
									if (UseGPUFrustumCull)
									{
										FrustrumCulling(context, globals, m_Camera, m_Viewport, args,
											bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
											(uint32_t)run.startCmd, run.count, run.psoIdx);
									}
								}

								for (const auto& run : bucketer.GetColorRunsRW())
								{
									FillCullingResult(context, globals, args,
										bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
										bucketer.GetCullingResultArgsBuffer(run.psoIdx),
										(uint32_t)run.startCmd, run.count, run.psoIdx, cullingStage);
								}

								for (const auto& run : bucketer.GetColorRunsRW())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, bucketer.GetCullingResultArgsBuffer(run.psoIdx), run.count, 0, &bucketer.GetCullingResultArgsBuffer(run.psoIdx).GetCounterBuffer(), 0);
								}
							}
							else
							{
								for (const auto& run : bucketer.GetColorRunsRW())
								{
									context.SetPipelineState(sm_PSOs[run.psoIdx]);
									GPUDriven::DrawIndirect(context, args, run.count, (uint64_t)run.startCmd * sizeof(GPUDriven::IndirectCommand));
								}
							}
                        }
                        
						if (UseCull)
						{
							CullingStage cullingStage = CullingStage::kNoCulled;
							if (UseGPUFrustumCull)
							{
								cullingStage = CullingStage::kFrustrumCulled;
							}

							for (const auto& run : bucketer.GetColorRunsEQ())
							{
								if (UseGPUFrustumCull)
								{
									FrustrumCulling(context, globals, m_Camera, m_Viewport, args,
										bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
										(uint32_t)run.startCmd, run.count, run.psoIdx);
								}
							}

							for (const auto& run : bucketer.GetColorRunsEQ())
							{
								FillCullingResult(context, globals, args,
									bucketer.GetArgsVisibleFlagsBuffer(run.psoIdx),
									bucketer.GetCullingResultArgsBuffer(run.psoIdx),
									(uint32_t)run.startCmd, run.count, run.psoIdx, cullingStage);
							}

							for (const auto& run : bucketer.GetColorRunsEQ())
							{
								context.SetPipelineState(sm_PSOs[run.psoIdx]);
								GPUDriven::DrawIndirect(context, bucketer.GetCullingResultArgsBuffer(run.psoIdx), run.count, 0, &bucketer.GetCullingResultArgsBuffer(run.psoIdx).GetCounterBuffer(), 0);
							}
						}
						else
						{
							for (const auto& run : bucketer.GetColorRunsEQ())
							{
								context.SetPipelineState(sm_PSOs[run.psoIdx]);
								GPUDriven::DrawIndirect(context, args, run.count, (uint64_t)run.startCmd * sizeof(GPUDriven::IndirectCommand));
							}
						}
                    }
				}

                m_CurrentDraw += passCount;
            } 
        }
    }

	if (m_BatchType == kShadows)
	{
		context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

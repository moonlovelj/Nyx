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
#include "../Core/TemporalEffects.h"

#include "CommandBucketer.h"
#include "GeometryStreaming.h"

#include "Shaders/SharedHeader.hlsli"


#include "CompiledShaders/SkyboxVS.h"
#include "CompiledShaders/SkyboxPS.h"
#include "CompiledShaders/UberMeshShader.h"
#include "CompiledShaders/UberMeshShaderPass0.h"
#include "CompiledShaders/UberMeshShaderPass1.h"
#include "CompiledShaders/MeshBufferGenCS.h"
#include "CompiledShaders/VBufferPS.h"
#include "CompiledShaders/ResolveVBufferToGBufferCS.h"
#include "CompiledShaders/InstanceCullPass0CS.h"
#include "CompiledShaders/InstanceCullPass1CS.h"
#include "CompiledShaders/DAGCullPass0CS.h"
#include "CompiledShaders/DAGCullPass1CS.h"
#include "CompiledShaders/ExportDepthVS.h"
#include "CompiledShaders/ExportDepthPS.h"
#include "CompiledShaders/InstanceCullDepthOnlyPassCS.h"
#include "CompiledShaders/DAGCullDepthOnlyPassCS.h"
#include "CompiledShaders/DepthOnlyPS.h"
#include "CompiledShaders/UberMeshShaderDepthOnlyPass.h"

#pragma warning(disable:4319) // '~': zero extending 'uint32_t' to 'uint64_t' of greater size

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer
{
	NumVar PixelErrorThreshold("Renderer/Pixel Error Threshold", 1.0f, 0.5f, 10.0f, 0.25f);
	BoolVar FreezeCull("Renderer/Freeze Cull", false);
    
	const char* ViewModeLabels[] = { "Lit", "MeshletLOD", "MeshletID", "MeshletTriangle", "MeshID", "InstanceID", "MaterialID"};
	EnumVar ViewMode("Visualize/View Mode", 0, _countof(ViewModeLabels), ViewModeLabels);

    bool s_Initialized = false;

    DescriptorHeap s_TextureHeap;
    DescriptorHeap s_SamplerHeap;

	CommandSignature GPUDrivenDrawIndirectCommandSignature;

    TextureRef s_RadianceCubeMap;
    TextureRef s_IrradianceCubeMap;
    float s_SpecularIBLRange;
    float s_SpecularIBLBias;
    uint32_t g_SSAOFullScreenID;
    uint32_t g_ShadowBufferID;

    RootSignature m_RootSig;
    GraphicsPSO m_SkyboxPSO(L"Renderer: Skybox PSO");
    GraphicsPSO m_DefaultPSO(L"Renderer: Default PSO"); // Not finalized.  Used as a template.

    DescriptorHandle m_BindlessResources;

	ComputePSO m_MeshBufferGenPSO(L"Renderer: Mesh Buffer Gen PSO");

    MeshShaderPSO m_UberMeshPSO[2] = {
        MeshShaderPSO(L"Renderer: Uber Mesh PSO Pass 0"),
        MeshShaderPSO(L"Renderer: Uber Mesh PSO Pass 1")
	};

	ComputePSO m_ResolveVBufferToGBufferPSO(L"Renderer: Resolve VBuffer To GBuffer PSO");

	ComputePSO m_InstanceCullPSO[2] = {
        ComputePSO(L"Renderer: Instance Cull Pass 0 PSO"),
        ComputePSO(L"Renderer: Instance Cull Pass 1 PSO")
	};

    ComputePSO m_DAGCullPSO[2] = {
        ComputePSO(L"Renderer: DAG Cull Pass 0 PSO"),
        ComputePSO(L"Renderer: DAG Cull Pass 1 PSO")
	};

    GraphicsPSO m_ExportDepthPSO(L"Renderer: Export Depth PSO");

    ComputePSO m_InstanceCullDepthOnlyPSO(L"Renderer: Instance Cull Depth Only PSO");
    ComputePSO m_DAGCullDepthOnlyPSO(L"Renderer: DAG Cull Depth Only PSO");
    MeshShaderPSO m_DepthOnlyPSO(L"Renderer: Depth Only PSO");
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
    m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
    m_RootSig[kMaterialConstants].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_ALL);
	m_RootSig[kCommonCBV].InitAsConstantBuffer(2);
	m_RootSig[kCommandConstants].InitAsConstants(3, 4);
	m_RootSig[kViewModeConstants].InitAsConstants(5, 4);
    m_RootSig[kStandbyCBV].InitAsConstantBuffer(6);
    m_RootSig.Finalize(L"RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT 
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneDepthBuffer.GetFormat();

    // Mesh shader PSO
    m_UberMeshPSO[0].SetRootSignature(m_RootSig);
	m_UberMeshPSO[0].SetRasterizerState(RasterizerTwoSided);
    m_UberMeshPSO[0].SetDepthStencilState(DepthStateDisabled);
	m_UberMeshPSO[0].SetBlendState(BlendDisable);
    m_UberMeshPSO[0].SetRenderTargetFormats(0, {}, g_SceneDepthBuffer.GetFormat());
	m_UberMeshPSO[0].SetMeshShader(g_pUberMeshShaderPass0, sizeof(g_pUberMeshShaderPass0));
	m_UberMeshPSO[0].SetPixelShader(g_pVBufferPS, sizeof(g_pVBufferPS));
	m_UberMeshPSO[0].Finalize();

    m_UberMeshPSO[1] = m_UberMeshPSO[0];
    m_UberMeshPSO[1].SetMeshShader(g_pUberMeshShaderPass1, sizeof(g_pUberMeshShaderPass1));
    m_UberMeshPSO[1].Finalize();

    m_MeshBufferGenPSO.SetRootSignature(m_RootSig);
    m_MeshBufferGenPSO.SetComputeShader(g_pMeshBufferGenCS, sizeof(g_pMeshBufferGenCS));
    m_MeshBufferGenPSO.Finalize();

    m_ResolveVBufferToGBufferPSO.SetRootSignature(m_RootSig);
    m_ResolveVBufferToGBufferPSO.SetComputeShader(g_pResolveVBufferToGBufferCS, sizeof(g_pResolveVBufferToGBufferCS));
    m_ResolveVBufferToGBufferPSO.Finalize();

    // Default PSO
    m_DefaultPSO.SetRootSignature(m_RootSig);
    m_DefaultPSO.SetRasterizerState(RasterizerDefault);
    m_DefaultPSO.SetBlendState(BlendDisable);
    m_DefaultPSO.SetDepthStencilState(DepthStateReadWrite);
    m_DefaultPSO.SetInputLayout(0, nullptr);
    m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_DefaultPSO.SetRenderTargetFormats(1, &ColorFormat, DepthFormat);
    //m_DefaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
    //m_DefaultPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));

    // Skybox PSO

    m_SkyboxPSO = m_DefaultPSO;
    m_SkyboxPSO.SetDepthStencilState(DepthStateReadOnly);
    m_SkyboxPSO.SetInputLayout(0, nullptr);
    m_SkyboxPSO.SetVertexShader(g_pSkyboxVS, sizeof(g_pSkyboxVS));
    m_SkyboxPSO.SetPixelShader(g_pSkyboxPS, sizeof(g_pSkyboxPS));
    m_SkyboxPSO.Finalize();

    TextureManager::Initialize(L"");

    s_TextureHeap.Create(L"Scene Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096 * 8);
    m_BindlessResources = s_TextureHeap.Alloc(BINDLESS_CAPACITY);

    // Maybe only need 2 for wrap vs. clamp?  Currently we allocate 1 for 1 with textures
    s_SamplerHeap.Create(L"Scene Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

    Lighting::InitializeResources();

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();

	GPUDrivenDrawIndirectCommandSignature.Reset(1);
	GPUDrivenDrawIndirectCommandSignature[0].DispatchMesh();
	GPUDrivenDrawIndirectCommandSignature.Finalize(&m_RootSig, sizeof(DispatchMeshCommand));

    m_InstanceCullPSO[0].SetRootSignature(m_RootSig);
	m_InstanceCullPSO[0].SetComputeShader(g_pInstanceCullPass0CS, sizeof(g_pInstanceCullPass0CS));
	m_InstanceCullPSO[0].Finalize();

	m_InstanceCullPSO[1].SetRootSignature(m_RootSig);
	m_InstanceCullPSO[1].SetComputeShader(g_pInstanceCullPass1CS, sizeof(g_pInstanceCullPass1CS));
	m_InstanceCullPSO[1].Finalize();

    m_DAGCullPSO[0].SetRootSignature(m_RootSig);
	m_DAGCullPSO[0].SetComputeShader(g_pDAGCullPass0CS, sizeof(g_pDAGCullPass0CS));
	m_DAGCullPSO[0].Finalize();

	m_DAGCullPSO[1].SetRootSignature(m_RootSig);
	m_DAGCullPSO[1].SetComputeShader(g_pDAGCullPass1CS, sizeof(g_pDAGCullPass1CS));
	m_DAGCullPSO[1].Finalize();

	m_ExportDepthPSO = m_DefaultPSO;
	m_ExportDepthPSO.SetRasterizerState(RasterizerTwoSided);
	m_ExportDepthPSO.SetDepthStencilState(DepthStateReadWrite);
	m_ExportDepthPSO.SetInputLayout(0, nullptr);
	m_ExportDepthPSO.SetVertexShader(g_pExportDepthVS, sizeof(g_pExportDepthVS));
	m_ExportDepthPSO.SetPixelShader(g_pExportDepthPS, sizeof(g_pExportDepthPS));
	m_ExportDepthPSO.Finalize();

    m_InstanceCullDepthOnlyPSO.SetRootSignature(m_RootSig);
    m_InstanceCullDepthOnlyPSO.SetComputeShader(g_pInstanceCullDepthOnlyPassCS, sizeof(g_pInstanceCullDepthOnlyPassCS));
    m_InstanceCullDepthOnlyPSO.Finalize();

    m_DAGCullDepthOnlyPSO.SetRootSignature(m_RootSig);
    m_DAGCullDepthOnlyPSO.SetComputeShader(g_pDAGCullDepthOnlyPassCS, sizeof(g_pDAGCullDepthOnlyPassCS));
    m_DAGCullDepthOnlyPSO.Finalize();

    m_DepthOnlyPSO.SetRootSignature(m_RootSig);
    m_DepthOnlyPSO.SetRasterizerState(RasterizerTwoSided);
    m_DepthOnlyPSO.SetDepthStencilState(DepthStateReadWrite);
    m_DepthOnlyPSO.SetBlendState(BlendDisable);
    m_DepthOnlyPSO.SetRenderTargetFormats(0, {}, g_ShadowBuffer.GetFormat());
    m_DepthOnlyPSO.SetMeshShader(g_pUberMeshShaderDepthOnlyPass, sizeof(g_pUberMeshShaderDepthOnlyPass));
    m_DepthOnlyPSO.SetPixelShader(g_pDepthOnlyPS, sizeof(g_pDepthOnlyPS));
    m_DepthOnlyPSO.Finalize();

    // Initialize bindless resource descriptor bindings
    {
        // srv
        SetBindlessResourceDescriptor(SRV_SCENE_COLOR, g_SceneColorBuffer.GetSRV());
		SetBindlessResourceDescriptor(SRV_SCENE_DEPTH, g_SceneDepthBuffer.GetDepthSRV());
		//SetBindlessResourceDescriptor(SRV_SCENE_STENCIL, g_SceneDepthBuffer.GetStencilSRV());
		SetBindlessResourceDescriptor(SRV_GBUFFER_A, g_GBufferA.GetSRV());
		SetBindlessResourceDescriptor(SRV_GBUFFER_B, g_GBufferB.GetSRV());
		SetBindlessResourceDescriptor(SRV_GBUFFER_C, g_GBufferC.GetSRV());
		SetBindlessResourceDescriptor(SRV_GBUFFER_D, g_GBufferD.GetSRV());
		SetBindlessResourceDescriptor(SRV_IBL_CUBE_MAP, GetDefaultTexture(kBlackCubeMap));
		SetBindlessResourceDescriptor(SRV_IBL_DIFFUSE_LD, GetDefaultTexture(kBlackCubeMap));
		SetBindlessResourceDescriptor(SRV_IBL_SPECULAR_LD, GetDefaultTexture(kBlackCubeMap));
		SetBindlessResourceDescriptor(SRV_IBL_LUT, GetDefaultTexture(kBlackTransparent2D));
        SetBindlessResourceDescriptor(SRV_SSAO, g_SSAOFullScreen.GetSRV());
		SetBindlessResourceDescriptor(SRV_SHADOW_MAP, g_ShadowBuffer.GetSRV());
		SetBindlessResourceDescriptor(SRV_SCENE_HZB0, g_FurthestHZB[0].GetSRV());
		SetBindlessResourceDescriptor(SRV_SCENE_HZB1, g_FurthestHZB[1].GetSRV());
		SetBindlessResourceDescriptor(SRV_VBUFFER, g_VisibilityBuffer.GetSRV());

        // uav
		SetBindlessResourceDescriptor(UAV_SCENE_COLOR, g_SceneColorBuffer.GetUAV());
		SetBindlessResourceDescriptor(UAV_VBUFFER, g_VisibilityBuffer.GetUAV());
		SetBindlessResourceDescriptor(UAV_GBUFFER_A, g_GBufferA.GetUAV());
		SetBindlessResourceDescriptor(UAV_GBUFFER_B, g_GBufferB.GetUAV());
		SetBindlessResourceDescriptor(UAV_GBUFFER_C, g_GBufferC.GetUAV());
		SetBindlessResourceDescriptor(UAV_GBUFFER_D, g_GBufferD.GetUAV());
    }

    s_Initialized = true;
}

void Renderer::UpdateGlobalDescriptors(void)
{
    if (g_SSAOFullScreenID == g_SSAOFullScreen.GetVersionID() &&
        g_ShadowBufferID == g_ShadowBuffer.GetVersionID())
    {
        return;
    }

	SetBindlessResourceDescriptor(SRV_SSAO, g_SSAOFullScreen.GetSRV());
	SetBindlessResourceDescriptor(SRV_SHADOW_MAP, g_ShadowBuffer.GetSRV());

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();

}

void Renderer::SetIBLTextures()
{
    if (IBL::IsValid())
    {
		SetBindlessResourceDescriptor(SRV_IBL_DIFFUSE_LD, Graphics::g_IBLDiffuseLDMap.GetSRV());
		SetBindlessResourceDescriptor(SRV_IBL_SPECULAR_LD, Graphics::g_IBLSpecularLDMap.GetSRV());
		SetBindlessResourceDescriptor(SRV_IBL_LUT, Graphics::g_IBLLut.GetSRV());
        SetBindlessResourceDescriptor(SRV_IBL_CUBE_MAP, Graphics::g_IBLCubeMap.GetSRV());
    }
    else
    {
		SetBindlessResourceDescriptor(SRV_IBL_DIFFUSE_LD, GetDefaultTexture(Graphics::kBlackCubeMap));
		SetBindlessResourceDescriptor(SRV_IBL_SPECULAR_LD, GetDefaultTexture(Graphics::kBlackCubeMap));
		SetBindlessResourceDescriptor(SRV_IBL_LUT, GetDefaultTexture(Graphics::kBlackTransparent2D));
        SetBindlessResourceDescriptor(SRV_IBL_CUBE_MAP, GetDefaultTexture(Graphics::kBlackCubeMap));
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

	GPUDrivenDrawIndirectCommandSignature.Destroy();
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
        uint32_t BindlessResourcesBaseIndex;
    } skyPSCB;
    skyPSCB.TextureLevel = s_SpecularIBLBias;
    skyPSCB.BindlessResourcesBaseIndex = GetBindlessResourcesBaseOffset();

    gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
    gfxContext.SetRootSignature(m_RootSig);
    gfxContext.SetPipelineState(m_SkyboxPSO);

    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
    gfxContext.SetViewportAndScissor(viewport, scissor);

   
    gfxContext.SetDynamicConstantBufferView(kMeshConstants, sizeof(SkyboxVSCB), &skyVSCB);
    gfxContext.SetDynamicConstantBufferView(kMaterialConstants, sizeof(SkyboxPSCB), &skyPSCB);
    gfxContext.Draw(3);
}

void Renderer::InstanceCull(DrawPass pass, GraphicsContext& gfxContext, const GlobalConstants& inGlobals, uint32_t cullPassIdx)
{
    ScopedTimer _prof(L"Renderer::InstanceCull", gfxContext);
    ComputeContext& context = gfxContext.GetComputeContext();

	context.TransitionResource( DrawCommandManager::GetPotentialDrawItemsGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
	context.SetRootSignature(m_RootSig);

    if (pass == kZPass)
        context.SetPipelineState(m_InstanceCullDepthOnlyPSO);
    else
	    context.SetPipelineState(m_InstanceCullPSO[cullPassIdx]);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
	context.SetConstants(kCommandConstants,  DrawCommandManager::GetNumPotentialDrawItems());

    context.Dispatch1D( DrawCommandManager::GetNumPotentialDrawItems());
}

 static uint32_t GetDAGCullGroupCount()
{
    static uint32_t s_GroupCount = 0;
    if (s_GroupCount != 0)
        return s_GroupCount;

    ID3D12Device* device = Graphics::g_Device;
    if (device == nullptr)
    {
        s_GroupCount = 768;
        return s_GroupCount;
    }

    // Vendor defaults (tune by profiling)
    if (Graphics::IsDeviceAMD(device))
        s_GroupCount = 1024;
    else if (Graphics::IsDeviceNvidia(device))
        s_GroupCount = 768;
    else if (Graphics::IsDeviceIntel(device))
        s_GroupCount = 512;
    else
        s_GroupCount = 768;

    return s_GroupCount;
}

void Renderer::DAGCull(DrawPass pass, GraphicsContext& gfxContext, const GlobalConstants& inGlobals, const BaseCamera* camera, 
    const D3D12_VIEWPORT& viewport, uint32_t cullPassIdx)
{
	ScopedTimer _prof(L"Renderer::DAGCull", gfxContext);
	ComputeContext& context = gfxContext.GetComputeContext();
	context.TransitionResource( DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
	context.SetRootSignature(m_RootSig);

    if (pass == kZPass)
        context.SetPipelineState(m_DAGCullDepthOnlyPSO);
    else
        context.SetPipelineState(m_DAGCullPSO[cullPassIdx]);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);

    context.SetConstants(kCommandConstants, 0, (float)PixelErrorThreshold);

    const uint32_t dagGroups = GetDAGCullGroupCount();
    context.Dispatch1D(dagGroups * DAG_CULL_GROUP_SIZE, DAG_CULL_GROUP_SIZE);
}


void Renderer::ExportDepth(GraphicsContext& gfxContext, const GlobalConstants& inGlobals,
	const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
{
	ScopedTimer _prof(L"Renderer::ExportDepth", gfxContext);
	gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
	gfxContext.SetRootSignature(m_RootSig);
	gfxContext.SetPipelineState(m_ExportDepthPSO);

	gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	gfxContext.SetDepthStencilTarget(g_SceneDepthBuffer.GetDSV());
	gfxContext.SetViewportAndScissor(viewport, scissor);
    gfxContext.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
	gfxContext.Draw(3);
}

void Renderer::ResolveVBufferToGBuffer(GraphicsContext& gfxContext, const GlobalConstants& inGlobals)
{
	ScopedTimer _prof(L"Renderer::ResolveVBufferToGBuffer", gfxContext);
	ComputeContext& context = gfxContext.GetComputeContext();

	context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	context.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
	context.SetRootSignature(m_RootSig);

    context.SetPipelineState(m_ResolveVBufferToGBufferPSO);
	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
    context.SetConstants(kViewModeConstants, (uint32_t)ViewMode);

    context.Dispatch2D(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());
}

uint32_t Renderer::GetBindlessResourcesBaseOffset()
{
    return s_TextureHeap.GetOffsetOfHandle(m_BindlessResources);
}

void Renderer::SetBindlessResourceDescriptor(uint32_t bindlessIndex, const D3D12_CPU_DESCRIPTOR_HANDLE& handle)
{
	ASSERT(handle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN, "Trying to set an invalid descriptor handle in bindless resources, bindlessIndex: %d", bindlessIndex);
	uint32_t DestCount = 1;
	uint32_t SourceCounts[] = { 1 };
	D3D12_CPU_DESCRIPTOR_HANDLE SourceBuffers[] =
	{
        handle
	};
	DescriptorHandle dest = m_BindlessResources + bindlessIndex * Renderer::s_TextureHeap.GetDescriptorSize();
	g_Device->CopyDescriptors(1, &dest, &DestCount, DestCount, SourceBuffers, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

HierarchicalDepthBuffer& Renderer::GetPrevHZB()
{
    return g_FurthestHZB[1 - TemporalEffects::GetFrameIndexMod2()];
}

HierarchicalDepthBuffer& Renderer::GetCurrentHZB()
{
    return g_FurthestHZB[TemporalEffects::GetFrameIndexMod2()];
}

void MeshSorter::Sort()
{
    struct { bool operator()(uint64_t a, uint64_t b) const { return a < b; } } Cmp;
    std::sort(m_SortKeys.begin(), m_SortKeys.end(), Cmp);
}

void MeshSorter::RenderMeshedInternal(
    DrawPass pass,
    GraphicsContext& context,
    const GlobalConstants& inGlobals)
{
    for (auto& chunksGPU : GeometryStreaming::m_GeometryChunksGPU)
    {
        context.TransitionResource(chunksGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    
    context.TransitionResource(GeometryStreaming::m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    if (!FreezeCull)
    {
        context.GetComputeContext().ClearBufferUAV( DrawCommandManager::GetTaskQueueStateGPU(),
             DrawCommandManager::GetTaskQueueStateGPU().GetBufferSize(), 0);
        context.TransitionResource( DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.ClearUAV( DrawCommandManager::GetTaskQueueGPU(), 0xFFFFFFFF);
        context.TransitionResource( DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.ClearUAV( DrawCommandManager::GetMeshletBatchGPU(), 0);
        context.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        //context.ClearUAV(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, 0);

        if (pass == DrawPass::kZPass)
        {
            uint32_t passIndex = 0;
            Renderer::InstanceCull(pass, context, inGlobals, passIndex);
            Renderer::DAGCull(pass, context, inGlobals, m_Camera, m_Viewport, passIndex);
            {
                // mesh buffer gen
                context.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                context.GetComputeContext().SetPipelineState(m_MeshBufferGenPSO);
                context.GetComputeContext().SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.GetComputeContext().SetConstants(kCommandConstants, passIndex); // pass index
                context.GetComputeContext().Dispatch1D(1, 1);
            }

            {
                ScopedTimer _prof(L"Dispatch Mesh Indirect Depth Only Pass", context);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.SetPipelineState(m_DepthOnlyPSO);
                context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.SetConstants(kCommandConstants, 0, 0, 0);
                context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
            }
        }
        else
        {

            uint32_t passIndex = 0;
            Renderer::InstanceCull(pass, context, inGlobals, passIndex);
            Renderer::DAGCull(pass, context, inGlobals, m_Camera, m_Viewport, passIndex);
            {
                // mesh buffer gen
                context.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                context.GetComputeContext().SetPipelineState(m_MeshBufferGenPSO);
                context.GetComputeContext().SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.GetComputeContext().SetConstants(kCommandConstants, passIndex); // pass index
                context.GetComputeContext().Dispatch1D(1, 1);
            }

            {
                ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 0", context);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.SetPipelineState(m_UberMeshPSO[passIndex]);
                context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.SetConstants(kCommandConstants, 0, 0, 0);
                context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
            }

            {
                ExportDepth(context, inGlobals, m_Viewport, m_Scissor);
            }

            {
                context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Renderer::GetCurrentHZB().GenerateHZB(context, g_SceneDepthBuffer);
            }

            passIndex = 1;
            Renderer::InstanceCull(pass, context, inGlobals, passIndex);
            Renderer::DAGCull(pass, context, inGlobals, m_Camera, m_Viewport, passIndex);
            {
                // mesh buffer gen
                context.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                context.GetComputeContext().SetPipelineState(m_MeshBufferGenPSO);
                context.GetComputeContext().SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.GetComputeContext().SetConstants(kCommandConstants, passIndex); // pass index
                context.GetComputeContext().Dispatch1D(1, 1);
            }

            {
                ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 1", context);
                context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context.SetPipelineState(m_UberMeshPSO[passIndex]);
                context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
                context.SetConstants(kCommandConstants, 0, 0, 0);
                context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
            }

            {
                ExportDepth(context, inGlobals, m_Viewport, m_Scissor);
            }

            {
                context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Renderer::GetCurrentHZB().GenerateHZB(context, g_SceneDepthBuffer);
            }
        }
    }
    else
    {
        uint32_t passIndex = 0;
        {
            // mesh buffer gen
            context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.GetComputeContext().SetPipelineState(m_MeshBufferGenPSO);
            context.GetComputeContext().SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
            context.GetComputeContext().SetConstants(kCommandConstants, passIndex); // pass index
            context.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 0", context);
            context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.SetPipelineState(m_UberMeshPSO[passIndex]);
            context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
            context.SetConstants(kCommandConstants, 0, 0, 0);
            context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

        passIndex = 1;
        {
            // mesh buffer gen
            context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.GetComputeContext().SetPipelineState(m_MeshBufferGenPSO);
            context.GetComputeContext().SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
            context.GetComputeContext().SetConstants(kCommandConstants, passIndex); // pass index
            context.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 1", context);
            context.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.SetPipelineState(m_UberMeshPSO[passIndex]);
            context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &inGlobals);
            context.SetConstants(kCommandConstants, 0, 0, 0);
            context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

		{
			ExportDepth(context, inGlobals, m_Viewport, m_Scissor);
		}

		{
			context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Renderer::GetCurrentHZB().GenerateHZB(context, g_SceneDepthBuffer);
		}
    }
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
    globals.ProjMatrix = m_Camera->GetProjMatrix();
    globals.InverseViewProjMatrix = Invert(m_Camera->GetViewProjMatrix());
    const Frustum& frustum = m_Camera->GetViewSpaceFrustum();

    //kNearPlane, kFarPlane, kLeftPlane, kRightPlane, kTopPlane, kBottomPlane

	globals.ViewSpaceFrustumPlanes[0] = frustum.GetFrustumPlane(Frustum::kNearPlane);
	globals.ViewSpaceFrustumPlanes[1] = frustum.GetFrustumPlane(Frustum::kFarPlane);
	globals.ViewSpaceFrustumPlanes[2] = frustum.GetFrustumPlane(Frustum::kLeftPlane);
	globals.ViewSpaceFrustumPlanes[3] = frustum.GetFrustumPlane(Frustum::kRightPlane);
	globals.ViewSpaceFrustumPlanes[4] = frustum.GetFrustumPlane(Frustum::kTopPlane);
	globals.ViewSpaceFrustumPlanes[5] = frustum.GetFrustumPlane(Frustum::kBottomPlane);

    Renderer::UpdateGlobalDescriptors();

    context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

	// Must set the Graphics / Compute root signature only * after * setting your descriptor heaps, as the correct heap pointers must be available when root signature is set.
    context.SetRootSignature(m_RootSig);

	context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);
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
    //const uint32_t passCount = m_PassCounts[pass];
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
                context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
                context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV());
				break;
            case kVBuffer:
				context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                break;
			case kGBuffer:
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

        //context.SetIndexBuffer(m_IBV);
        
        //const uint32_t lastDraw = m_CurrentDraw + passCount;
		if (pass == kTransparent)
		{
            //RenderMeshedInternal(context, globals, Renderer::PsoIdx::kPsoTransparent, true);
		}
		else
		{
            //if (!FreezeCull)
            //{
            //    context.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            //    context.ClearUAV(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, 0);
            //}

			if (kVBuffer == pass || kZPass == pass)
			{
                if (DrawCommandManager::GetNumPotentialDrawItems() > 0)
                    RenderMeshedInternal(pass, context, globals);
			}
        }
    }

	if (m_BatchType == kShadows)
	{
		context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

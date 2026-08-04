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
#include "../Core/ProgramBinder.h"
#include "../Core/ProgramUtils.h"
#include "../Core/Utility.h"

#include "CommandBucketer.h"
#include "GeometryStreaming.h"
#include "VirtualShadowMap.h"

#include <filesystem>

#pragma warning(disable:4319) // '~': zero extending 'uint32_t' to 'uint64_t' of greater size

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace
{
    struct HZBDescriptorSet
    {
        DescriptorHandle SRVs;
    };

    Renderer::HZBResources MakeHZBResources(
        HierarchicalDepthBuffer (&resources)[2],
        HZBDescriptorSet& descriptorSet,
        uint32_t currentIndex)
    {
        ASSERT(currentIndex < 2);

        const uint32_t previousIndex = 1 - currentIndex;
        const uint32_t descriptorBaseIndex =
            Renderer::s_TextureHeap.GetOffsetOfHandle(descriptorSet.SRVs);

        Renderer::HZBResources result;
        result.Previous = &resources[previousIndex];
        result.Current = &resources[currentIndex];
        result.PreviousSRVIndex = descriptorBaseIndex + previousIndex;
        result.CurrentSRVIndex = descriptorBaseIndex + currentIndex;
        return result;
    }

    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring modulePath(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                nullptr,
                modulePath.data(),
                static_cast<DWORD>(modulePath.size()));
            if (length == 0)
                return {};

            if (static_cast<size_t>(length) < modulePath.size())
            {
                modulePath.resize(length);
                return std::filesystem::path(modulePath).parent_path();
            }

            modulePath.resize(modulePath.size() * 2);
        }
    }

    void SetCommonResources(ProgramBinder& binder, const FrameConstants& frame)
    {
        ProgramVar commonResources = binder["g_CommonResources"];
        commonResources["BindlessResourcesBaseIndex"].Set(frame.BindlessResourcesBaseIndex);
        commonResources["FrameIndexMod2"].Set(frame.FrameIndexMod2);
    }

    bool BindVBufferMeshPass(
        GraphicsContext& context,
        const std::shared_ptr<Program>& program,
        const MeshShaderPSO& pso,
        const RenderView& view,
        const FrameConstants& frame)
    {
        ASSERT(program != nullptr);
        if (!program)
            return false;

        ProgramBinder binder(*program, context);
        binder.SetRootSignature();
        context.SetPipelineState(pso);

        const ViewConstants& viewConstants = view.GetConstants();
        ProgramVar constants = binder["g_VBufferMesh"];
        constants["ViewProjMatrix"].Set(viewConstants.ViewProjMatrix);
        constants["ViewportWidth"].Set(viewConstants.ViewportWidth);
        constants["ViewportHeight"].Set(viewConstants.ViewportHeight);
        SetCommonResources(binder, frame);
        binder.Apply();
        return true;
    }

    bool BindMeshBufferGenPass(
        GraphicsContext& context,
        const std::shared_ptr<Program>& program,
        const ComputePSO& pso,
        const FrameConstants& frame,
        uint32_t passIndex)
    {
        ASSERT(program != nullptr);
        if (!program)
            return false;

        ComputeContext& computeContext = context.GetComputeContext();
        computeContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
        computeContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

        ProgramBinder binder(*program, computeContext);
        binder.SetRootSignature();
        computeContext.SetPipelineState(pso);

        ProgramVar constants = binder["g_MeshBufferGen"];
        constants["PassIndex"].Set(passIndex);
        SetCommonResources(binder, frame);
        binder.Apply();
        return true;
    }
}

namespace Renderer
{
	NumVar PixelErrorThreshold("Renderer/Pixel Error Threshold", 1.0f, 0.5f, 10.0f, 0.25f);
	BoolVar FreezeCull("Renderer/Freeze Cull", false);
    
    const char* ViewModeLabels[] = {
        "Lit", "MeshletLOD", "MeshletID", "MeshletTriangle", "MeshID", "InstanceID", "MaterialID", "VSMAddress"
    };
    EnumVar ViewMode("Visualize/View Mode", 0, _countof(ViewModeLabels), ViewModeLabels);

    bool s_Initialized = false;

    DescriptorHeap s_TextureHeap;
    DescriptorHeap s_SamplerHeap;
    HZBDescriptorSet m_SceneHZBDescriptors;
    HZBDescriptorSet m_ShadowHZBDescriptors;

	CommandSignature GPUDrivenDrawIndirectCommandSignature;

    TextureRef s_RadianceCubeMap;
    TextureRef s_IrradianceCubeMap;
    float s_SpecularIBLRange;
    float s_SpecularIBLBias;
    uint32_t g_SSAOFullScreenID;
    uint32_t g_ShadowBufferID;
    uint32_t g_SceneHZBIDs[2];

    //RootSignature m_RootSig;
    GraphicsPSO m_SkyboxPSO(L"Renderer: Skybox PSO");
    std::shared_ptr<Program> m_SkyboxProgram;
    GraphicsPSO m_DefaultPSO(L"Renderer: Default PSO"); // Not finalized.  Used as a template.

    DescriptorHandle m_BindlessResources;

	ComputePSO m_MeshBufferGenPSO(L"Renderer: Mesh Buffer Gen PSO");
    std::shared_ptr<Program> m_MeshBufferGenProgram;

    MeshShaderPSO m_VBufferMeshPSO[2] = {
        MeshShaderPSO(L"Renderer: VBuffer Mesh PSO Pass 0"),
        MeshShaderPSO(L"Renderer: VBuffer Mesh PSO Pass 1")
	};
    std::shared_ptr<Program> m_VBufferMeshProgram[2];

	ComputePSO m_ResolveVBufferToGBufferPSO(L"Renderer: Resolve VBuffer To GBuffer PSO");
    std::shared_ptr<Program> m_ResolveVBufferToGBufferProgram;

	ComputePSO m_InstanceCullPSO[2] = {
        ComputePSO(L"Renderer: Instance Cull Pass 0 PSO"),
        ComputePSO(L"Renderer: Instance Cull Pass 1 PSO")
	};
    std::shared_ptr<Program> m_InstanceCullProgram[2];

    ComputePSO m_DAGCullPSO[2] = {
        ComputePSO(L"Renderer: DAG Cull Pass 0 PSO"),
        ComputePSO(L"Renderer: DAG Cull Pass 1 PSO")
	};
    std::shared_ptr<Program> m_DAGCullProgram[2];

    GraphicsPSO m_ExportDepthPSO(L"Renderer: Export Depth PSO");
    std::shared_ptr<Program> m_ExportDepthProgram;

    ComputePSO m_InstanceCullNoHZBPSO(L"Renderer: Instance Cull No HZB PSO");
    std::shared_ptr<Program> m_InstanceCullNoHZBProgram;

    ComputePSO m_DAGCullNoHZBPSO(L"Renderer: DAG Cull No HZB PSO");
    std::shared_ptr<Program> m_DAGCullNoHZBProgram;

    MeshShaderPSO m_DepthOnlyMeshPSO[2] = {
        MeshShaderPSO (L"Renderer: Depth Only Mesh Pass 0 PSO"),
        MeshShaderPSO (L"Renderer: Depth Only Mesh Pass 1 PSO")
    };
    std::shared_ptr<Program> m_DepthOnlyMeshProgram[2];
}

std::string Renderer::GetModelShaderPath(const char* shaderFileName)
{
    ASSERT(shaderFileName != nullptr && shaderFileName[0] != '\0');
    if (shaderFileName == nullptr || shaderFileName[0] == '\0')
        return {};

    const std::filesystem::path shaderRelativePath =
        std::filesystem::path(L"MiniEngine") / L"Model" / L"Shaders" / shaderFileName;
    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    const std::filesystem::path packagedPath =
        executableDirectory.empty() ? std::filesystem::path() : executableDirectory / shaderRelativePath;

    std::error_code error;
    if (!packagedPath.empty() && std::filesystem::is_regular_file(packagedPath, error))
        return Utility::WideStringToUTF8(packagedPath.lexically_normal().wstring());

    const std::filesystem::path sourcePath =
        std::filesystem::path(__FILE__).parent_path() / L"Shaders" / shaderFileName;
    error.clear();
    if (std::filesystem::is_regular_file(sourcePath, error))
        return Utility::WideStringToUTF8(sourcePath.lexically_normal().wstring());

    const std::string message =
        "Model shader not found. packaged='" + Utility::WideStringToUTF8(packagedPath.wstring()) +
        "', source='" + Utility::WideStringToUTF8(sourcePath.wstring()) + "'\n";
    Utility::Print(message.c_str());
    return Utility::WideStringToUTF8(
        (packagedPath.empty() ? sourcePath : packagedPath).lexically_normal().wstring());
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

 //   m_RootSig.Reset(kNumRootBindings, 5);
 //   m_RootSig.InitStaticSampler(10, DefaultSamplerDesc);
 //   m_RootSig.InitStaticSampler(11, SamplerShadowDesc);
 //   m_RootSig.InitStaticSampler(12, CubeMapSamplerDesc);
 //   m_RootSig.InitStaticSampler(13, LinearSamplerDesc);
 //   m_RootSig.InitStaticSampler(14, PointSamplerDesc);
 //   m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
 //   m_RootSig[kMaterialConstants].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_ALL);
	//m_RootSig[kCommonCBV].InitAsConstantBuffer(2);
	//m_RootSig[kCommandConstants].InitAsConstants(3, 4);
	//m_RootSig[kViewModeConstants].InitAsConstants(5, 4);
 //   m_RootSig[kStandbyCBV].InitAsConstantBuffer(6);
 //   m_RootSig.Finalize(L"RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT 
 //       | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
 //       | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneDepthBuffer.GetFormat();

    // Mesh shader PSO
    for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
    {
        ProgramDesc vBufferMeshDesc = ProgramUtils::MakeGraphicsDesc(
            GetModelShaderPath("VBufferMesh.slang"),
            "",
            "pixelMain",
            "meshMain",
            ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
        vBufferMeshDesc.AddDefine("VBUFFER_MESH_PASS_INDEX", std::to_string(passIndex));

        m_VBufferMeshProgram[passIndex] = ProgramUtils::GetProgram(
            vBufferMeshDesc,
            passIndex == 0 ? "Renderer: VBuffer Mesh Pass 0" : "Renderer: VBuffer Mesh Pass 1");
        if (!m_VBufferMeshProgram[passIndex])
            return;

	    m_VBufferMeshPSO[passIndex].SetRasterizerState(RasterizerTwoSided);
        m_VBufferMeshPSO[passIndex].SetDepthStencilState(DepthStateDisabled);
	    m_VBufferMeshPSO[passIndex].SetBlendState(BlendDisable);
        m_VBufferMeshPSO[passIndex].SetRenderTargetFormats(0, {}, g_SceneDepthBuffer.GetFormat());
        ProgramUtils::SetProgram(m_VBufferMeshPSO[passIndex], *m_VBufferMeshProgram[passIndex]);
	    m_VBufferMeshPSO[passIndex].Finalize();
    }

    ProgramDesc meshBufferGenDesc = ProgramUtils::MakeComputeDesc(
        GetModelShaderPath("MeshBufferGen.slang"),
        "computeMain",
        ProgramUtils::BindlessMode::ResourceHeap);
    m_MeshBufferGenProgram = ProgramUtils::GetProgram(
        meshBufferGenDesc,
        "Renderer: MeshBufferGen");
    if (!m_MeshBufferGenProgram)
        return;

    ProgramUtils::SetProgram(m_MeshBufferGenPSO, *m_MeshBufferGenProgram);
    m_MeshBufferGenPSO.Finalize();

    ProgramDesc resolveVBufferToGBufferDesc = ProgramUtils::MakeComputeDesc(
        GetModelShaderPath("ResolveVBufferToGBuffer.slang"),
        "computeMain",
        ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
    m_ResolveVBufferToGBufferProgram = ProgramUtils::GetProgram(
        resolveVBufferToGBufferDesc,
        "Renderer: ResolveVBufferToGBuffer");
    if (!m_ResolveVBufferToGBufferProgram)
        return;

    ProgramUtils::SetProgram(m_ResolveVBufferToGBufferPSO, *m_ResolveVBufferToGBufferProgram);
    m_ResolveVBufferToGBufferPSO.Finalize();

    // Default PSO
    //m_DefaultPSO.SetRootSignature(m_RootSig);
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

    ProgramDesc skyboxDesc = ProgramUtils::MakeGraphicsDesc(
        GetModelShaderPath("Skybox.slang"),
        "vertexMain",
        "pixelMain",
        ProgramUtils::BindlessMode::ResourceHeap);
    skyboxDesc.AddStaticSampler(
        "g_SkyboxSampler",
        DefaultSamplerDesc,
        D3D12_SHADER_VISIBILITY_PIXEL);
    m_SkyboxProgram = ProgramUtils::GetProgram(skyboxDesc, "Renderer: Skybox");
    if (!m_SkyboxProgram)
        return;

    ProgramUtils::SetProgram(m_SkyboxPSO, *m_SkyboxProgram);
    m_SkyboxPSO.Finalize();

    TextureManager::Initialize(L"");

    s_TextureHeap.Create(L"Scene Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096 * 8);
    m_BindlessResources = s_TextureHeap.Alloc(BINDLESS_CAPACITY);
    const uint32_t descriptorSize = s_TextureHeap.GetDescriptorSize();
    m_SceneHZBDescriptors.SRVs = m_BindlessResources + SRV_SCENE_HZB0 * descriptorSize;
    m_ShadowHZBDescriptors.SRVs = m_BindlessResources + SRV_SHADOW_HZB0 * descriptorSize;

    // Maybe only need 2 for wrap vs. clamp?  Currently we allocate 1 for 1 with textures
    s_SamplerHeap.Create(L"Scene Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

    Lighting::InitializeResources();
    if (!VirtualShadowMap::Initialize())
        return;

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();
    g_SceneHZBIDs[0] = g_FurthestHZB[0].GetVersionID();
    g_SceneHZBIDs[1] = g_FurthestHZB[1].GetVersionID();

	GPUDrivenDrawIndirectCommandSignature.Reset(1);
	GPUDrivenDrawIndirectCommandSignature[0].DispatchMesh();
	GPUDrivenDrawIndirectCommandSignature.Finalize(nullptr, sizeof(DispatchMeshCommand));

    for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
    {
        ProgramDesc instanceCullDesc = ProgramUtils::MakeComputeDesc(
            GetModelShaderPath("InstanceCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        instanceCullDesc.AddDefine("INSTANCE_CULL_PASS_INDEX", std::to_string(passIndex));
        instanceCullDesc.AddStaticSampler("g_HZBSampler", PointSamplerDesc);

        m_InstanceCullProgram[passIndex] = ProgramUtils::GetProgram(
            instanceCullDesc,
            passIndex == 0 ? "Renderer: InstanceCull Pass 0" : "Renderer: InstanceCull Pass 1");
        if (!m_InstanceCullProgram[passIndex])
            return;

        ProgramUtils::SetProgram(m_InstanceCullPSO[passIndex], *m_InstanceCullProgram[passIndex]);
        m_InstanceCullPSO[passIndex].Finalize();
    }

    for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
    {
        ProgramDesc dagCullDesc = ProgramUtils::MakeComputeDesc(
            GetModelShaderPath("DAGCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        dagCullDesc.AddDefine("DAG_CULL_PASS_INDEX", std::to_string(passIndex));
        dagCullDesc.AddStaticSampler("g_HZBSampler", PointSamplerDesc);
        dagCullDesc.AddRootBufferUAV("g_TaskQueueStateUAV");
        dagCullDesc.AddRootBufferUAV("g_TaskQueueUAV");
        dagCullDesc.AddRootBufferUAV("g_MeshletBatchUAV");
        dagCullDesc.AddRootBufferUAV("g_CandidateMeshletUAV");

        m_DAGCullProgram[passIndex] = ProgramUtils::GetProgram(
            dagCullDesc,
            passIndex == 0 ? "Renderer: DAGCull Pass 0" : "Renderer: DAGCull Pass 1");
        if (!m_DAGCullProgram[passIndex])
            return;

        ProgramUtils::SetProgram(m_DAGCullPSO[passIndex], *m_DAGCullProgram[passIndex]);
        m_DAGCullPSO[passIndex].Finalize();
    }

    {
        ProgramDesc instanceCullNoHZBDesc = ProgramUtils::MakeComputeDesc(
            GetModelShaderPath("InstanceCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        instanceCullNoHZBDesc.AddDefine("DISABLE_HZB_CULL");
        m_InstanceCullNoHZBProgram = ProgramUtils::GetProgram(
            instanceCullNoHZBDesc,
            "Renderer: InstanceCull No HZB");
        if (!m_InstanceCullNoHZBProgram)
            return;

        ProgramUtils::SetProgram(m_InstanceCullNoHZBPSO, *m_InstanceCullNoHZBProgram);
        m_InstanceCullNoHZBPSO.Finalize();
    }

    {
        ProgramDesc dagCullNoHZBDesc = ProgramUtils::MakeComputeDesc(
            GetModelShaderPath("DAGCull.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        dagCullNoHZBDesc.AddDefine("DISABLE_HZB_CULL");
        dagCullNoHZBDesc.AddRootBufferUAV("g_TaskQueueStateUAV");
        dagCullNoHZBDesc.AddRootBufferUAV("g_TaskQueueUAV");
        dagCullNoHZBDesc.AddRootBufferUAV("g_MeshletBatchUAV");
        dagCullNoHZBDesc.AddRootBufferUAV("g_CandidateMeshletUAV");

        m_DAGCullNoHZBProgram = ProgramUtils::GetProgram(
            dagCullNoHZBDesc,
            "Renderer: DAGCull No HZB");
        if (!m_DAGCullNoHZBProgram)
            return;

        ProgramUtils::SetProgram(m_DAGCullNoHZBPSO, *m_DAGCullNoHZBProgram);
        m_DAGCullNoHZBPSO.Finalize();
    }

    for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
    {
        ProgramDesc vDepthOnlyMeshDesc = ProgramUtils::MakeGraphicsDesc(
            GetModelShaderPath("VBufferMesh.slang"),
            "",
            "pixelMain",
            "meshMain",
            ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
        vDepthOnlyMeshDesc.AddDefine("VBUFFER_MESH_PASS_INDEX", std::to_string(passIndex));
        vDepthOnlyMeshDesc.AddDefine("DEPTH_ONLY");

        m_DepthOnlyMeshProgram[passIndex] = ProgramUtils::GetProgram(
            vDepthOnlyMeshDesc,
            "Renderer: DepthOnly Mesh");
        if (!m_DepthOnlyMeshProgram[passIndex])
            return;

        m_DepthOnlyMeshPSO[passIndex].SetRasterizerState(RasterizerShadowTwoSided);
        m_DepthOnlyMeshPSO[passIndex].SetDepthStencilState(DepthStateReadWrite);
        m_DepthOnlyMeshPSO[passIndex].SetBlendState(BlendDisable);
        m_DepthOnlyMeshPSO[passIndex].SetRenderTargetFormats(0, {}, SHADOW_FORMAT);
        ProgramUtils::SetProgram(m_DepthOnlyMeshPSO[passIndex], *m_DepthOnlyMeshProgram[passIndex]);
        m_DepthOnlyMeshPSO[passIndex].Finalize();
    }

	m_ExportDepthPSO = m_DefaultPSO;
	m_ExportDepthPSO.SetRasterizerState(RasterizerTwoSided);
	m_ExportDepthPSO.SetDepthStencilState(DepthStateReadWrite);
	m_ExportDepthPSO.SetInputLayout(0, nullptr);

    ProgramDesc exportDepthDesc = ProgramUtils::MakeGraphicsDesc(
        GetModelShaderPath("ExportDepth.slang"),
        "vertexMain",
        "pixelMain",
        ProgramUtils::BindlessMode::ResourceHeap);
    m_ExportDepthProgram = ProgramUtils::GetProgram(exportDepthDesc, "Renderer: ExportDepth");
    if (!m_ExportDepthProgram)
        return;

    ProgramUtils::SetProgram(m_ExportDepthPSO, *m_ExportDepthProgram);
	m_ExportDepthPSO.Finalize();


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
        SetBindlessResourceDescriptor(SRV_SHADOW_HZB0, g_ShadowHZBBuffer[0].GetSRV());
        SetBindlessResourceDescriptor(SRV_SHADOW_HZB1, g_ShadowHZBBuffer[1].GetSRV());

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
        g_ShadowBufferID == g_ShadowBuffer.GetVersionID() &&
        g_SceneHZBIDs[0] == g_FurthestHZB[0].GetVersionID() &&
        g_SceneHZBIDs[1] == g_FurthestHZB[1].GetVersionID())
    {
        return;
    }

	SetBindlessResourceDescriptor(SRV_SSAO, g_SSAOFullScreen.GetSRV());
	SetBindlessResourceDescriptor(SRV_SHADOW_MAP, g_ShadowBuffer.GetSRV());
    SetBindlessResourceDescriptor(SRV_SCENE_HZB0, g_FurthestHZB[0].GetSRV());
    SetBindlessResourceDescriptor(SRV_SCENE_HZB1, g_FurthestHZB[1].GetSRV());

    g_SSAOFullScreenID = g_SSAOFullScreen.GetVersionID();
    g_ShadowBufferID = g_ShadowBuffer.GetVersionID();
    g_SceneHZBIDs[0] = g_FurthestHZB[0].GetVersionID();
    g_SceneHZBIDs[1] = g_FurthestHZB[1].GetVersionID();

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
    VirtualShadowMap::Shutdown();
    s_RadianceCubeMap = nullptr;
    s_IrradianceCubeMap = nullptr;
    TextureManager::Shutdown();
    s_TextureHeap.Destroy();
    s_SamplerHeap.Destroy();

	GPUDrivenDrawIndirectCommandSignature.Destroy();
}

void Renderer::DrawSkybox( GraphicsContext& gfxContext,
     const RenderView& view,
     const FrameConstants& frame)
{
    ScopedTimer _prof(L"Draw Skybox", gfxContext);

    ASSERT(m_SkyboxProgram != nullptr);
    if (!m_SkyboxProgram)
        return;

    const Matrix4 projInverse = Invert(view.GetConstants().ProjMatrix);
    const Matrix3 viewInverse = Invert(view.GetConstants().ViewMatrix).Get3x3();

    gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*m_SkyboxProgram, gfxContext);
    binder.SetRootSignature();
    gfxContext.SetPipelineState(m_SkyboxPSO);

    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
    gfxContext.SetViewportAndScissor(view.GetViewport(), view.GetScissor());
    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    binder["g_SkyboxVS"]["ProjInverse"].Set(projInverse);
    binder["g_SkyboxVS"]["ViewInverse"].Set(viewInverse);
    SetCommonResources(binder, frame);
    binder.Apply();

    gfxContext.Draw(3);
}

void Renderer::InstanceCull(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame,
    const Program* program,
    const ComputePSO& pso,
    const HZBResources* hzbResources,
    bool disableHZBCull)
{
    ScopedTimer _prof(L"Renderer::InstanceCull", gfxContext);

    ASSERT(program != nullptr);
    if (!program)
        return;

	ComputeContext& context = gfxContext.GetComputeContext();

	context.TransitionResource( DrawCommandManager::GetPotentialDrawItemsGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!disableHZBCull)
    {
        ASSERT(hzbResources != nullptr && hzbResources->IsValid());
        if (hzbResources == nullptr || !hzbResources->IsValid())
            return;

        context.TransitionResource(*hzbResources->Previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(*hzbResources->Current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*program, context);
    binder.SetRootSignature();
	context.SetPipelineState(pso);

    const ViewConstants& viewConstants = view.GetConstants();
    ProgramVar constants = binder["g_InstanceCull"];
    constants["ViewProjMatrix"].Set(viewConstants.ViewProjMatrix);
    constants["ViewportWidth"].Set(viewConstants.ViewportWidth);
    constants["ViewportHeight"].Set(viewConstants.ViewportHeight);
    constants["MaxCommands"].Set(DrawCommandManager::GetNumPotentialDrawItems());
    if (!disableHZBCull)
    {
        constants["PrevViewProjMatrix"].Set(viewConstants.PrevViewProjMatrix);
        constants["HZBSizeAndInv"].Set(viewConstants.HZBSizeAndInv);
        constants["PreviousHZB"].Set(
            SlangDescriptorHandle{ hzbResources->PreviousSRVIndex, 0 });
        constants["CurrentHZB"].Set(
            SlangDescriptorHandle{ hzbResources->CurrentSRVIndex, 0 });
    }

    SetCommonResources(binder, frame);
    binder.Apply();

	context.Dispatch1D( DrawCommandManager::GetNumPotentialDrawItems());
}

uint32_t Renderer::GetDAGCullGroupCount()
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

float Renderer::GetPixelErrorThreshold()
{
    return static_cast<float>(PixelErrorThreshold);
}

void Renderer::DAGCull(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame,
    const Program* program,
    const ComputePSO& pso,
    const HZBResources* hzbResources,
    bool disableHZBCull)
{
	ScopedTimer _prof(L"Renderer::DAGCull", gfxContext);

    ASSERT(program != nullptr);
    if (!program)
        return;

	ComputeContext& context = gfxContext.GetComputeContext();
	context.TransitionResource( DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource( DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource( DrawCommandManager::GetCandidateMeshletGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!disableHZBCull)
    {
        ASSERT(hzbResources != nullptr && hzbResources->IsValid());
        if (hzbResources == nullptr || !hzbResources->IsValid())
            return;

        context.TransitionResource(*hzbResources->Previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(*hzbResources->Current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*program, context);
    binder.SetRootSignature();
	context.SetPipelineState(pso);

    const ViewConstants& viewConstants = view.GetConstants();
    ProgramVar constants = binder["g_DAGCull"];
    constants["ViewProjMatrix"].Set(viewConstants.ViewProjMatrix);
    constants["ViewerPos"].Set(viewConstants.ViewerPos);
    constants["LodScale"].Set(viewConstants.LodScale);
    constants["PixelErrorThreshold"].Set(static_cast<float>(PixelErrorThreshold));
    constants["ProjectionType"].Set(static_cast<uint32_t>(viewConstants.Projection));
    constants["ViewportWidth"].Set(viewConstants.ViewportWidth);
    constants["ViewportHeight"].Set(viewConstants.ViewportHeight);
    if (!disableHZBCull)
    {
        constants["PrevViewProjMatrix"].Set(viewConstants.PrevViewProjMatrix);
        constants["HZBSizeAndInv"].Set(viewConstants.HZBSizeAndInv);
        constants["PreviousHZB"].Set(
            SlangDescriptorHandle{ hzbResources->PreviousSRVIndex, 0 });
        constants["CurrentHZB"].Set(
            SlangDescriptorHandle{ hzbResources->CurrentSRVIndex, 0 });
    }
    SetCommonResources(binder, frame);
    binder["g_TaskQueueStateUAV"].SetRootBufferUAV(DrawCommandManager::GetTaskQueueStateGPU());
    binder["g_TaskQueueUAV"].SetRootBufferUAV(DrawCommandManager::GetTaskQueueGPU());
    binder["g_MeshletBatchUAV"].SetRootBufferUAV(DrawCommandManager::GetMeshletBatchGPU());
    binder["g_CandidateMeshletUAV"].SetRootBufferUAV(DrawCommandManager::GetCandidateMeshletGPU());
    binder.Apply();

    const uint32_t dagGroups = GetDAGCullGroupCount();
    context.Dispatch1D(dagGroups * DAG_CULL_GROUP_SIZE, DAG_CULL_GROUP_SIZE);
}


void Renderer::ExportDepth(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame)
{
	ScopedTimer _prof(L"Renderer::ExportDepth", gfxContext);

    ASSERT(m_ExportDepthProgram != nullptr);
    if (!m_ExportDepthProgram)
        return;

	gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*m_ExportDepthProgram, gfxContext);
    binder.SetRootSignature();
	gfxContext.SetPipelineState(m_ExportDepthPSO);

	gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	gfxContext.SetDepthStencilTarget(g_SceneDepthBuffer.GetDSV());
	gfxContext.SetViewportAndScissor(view.GetViewport(), view.GetScissor());
    SetCommonResources(binder, frame);
    binder.Apply();
	gfxContext.Draw(3);
}

void Renderer::ResolveVBufferToGBuffer(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame)
{
	ScopedTimer _prof(L"Renderer::ResolveVBufferToGBuffer", gfxContext);
    ASSERT(m_ResolveVBufferToGBufferProgram != nullptr);
    if (!m_ResolveVBufferToGBufferProgram)
        return;

    gfxContext.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.ClearColor(g_GBufferA);
    gfxContext.ClearColor(g_GBufferB);
    gfxContext.ClearColor(g_GBufferC);
    gfxContext.ClearColor(g_GBufferD);

	ComputeContext& context = gfxContext.GetComputeContext();

	context.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	context.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	context.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*m_ResolveVBufferToGBufferProgram, context);
    binder.SetRootSignature();
    context.SetPipelineState(m_ResolveVBufferToGBufferPSO);

    const ViewConstants& viewConstants = view.GetConstants();
    ProgramVar constants = binder["g_ResolveVBufferToGBuffer"];
    constants["ViewProjMatrix"].Set(viewConstants.ViewProjMatrix);
    constants["InvViewportSize"].Set(
        DirectX::XMFLOAT2(viewConstants.InvViewportWidth, viewConstants.InvViewportHeight));
    constants["ViewportWidth"].Set(viewConstants.ViewportWidth);
    constants["ViewportHeight"].Set(viewConstants.ViewportHeight);
    constants["ViewMode"].Set((uint32_t)ViewMode);
    SetCommonResources(binder, frame);
    binder.Apply();

    context.Dispatch2D(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());
}

void Renderer::RenderVisibility(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame)
{
    ScopedTimer _profMain(L"Renderer::RenderVisibility", gfxContext);
    gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.ClearColor(g_VisibilityBuffer);

    if (DrawCommandManager::GetNumPotentialDrawItems() == 0)
    {
        return;
    }

    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxContext.SetViewportAndScissor(view.GetViewport(), view.GetScissor());

    for (auto& chunksGPU : GeometryStreaming::m_GeometryChunksGPU)
    {
        gfxContext.TransitionResource(chunksGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }

    gfxContext.TransitionResource(GeometryStreaming::m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    if (!FreezeCull)
    {
        const HZBResources hzbResources = GetSceneHZBResources();

        gfxContext.GetComputeContext().ClearBufferUAV(DrawCommandManager::GetTaskQueueStateGPU(),
                                                   DrawCommandManager::GetTaskQueueStateGPU().GetBufferSize(), 0);
        gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gfxContext.ClearUAV(DrawCommandManager::GetTaskQueueGPU(), 0xFFFFFFFF);
        gfxContext.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gfxContext.ClearUAV(DrawCommandManager::GetMeshletBatchGPU(), 0);
        gfxContext.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        uint32_t passIndex = 0;
        Renderer::InstanceCull(
            gfxContext,
            view,
            frame,
            m_InstanceCullProgram[passIndex].get(),
            m_InstanceCullPSO[passIndex],
            &hzbResources);
        Renderer::DAGCull(
            gfxContext,
            view,
            frame,
            m_DAGCullProgram[passIndex].get(),
            m_DAGCullPSO[passIndex],
            &hzbResources);
        {
            // mesh buffer gen
            gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (!BindMeshBufferGenPass(
                    gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
                return;
            gfxContext.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 0", gfxContext);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
            if (!BindVBufferMeshPass(
                    gfxContext, m_VBufferMeshProgram[passIndex], m_VBufferMeshPSO[passIndex], view, frame))
                return;
            gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

        {
            ExportDepth(gfxContext, view, frame);
        }

        {
            gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Renderer::GetCurrentHZB().GenerateHZB(gfxContext, g_SceneDepthBuffer);
        }

        passIndex = 1;
        Renderer::InstanceCull(
            gfxContext,
            view,
            frame,
            m_InstanceCullProgram[passIndex].get(),
            m_InstanceCullPSO[passIndex],
            &hzbResources);
        Renderer::DAGCull(
            gfxContext,
            view,
            frame,
            m_DAGCullProgram[passIndex].get(),
            m_DAGCullPSO[passIndex],
            &hzbResources);
        {
            // mesh buffer gen
            gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!BindMeshBufferGenPass(
                    gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
                return;
            gfxContext.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 1", gfxContext);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
            if (!BindVBufferMeshPass(
                    gfxContext, m_VBufferMeshProgram[passIndex], m_VBufferMeshPSO[passIndex], view, frame))
                return;
            gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

        {
            ExportDepth(gfxContext, view, frame);
        }

        {
            gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Renderer::GetCurrentHZB().GenerateHZB(gfxContext, g_SceneDepthBuffer);
        }
    }
    else
    {
        uint32_t passIndex = 0;
        {
            // mesh buffer gen
            gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!BindMeshBufferGenPass(
                    gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
                return;
            gfxContext.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 0", gfxContext);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
            if (!BindVBufferMeshPass(
                    gfxContext, m_VBufferMeshProgram[passIndex], m_VBufferMeshPSO[passIndex], view, frame))
                return;
            gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

        passIndex = 1;
        {
            // mesh buffer gen
            gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!BindMeshBufferGenPass(
                    gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
                return;
            gfxContext.GetComputeContext().Dispatch1D(1, 1);
        }

        {
            ScopedTimer _prof(L"Dispatch Mesh Indirect Pass 1", gfxContext);
            gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
            if (!BindVBufferMeshPass(
                    gfxContext, m_VBufferMeshProgram[passIndex], m_VBufferMeshPSO[passIndex], view, frame))
                return;
            gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);
        }

        {
            ExportDepth(gfxContext, view, frame);
        }

        {
            gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Renderer::GetCurrentHZB().GenerateHZB(gfxContext, g_SceneDepthBuffer);
        }
    }
}

void Renderer::RenderSceneDepth(
    GraphicsContext& gfxContext,
    const RenderView& view,
    const FrameConstants& frame,
    HZBResources* hzbResources,
    DepthBuffer& depthTarget)
{
    ASSERT(depthTarget.GetFormat() == SHADOW_FORMAT);

    ScopedTimer _prof0(L"Renderer::RenderSceneDepth", gfxContext);

    gfxContext.TransitionResource(depthTarget, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    gfxContext.ClearDepth(depthTarget);

    if (FreezeCull) return;

    if (DrawCommandManager::GetNumPotentialDrawItems() == 0)
    {
        return;
    }

    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxContext.SetViewportAndScissor(view.GetViewport(), view.GetScissor());

    for (auto& chunksGPU : GeometryStreaming::m_GeometryChunksGPU)
    {
        gfxContext.TransitionResource(chunksGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }

    gfxContext.TransitionResource(GeometryStreaming::m_GroupDataLocationGPU, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    gfxContext.GetComputeContext().ClearBufferUAV(DrawCommandManager::GetTaskQueueStateGPU(),
                                                  DrawCommandManager::GetTaskQueueStateGPU().GetBufferSize(), 0);
    gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gfxContext.ClearUAV(DrawCommandManager::GetTaskQueueGPU(), 0xFFFFFFFF);
    gfxContext.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gfxContext.ClearUAV(DrawCommandManager::GetMeshletBatchGPU(), 0);
    gfxContext.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        uint32_t passIndex = 0;
        Renderer::InstanceCull(
            gfxContext,
            view,
            frame,
            m_InstanceCullProgram[passIndex].get(),
            m_InstanceCullPSO[passIndex],
            hzbResources);
        Renderer::DAGCull(
            gfxContext,
            view,
            frame,
            m_DAGCullProgram[passIndex].get(),
            m_DAGCullPSO[passIndex],
            hzbResources);

        // mesh buffer gen
        gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (!BindMeshBufferGenPass(
                gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
            return;
        gfxContext.GetComputeContext().Dispatch1D(1, 1);

        ScopedTimer _prof(L"Dispatch Depth Only Mesh Pass 0", gfxContext);
        gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
        gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
        gfxContext.SetDepthStencilTarget(depthTarget.GetDSV());
        if (!BindVBufferMeshPass(
                gfxContext, m_DepthOnlyMeshProgram[passIndex], m_DepthOnlyMeshPSO[passIndex], view, frame))
            return;
        gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);

        gfxContext.TransitionResource(depthTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        hzbResources->Current->GenerateHZB(gfxContext, depthTarget);
    }

    {
        uint32_t passIndex = 1;
        Renderer::InstanceCull(
            gfxContext,
            view,
            frame,
            m_InstanceCullProgram[passIndex].get(),
            m_InstanceCullPSO[passIndex],
            hzbResources);
        Renderer::DAGCull(
            gfxContext,
            view,
            frame,
            m_DAGCullProgram[passIndex].get(),
            m_DAGCullPSO[passIndex],
            hzbResources);

        // mesh buffer gen
        gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueStateGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (!BindMeshBufferGenPass(
                gfxContext, m_MeshBufferGenProgram, m_MeshBufferGenPSO, frame, passIndex))
            return;
        gfxContext.GetComputeContext().Dispatch1D(1, 1);

        ScopedTimer _prof(L"Dispatch Depth Only Mesh Pass 1", gfxContext);
        gfxContext.TransitionResource(DrawCommandManager::GetIndirectDispatchMeshGPU(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        gfxContext.TransitionResource(DrawCommandManager::GetVisibleMeshletBufferGPU(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gfxContext.TransitionResource(depthTarget, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
        gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());
        gfxContext.SetDepthStencilTarget(depthTarget.GetDSV());
        if (!BindVBufferMeshPass(
                gfxContext, m_DepthOnlyMeshProgram[passIndex], m_DepthOnlyMeshPSO[passIndex], view, frame))
            return;
        gfxContext.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, DrawCommandManager::GetIndirectDispatchMeshGPU(), 0, 1);

        gfxContext.TransitionResource(depthTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        hzbResources->Current->GenerateHZB(gfxContext, depthTarget);
    }

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

HZBResources Renderer::GetSceneHZBResources()
{
    return MakeHZBResources(
        g_FurthestHZB,
        m_SceneHZBDescriptors,
        TemporalEffects::GetFrameIndexMod2());
}

HZBResources Renderer::GetShadowHZBResources()
{
    return MakeHZBResources(
        g_ShadowHZBBuffer,
        m_ShadowHZBDescriptors,
        TemporalEffects::GetFrameIndexMod2());
}

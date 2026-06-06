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
// Author(s):  Alex Nankervis
//             James Stanard
//

#include "LightManager.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "CommandContext.h"
#include "Camera.h"
#include "BufferManager.h"
#include "Model.h"
#include "Renderer.h"
#include "TemporalEffects.h"
#include "ConstantBuffers.h"
#include "IBL.h"
#include "ModelInstanceManager.h"
#include "../Core/GraphicsCommon.h"
#include "../Core/ProgramBinder.h"
#include "../Core/ProgramUtils.h"

using namespace Math;
using namespace Graphics;

namespace
{
    void SetCommonResources(ProgramBinder& binder, uint32_t frameIndexMod2)
    {
        ProgramVar commonResources = binder["g_CommonResources"];
        commonResources["BindlessResourcesBaseIndex"].Set(Renderer::GetBindlessResourcesBaseOffset());
        commonResources["FrameIndexMod2"].Set(frameIndexMod2);
    }

    bool InitializeFillLightGridProgram(
        ComputePSO& pso,
        std::shared_ptr<Program>& program,
        uint32_t lightGridDim,
        const char* debugName)
    {
        ProgramDesc desc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("FillLightGrid.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        desc.AddDefine("WORK_GROUP_SIZE_X", std::to_string(lightGridDim));
        desc.AddDefine("WORK_GROUP_SIZE_Y", std::to_string(lightGridDim));

        program = ProgramUtils::GetProgram(desc, debugName);
        if (!program)
            return false;

        ProgramUtils::SetProgram(pso, *program);
        pso.Finalize();
        return true;
    }
}

// must keep in sync with HLSL
struct LightData
{
    float pos[3];
    float radiusSq;
    float color[3];

    uint32_t type;
    float coneDir[3];
    float coneAngles[2];

    float shadowTextureMatrix[16];
};

enum { kMinLightGridDim = 8 };

namespace Lighting
{
    IntVar LightGridDim("Application/Forward+/Light Grid Dim", 16, kMinLightGridDim, 32, 8 );

    ComputePSO m_FillLightGridCS_8(L"Fill Light Grid 8 CS");
    ComputePSO m_FillLightGridCS_16(L"Fill Light Grid 16 CS");
    ComputePSO m_FillLightGridCS_24(L"Fill Light Grid 24 CS");
    ComputePSO m_FillLightGridCS_32(L"Fill Light Grid 32 CS");
    std::shared_ptr<Program> m_FillLightGridProgram_8;
    std::shared_ptr<Program> m_FillLightGridProgram_16;
    std::shared_ptr<Program> m_FillLightGridProgram_24;
    std::shared_ptr<Program> m_FillLightGridProgram_32;

    LightData m_LightData[MaxLights];
    StructuredBuffer m_LightBuffer;
    ByteAddressBuffer m_LightGrid;

    ByteAddressBuffer m_LightGridBitMask;
    uint32_t m_FirstConeLight;
    uint32_t m_FirstConeShadowedLight;

    enum {shadowDim = 512};
    ColorBuffer m_LightShadowArray;
    ShadowBuffer m_LightShadowTempBuffer;
    Matrix4 m_LightShadowMatrix[MaxLights];
    Math::Camera m_LightCamera[MaxLights];

    ComputePSO m_DeferredLightingPSO(L"Deferred Lighting PSO");
    std::shared_ptr<Program> m_DeferredLightingProgram;

    void InitializeResources(void);
    void CreateRandomLights(const Vector3 minBound, const Vector3 maxBound);
    void FillLightGrid(GraphicsContext& gfxContext, const Camera& camera);
    void Shutdown(void);
}

void Lighting::InitializeResources( void )
{
    if (!InitializeFillLightGridProgram(m_FillLightGridCS_8, m_FillLightGridProgram_8, 8, "Lighting: FillLightGrid 8"))
        return;
    if (!InitializeFillLightGridProgram(m_FillLightGridCS_16, m_FillLightGridProgram_16, 16, "Lighting: FillLightGrid 16"))
        return;
    if (!InitializeFillLightGridProgram(m_FillLightGridCS_24, m_FillLightGridProgram_24, 24, "Lighting: FillLightGrid 24"))
        return;
    if (!InitializeFillLightGridProgram(m_FillLightGridCS_32, m_FillLightGridProgram_32, 32, "Lighting: FillLightGrid 32"))
        return;

    // Assumes max resolution of 3840x2160
    uint32_t lightGridCells = Math::DivideByMultiple(3840, kMinLightGridDim) * Math::DivideByMultiple(2160, kMinLightGridDim);
    uint32_t lightGridSizeBytes = lightGridCells * (4 + MaxLights * 4);
    m_LightGrid.Create(L"m_LightGrid", lightGridSizeBytes, 1);

    uint32_t lightGridBitMaskSizeBytes = lightGridCells * 4 * 4;
    m_LightGridBitMask.Create(L"m_LightGridBitMask", lightGridBitMaskSizeBytes, 1);

    m_LightShadowArray.CreateArray(L"m_LightShadowArray", shadowDim, shadowDim, MaxLights, DXGI_FORMAT_R16_UNORM);
    m_LightShadowTempBuffer.Create(L"m_LightShadowTempBuffer", shadowDim, shadowDim);

    m_LightBuffer.Create(L"m_LightBuffer", MaxLights, sizeof(LightData));
    
    ProgramDesc deferredLightingDesc = ProgramUtils::MakeComputeDesc(
        Renderer::GetModelShaderPath("DeferredLighting.slang"),
        "computeMain",
        ProgramUtils::BindlessMode::ResourceHeap);

    SamplerDesc cubeMapSamplerDesc;
    cubeMapSamplerDesc.MaxAnisotropy = 8;

    SamplerDesc linearSamplerDesc;
    linearSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSamplerDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    SamplerDesc pointSamplerDesc;
    pointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointSamplerDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    deferredLightingDesc.AddStaticSampler("shadowSampler", SamplerShadowDesc);
    deferredLightingDesc.AddStaticSampler("cubeMapSampler", cubeMapSamplerDesc);
    deferredLightingDesc.AddStaticSampler("linearSampler", linearSamplerDesc);
    deferredLightingDesc.AddStaticSampler("pointSampler", pointSamplerDesc);

    m_DeferredLightingProgram = ProgramUtils::GetProgram(
        deferredLightingDesc,
        "Lighting: DeferredLighting");
    if (!m_DeferredLightingProgram)
        return;

    ProgramUtils::SetProgram(m_DeferredLightingPSO, *m_DeferredLightingProgram);
    m_DeferredLightingPSO.Finalize();
    
    {
		Renderer::SetBindlessResourceDescriptor(SRV_LIGHT_BUFFER, m_LightBuffer.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_LIGHT_SHADOW_ARRAY, m_LightShadowArray.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_LIGHT_GRID, m_LightGrid.GetSRV());
		Renderer::SetBindlessResourceDescriptor(SRV_LIGHT_GRID_MASK, m_LightGridBitMask.GetSRV());

		Renderer::SetBindlessResourceDescriptor(UAV_LIGHT_GRID, m_LightGrid.GetUAV());
		Renderer::SetBindlessResourceDescriptor(UAV_LIGHT_GRID_MASK, m_LightGridBitMask.GetUAV());
    }
}

void Lighting::CreateRandomLights( const Vector3 minBound, const Vector3 maxBound )
{
    Vector3 posScale = maxBound - minBound;
    Vector3 posBias = minBound;

    // todo: replace this with MT
    srand(12645);
    auto randUint = []() -> uint32_t
    {
        return rand(); // [0, RAND_MAX]
    };
    auto randFloat = [randUint]() -> float
    {
        return randUint() * (1.0f / RAND_MAX); // convert [0, RAND_MAX] to [0, 1]
    };
    auto randVecUniform = [randFloat]() -> Vector3
    {
        return Vector3(randFloat(), randFloat(), randFloat());
    };
    auto randGaussian = [randFloat]() -> float
    {
        // polar box-muller
        static bool gaussianPair = true;
        static float y2;

        if (gaussianPair)
        {
            gaussianPair = false;

            float x1, x2, w;
            do
            {
                x1 = 2 * randFloat() - 1;
                x2 = 2 * randFloat() - 1;
                w = x1 * x1 + x2 * x2;
            } while (w >= 1);

            w = sqrtf(-2 * logf(w) / w);
            y2 = x2 * w;
            return x1 * w;
        }
        else
        {
            gaussianPair = true;
            return y2;
        }
    };
    auto randVecGaussian = [randGaussian]() -> Vector3
    {
        return Normalize(Vector3(randGaussian(), randGaussian(), randGaussian()));
    };

    const float pi = 3.14159265359f;
    for (uint32_t n = 0; n < MaxLights; n++)
    {
        Vector3 pos = randVecUniform() * posScale + posBias;
        float lightRadius = randFloat() * 8.0f + 2.0f;

        Vector3 color = randVecUniform();
        float colorScale = randFloat() * .3f + .3f;
        color = color * colorScale;

        uint32_t type;
        // force types to match 32-bit boundaries for the BIT_MASK_SORTED case
        if (n < 32 * 1)
            type = 0;
        else if (n < 32 * 3)
            type = 1;
        else
            type = 2;

        type = 2;

        Vector3 coneDir = randVecGaussian();
        float coneInner = (randFloat() * .2f + .025f) * pi;
        float coneOuter = coneInner + randFloat() * .1f * pi;

        if (type == 1 || type == 2)
        {
            // emphasize cone lights
            color = color * 5.0f;
        }

        Math::Camera shadowCamera;
        shadowCamera.SetEyeAtUp(pos, pos + coneDir, Vector3(0, 1, 0));
        shadowCamera.SetPerspectiveMatrix(coneOuter * 2, 1.0f, lightRadius * .05f, lightRadius * 1.0f);
        shadowCamera.Update();
        m_LightCamera[n] = shadowCamera;
        m_LightShadowMatrix[n] = shadowCamera.GetViewProjMatrix();
        Matrix4 shadowTextureMatrix = Matrix4(AffineTransform(Matrix3::MakeScale( 0.5f, -0.5f, 1.0f ), Vector3(0.5f, 0.5f, 0.0f))) * m_LightShadowMatrix[n];

        m_LightData[n].pos[0] = pos.GetX();
        m_LightData[n].pos[1] = pos.GetY();
        m_LightData[n].pos[2] = pos.GetZ();
        m_LightData[n].radiusSq = lightRadius * lightRadius;
        m_LightData[n].color[0] = color.GetX();
        m_LightData[n].color[1] = color.GetY();
        m_LightData[n].color[2] = color.GetZ();
        m_LightData[n].type = type;
        m_LightData[n].coneDir[0] = coneDir.GetX();
        m_LightData[n].coneDir[1] = coneDir.GetY();
        m_LightData[n].coneDir[2] = coneDir.GetZ();
        m_LightData[n].coneAngles[0] = 1.0f / (cosf(coneInner) - cosf(coneOuter));
        m_LightData[n].coneAngles[1] = cosf(coneOuter);
        std::memcpy(m_LightData[n].shadowTextureMatrix, &shadowTextureMatrix, sizeof(shadowTextureMatrix));
        //*(Matrix4*)(m_LightData[n].shadowTextureMatrix) = shadowTextureMatrix;
    }
    // sort lights by type, needed for efficiency in the BIT_MASK approach
    /*	{
    Matrix4 copyLightShadowMatrix[MaxLights];
    memcpy(copyLightShadowMatrix, m_LightShadowMatrix, sizeof(Matrix4) * MaxLights);
    LightData copyLightData[MaxLights];
    memcpy(copyLightData, m_LightData, sizeof(LightData) * MaxLights);

    uint32_t sortArray[MaxLights];
    for (uint32_t n = 0; n < MaxLights; n++)
    {
    sortArray[n] = n;
    }
    std::sort(sortArray, sortArray + MaxLights,
    [this](const uint32_t &a, const uint32_t &b) -> bool
    {
    return this->m_LightData[a].type < this->m_LightData[b].type;
    });
    for (uint32_t n = 0; n < MaxLights; n++)
    {
    m_LightShadowMatrix[n] = copyLightShadowMatrix[sortArray[n]];
    m_LightData[n] = copyLightData[sortArray[n]];
    }
    }*/
    for (uint32_t n = 0; n < MaxLights; n++)
    {
        if (m_LightData[n].type == 1)
        {
            m_FirstConeLight = n;
            break;
        }
    }
    for (uint32_t n = 0; n < MaxLights; n++)
    {
        if (m_LightData[n].type == 2)
        {
            m_FirstConeShadowedLight = n;
            break;
        }
    }

    CommandContext::InitializeBuffer(m_LightBuffer, m_LightData, MaxLights * sizeof(LightData));
}

void Lighting::Shutdown(void)
{
    m_LightBuffer.Destroy();
    m_LightGrid.Destroy();
    m_LightGridBitMask.Destroy();
    m_LightShadowArray.Destroy();
    m_LightShadowTempBuffer.Destroy();
}

void Lighting::FillLightGrid(GraphicsContext& gfxContext, const Camera& camera)
{
    ScopedTimer _prof(L"FillLightGrid", gfxContext);

    ComputeContext& Context = gfxContext.GetComputeContext();

	Context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
    Context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ComputePSO* pso = nullptr;
    std::shared_ptr<Program>* program = nullptr;
    switch ((int)LightGridDim)
    {
    case  8: pso = &m_FillLightGridCS_8;  program = &m_FillLightGridProgram_8;  break;
    case 16: pso = &m_FillLightGridCS_16; program = &m_FillLightGridProgram_16; break;
    case 24: pso = &m_FillLightGridCS_24; program = &m_FillLightGridProgram_24; break;
    case 32: pso = &m_FillLightGridCS_32; program = &m_FillLightGridProgram_32; break;
    default: ASSERT(false); break;
    }

    ASSERT(pso != nullptr);
    ASSERT(program != nullptr && *program != nullptr);
    if (pso == nullptr || program == nullptr || !*program)
        return;

    ProgramBinder binder(**program, Context);
    binder.SetRootSignature();
    Context.SetPipelineState(*pso);

    ColorBuffer& LinearDepth = g_LinearDepth[ TemporalEffects::GetFrameIndexMod2() ];

    Context.TransitionResource(m_LightBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(LinearDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightGrid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(m_LightGridBitMask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // todo: assumes 1920x1080 resolution
    uint32_t tileCountX = Math::DivideByMultiple(g_SceneColorBuffer.GetWidth(), LightGridDim);
    uint32_t tileCountY = Math::DivideByMultiple(g_SceneColorBuffer.GetHeight(), LightGridDim);

    float FarClipDist = camera.GetFarClip();
    float NearClipDist = camera.GetNearClip();
    const float RcpZMagic = NearClipDist / (FarClipDist - NearClipDist);

    // todo: assumes 1920x1080 resolution
    ProgramVar constants = binder["g_FillLightGrid"];
    constants["ViewWidth"].Set(g_SceneColorBuffer.GetWidth());
    constants["ViewHeight"].Set(g_SceneColorBuffer.GetHeight());
    constants["InvTileDimf32"].Set(1.0f / LightGridDim);
    constants["RcpZMagic"].Set(RcpZMagic);
    constants["TileCountX"].Set(tileCountX);
    constants["ViewProjMatrix"].Set(camera.GetViewProjMatrix());
    SetCommonResources(binder, TemporalEffects::GetFrameIndexMod2());
    binder.Apply();

    Context.Dispatch(tileCountX, tileCountY, 1);

    Context.TransitionResource(m_LightBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightGrid, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightGridBitMask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Lighting::RenderDeferredLighting(GraphicsContext& gfxContext,
    const GlobalConstants& globals)
{
    ScopedTimer _prof(L"DeferredLighting", gfxContext);

    ASSERT(m_DeferredLightingProgram != nullptr);
    if (!m_DeferredLightingProgram)
        return;

    ComputeContext& Context = gfxContext.GetComputeContext();

    Context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
    Context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, Renderer::s_SamplerHeap.GetHeapPointer());

    ProgramBinder binder(*m_DeferredLightingProgram, Context);
    binder.SetRootSignature();

    Context.SetPipelineState(m_DeferredLightingPSO);

    Context.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_ShadowBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.TransitionResource(m_LightBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightGrid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightGridBitMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(m_LightShadowArray, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.TransitionResource(g_IBLDiffuseLDMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_IBLSpecularLDMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_IBLLut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

   
    ProgramVar constants = binder["g_DeferredLighting"];
    constants["InverseViewProjMatrix"].Set(globals.InverseViewProjMatrix);
    constants["SunShadowMatrix"].Set(globals.SunShadowMatrix);
    constants["ViewerPos"].Set(globals.ViewerPos);
    constants["SunDirection"].Set(globals.SunDirection);
    constants["SunIntensity"].Set(globals.SunIntensity);
    constants["ShadowTexelSize"].Set(DirectX::XMFLOAT4(
        globals.ShadowTexelSize[0],
        globals.ShadowTexelSize[1],
        globals.ShadowTexelSize[2],
        globals.ShadowTexelSize[3]));
    constants["InvTileDim"].Set(DirectX::XMFLOAT4(
        globals.InvTileDim[0],
        globals.InvTileDim[1],
        globals.InvTileDim[2],
        globals.InvTileDim[3]));
    constants["ViewportWidth"].Set(globals.ViewportWidth);
    constants["ViewportHeight"].Set(globals.ViewportHeight);
    constants["TileCountX"].Set(globals.TileCount[0]);
    constants["IBLSpecularLDMapMipCount"].Set(globals.IBLSpecularLDMapMipCount);
    SetCommonResources(binder, globals.FrameIndexMod2);
    binder.Apply();

    uint32_t groupCountX = Math::DivideByMultiple(g_SceneColorBuffer.GetWidth(), 8);
    uint32_t groupCountY = Math::DivideByMultiple(g_SceneColorBuffer.GetHeight(), 8);

    Context.Dispatch(groupCountX, groupCountY, 1);
}

void Lighting::RenderLightShadows(GraphicsContext& gfxContext, const GlobalConstants& globals)
{
    using namespace Renderer;
    ScopedTimer _prof(L"RenderLightShadows", gfxContext);

    gfxContext.TransitionResource(m_LightShadowArray, D3D12_RESOURCE_STATE_COPY_DEST);

    for (uint32_t LightIndex = 0; LightIndex < MaxLights; ++LightIndex)
    {
        if (m_LightData[LightIndex].type == 2)
        {
            std::wstring passName = L"LightIndex: " + std::to_wstring(LightIndex);
            ScopedTimer _profShadow(passName, gfxContext);
            MeshSorter shadowSorter(MeshSorter::kShadows);
            shadowSorter.SetCamera(m_LightCamera[LightIndex]);
            shadowSorter.SetDepthStencilTarget(m_LightShadowTempBuffer);
            ModelInstanceManager::Render(shadowSorter);
            shadowSorter.Sort();
            shadowSorter.RenderMeshes(MeshSorter::kZPass, gfxContext, globals);
            
            gfxContext.TransitionResource(m_LightShadowTempBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gfxContext.CopySubresource(m_LightShadowArray, LightIndex, m_LightShadowTempBuffer, 0);
        }
    }
}

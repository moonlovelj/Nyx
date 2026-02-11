//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
//

#include "pch.h"
#include "XeGTAO.h"
#include "BufferManager.h"
#include "GraphicsCore.h"
#include "CommandContext.h"
#include "Camera.h"
#include "TemporalEffects.h"
#include "CompiledShaders/XeGTAOPrefilterCS.h"
#include "CompiledShaders/XeGTAOMainCS.h"
#include "CompiledShaders/XeGTAODenoiseCS.h"
#include "CompiledShaders/XeGTAOResolveCS.h"


using namespace Graphics;
using namespace Math;

namespace XeGTAO
{
    BoolVar Enable("Graphics/XeGTAO/Enable", true);

    const char* kQualityLabels[] = { "Low", "Medium", "High", "Ultra" };
    EnumVar g_QualityLevel("Graphics/XeGTAO/Quality", 4, 4, kQualityLabels);
    IntVar g_DenoisePasses("Graphics/XeGTAO/Denoise Passes", 3, 0, 3);

    NumVar g_EffectRadius("Graphics/XeGTAO/Radius", 0.5f, 0.05f, 5.0f, 0.05f);
    NumVar g_RadiusMultiplier("Graphics/XeGTAO/Radius Multiplier", 1.457f, 0.3f, 3.0f, 0.01f);
    NumVar g_FalloffRange("Graphics/XeGTAO/Falloff Range", 0.615f, 0.0f, 1.0f, 0.01f);
    NumVar g_SampleDistributionPower("Graphics/XeGTAO/Sample Distribution Power", 2.0f, 1.0f, 3.0f, 0.05f);
    NumVar g_ThinOccluderCompensation("Graphics/XeGTAO/Thin Occluder Compensation", 0.0f, 0.0f, 0.7f, 0.01f);
    NumVar g_FinalValuePower("Graphics/XeGTAO/Final Value Power", 2.2f, 0.5f, 5.0f, 0.05f);
    NumVar g_DepthMipSamplingOffset("Graphics/XeGTAO/Depth Mip Sampling Offset", 3.3f, 0.0f, 8.0f, 0.05f);
}

namespace
{
    static constexpr uint32_t kDepthMipCount = 5;
    static constexpr uint32_t kNumThreadsX = 8;
    static constexpr uint32_t kNumThreadsY = 8;

    struct XeGTAOConstants
    {
        uint32_t ViewportSize[2];
        float InvViewportSize[2];

        float DepthUnpackConsts[2];
        float CameraTanHalfFov[2];

        float NdcToViewMul[2];
        float NdcToViewAdd[2];

        float NdcToViewMulTimesPixelSize[2];
        float EffectRadius;
        float EffectFalloffRange;
        float RadiusMultiplier;

        float FinalValuePower;
        float DenoiseBlurBeta;
        float SampleDistributionPower;
        float ThinOccluderCompensation;
        float DepthMipSamplingOffset;

        uint32_t NoiseIndex;
        float Padding0;
    };
    static_assert(sizeof(XeGTAOConstants) == 96, "XeGTAO constant buffer layout must match HLSL.");

    RootSignature s_RootSignature;
    ComputePSO s_PrefilterDepthCS(L"XeGTAO: Prefilter Depth CS");
    ComputePSO s_MainPassCS(L"XeGTAO: Main Pass CS");
    ComputePSO s_DenoiseCS(L"XeGTAO: Denoise CS");
    ComputePSO s_ResolveCS(L"XeGTAO: Resolve CS");

    ColorBuffer s_WorkingDepth;
    ColorBuffer s_WorkingEdges;
    ColorBuffer s_WorkingAOTerm[2];

    uint32_t s_Width = 0;
    uint32_t s_Height = 0;

    void EnsureResources(uint32_t width, uint32_t height)
    {
        if (s_Width == width && s_Height == height)
        {
            return;
        }

        s_Width = width;
        s_Height = height;

        s_WorkingDepth.Create(L"XeGTAO Working Depth", width, height, kDepthMipCount, DXGI_FORMAT_R16_FLOAT);
        s_WorkingEdges.Create(L"XeGTAO Working Edges", width, height, 1, DXGI_FORMAT_R8_UNORM);
        s_WorkingAOTerm[0].Create(L"XeGTAO Working AO A", width, height, 1, DXGI_FORMAT_R8_UINT);
        s_WorkingAOTerm[1].Create(L"XeGTAO Working AO B", width, height, 1, DXGI_FORMAT_R8_UINT);
    }

    void GetQualitySampleBudget(int qualityLevel, float& outSliceCount, float& outStepsPerSlice)
    {
        switch (qualityLevel)
        {
        case 0: outSliceCount = 1.0f; outStepsPerSlice = 2.0f; break;
        case 1: outSliceCount = 2.0f; outStepsPerSlice = 2.0f; break;
        case 3: outSliceCount = 9.0f; outStepsPerSlice = 3.0f; break;
        case 2:
        default:
            outSliceCount = 3.0f;
            outStepsPerSlice = 3.0f;
            break;
        }
    }

    void UpdateConstants(XeGTAOConstants& outConstants, uint32_t width, uint32_t height, const float* proj, uint32_t frameIndex)
    {
        outConstants.ViewportSize[0] = width;
        outConstants.ViewportSize[1] = height;
        outConstants.InvViewportSize[0] = 1.0f / (float)width;
        outConstants.InvViewportSize[1] = 1.0f / (float)height;

        float depthLinearizeMul = -proj[3 * 4 + 2];
        float depthLinearizeAdd = proj[2 * 4 + 2];
        if (depthLinearizeMul * depthLinearizeAdd < 0.0f)
        {
            depthLinearizeAdd = -depthLinearizeAdd;
        }

        float tanHalfFovY = 1.0f / proj[1 * 4 + 1];
        float tanHalfFovX = 1.0f / proj[0 * 4 + 0];

        outConstants.DepthUnpackConsts[0] = depthLinearizeMul;
        outConstants.DepthUnpackConsts[1] = depthLinearizeAdd;
        outConstants.CameraTanHalfFov[0] = tanHalfFovX;
        outConstants.CameraTanHalfFov[1] = tanHalfFovY;

        outConstants.NdcToViewMul[0] = tanHalfFovX * 2.0f;
        outConstants.NdcToViewMul[1] = tanHalfFovY * -2.0f;
        outConstants.NdcToViewAdd[0] = tanHalfFovX * -1.0f;
        outConstants.NdcToViewAdd[1] = tanHalfFovY * 1.0f;

        outConstants.NdcToViewMulTimesPixelSize[0] = outConstants.NdcToViewMul[0] * outConstants.InvViewportSize[0];
        outConstants.NdcToViewMulTimesPixelSize[1] = outConstants.NdcToViewMul[1] * outConstants.InvViewportSize[1];

        outConstants.EffectRadius = XeGTAO::g_EffectRadius;
        outConstants.EffectFalloffRange = XeGTAO::g_FalloffRange;
        outConstants.RadiusMultiplier = XeGTAO::g_RadiusMultiplier;
        outConstants.FinalValuePower = XeGTAO::g_FinalValuePower;
        outConstants.SampleDistributionPower = XeGTAO::g_SampleDistributionPower;
        outConstants.ThinOccluderCompensation = XeGTAO::g_ThinOccluderCompensation;
        outConstants.DepthMipSamplingOffset = XeGTAO::g_DepthMipSamplingOffset;
        outConstants.DenoiseBlurBeta = (XeGTAO::g_DenoisePasses == 0) ? 1e4f : 1.2f;
        const bool useTemporalNoise = (bool)TemporalEffects::EnableTAA;
        outConstants.NoiseIndex = (XeGTAO::g_DenoisePasses > 0 && useTemporalNoise) ? (frameIndex % 64) : 0;

        outConstants.Padding0 = 0.0f;
    }
}

void XeGTAO::Initialize( void )
{
    s_RootSignature.Reset(4, 1);
    s_RootSignature.InitStaticSampler(0, SamplerPointClampDesc);
    s_RootSignature[0].InitAsConstantBuffer(0);
    s_RootSignature[1].InitAsConstants(1, 2);
    s_RootSignature[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 5);
    s_RootSignature[3].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 5);
    s_RootSignature.Finalize(L"XeGTAO");

#define CreatePSO( ObjName, ShaderByteCode ) \
    ObjName.SetRootSignature(s_RootSignature); \
    ObjName.SetComputeShader(ShaderByteCode, sizeof(ShaderByteCode)); \
    ObjName.Finalize()

    CreatePSO(s_PrefilterDepthCS, g_pXeGTAOPrefilterCS);
    CreatePSO(s_MainPassCS, g_pXeGTAOMainCS);
    CreatePSO(s_DenoiseCS, g_pXeGTAODenoiseCS);
    CreatePSO(s_ResolveCS, g_pXeGTAOResolveCS);
}

void XeGTAO::Shutdown( void )
{
    s_WorkingDepth.Destroy();
    s_WorkingEdges.Destroy();
    s_WorkingAOTerm[0].Destroy();
    s_WorkingAOTerm[1].Destroy();
    s_Width = 0;
    s_Height = 0;
}

void XeGTAO::Render( GraphicsContext& GfxContext, const Camera& camera )
{
    if (!Enable)
    {
        ScopedTimer _prof(L"Generate XeGTAO", GfxContext);
        GfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        GfxContext.ClearColor(g_SSAOFullScreen);
        GfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return;
    }

    const uint32_t width = g_SceneDepthBuffer.GetWidth();
    const uint32_t height = g_SceneDepthBuffer.GetHeight();
    EnsureResources(width, height);

    XeGTAOConstants constants = {};
    const float* proj = reinterpret_cast<const float*>(&camera.GetProjMatrix());
    UpdateConstants(constants, width, height, proj, TemporalEffects::GetFrameIndex());

    float sliceCount = 3.0f;
    float stepsPerSlice = 3.0f;
    GetQualitySampleBudget(g_QualityLevel, sliceCount, stepsPerSlice);

    ScopedTimer _prof(L"Generate XeGTAO", GfxContext);
    ComputeContext& Context = GfxContext.GetComputeContext();
    Context.SetRootSignature(s_RootSignature);
    Context.SetDynamicConstantBufferView(0, sizeof(constants), &constants);

    {
        ScopedTimer _prefilter(L"XeGTAO Prefilter", Context);
        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(s_WorkingDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.SetPipelineState(s_PrefilterDepthCS);

        D3D12_CPU_DESCRIPTOR_HANDLE depthUAVs[kDepthMipCount] =
        {
            s_WorkingDepth.GetUAV(0),
            s_WorkingDepth.GetUAV(1),
            s_WorkingDepth.GetUAV(2),
            s_WorkingDepth.GetUAV(3),
            s_WorkingDepth.GetUAV(4)
        };
        Context.SetDynamicDescriptors(2, 0, kDepthMipCount, depthUAVs);
        Context.SetDynamicDescriptor(3, 0, g_SceneDepthBuffer.GetDepthSRV());
        Context.Dispatch2D(width, height, 16, 16);
    }

    {
        ScopedTimer _main(L"XeGTAO Main Pass", Context);
        Context.TransitionResource(s_WorkingDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(s_WorkingAOTerm[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.TransitionResource(s_WorkingEdges, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.SetPipelineState(s_MainPassCS);
        Context.SetConstants(1, sliceCount, stepsPerSlice);

        D3D12_CPU_DESCRIPTOR_HANDLE mainUAVs[] = { s_WorkingAOTerm[0].GetUAV(), s_WorkingEdges.GetUAV() };
        Context.SetDynamicDescriptors(2, 0, _countof(mainUAVs), mainUAVs);
        Context.SetDynamicDescriptor(3, 0, s_WorkingDepth.GetSRV());
        Context.Dispatch2D(width, height, kNumThreadsX, kNumThreadsY);
    }

    ColorBuffer* aoRead = &s_WorkingAOTerm[0];
    ColorBuffer* aoWrite = &s_WorkingAOTerm[1];
    const int passCount = std::max(1, (int)g_DenoisePasses);

    for (int passIndex = 0; passIndex < passCount; ++passIndex)
    {
        ScopedTimer _denoise(L"XeGTAO Denoise", Context);
        const bool lastPass = (passIndex == passCount - 1);

        Context.TransitionResource(*aoRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(s_WorkingEdges, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(*aoWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.SetPipelineState(s_DenoiseCS);
        Context.SetConstants(1, lastPass ? 1.0f : 0.0f, 0.0f);

        D3D12_CPU_DESCRIPTOR_HANDLE denoiseUAV = aoWrite->GetUAV();
        D3D12_CPU_DESCRIPTOR_HANDLE denoiseSRVs[] = { aoRead->GetSRV(), s_WorkingEdges.GetSRV() };
        Context.SetDynamicDescriptors(2, 0, 1, &denoiseUAV);
        Context.SetDynamicDescriptors(3, 0, _countof(denoiseSRVs), denoiseSRVs);
        Context.Dispatch2D(width, height, kNumThreadsX * 2, kNumThreadsY);

        std::swap(aoRead, aoWrite);
    }

    {
        ScopedTimer _resolve(L"XeGTAO Resolve", Context);
        Context.TransitionResource(*aoRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.SetPipelineState(s_ResolveCS);

        D3D12_CPU_DESCRIPTOR_HANDLE resolveUAV = g_SSAOFullScreen.GetUAV();
        Context.SetDynamicDescriptors(2, 0, 1, &resolveUAV);
        Context.SetDynamicDescriptor(3, 0, aoRead->GetSRV());
        Context.Dispatch2D(width, height, 8, 8);
    }

    Context.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}



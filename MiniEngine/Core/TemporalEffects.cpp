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
//

#include "pch.h"
#include "TemporalEffects.h"
#include "BufferManager.h"
#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "CommandContext.h"
#include "SystemTime.h"
#include "PostEffects.h"

#include "CompiledShaders/TemporalBlendCS.h"
#include "CompiledShaders/ResolveTAACS.h"
#include "CompiledShaders/SharpenTAACS.h"

using namespace Graphics;
using namespace Math;
using namespace TemporalEffects;

namespace TemporalEffects
{
    BoolVar EnableTAA("Graphics/AA/TAA/Enable", false);
    NumVar Sharpness("Graphics/AA/TAA/Sharpness", 0.2f, 0.0f, 1.0f, 0.05f);
    ExpVar TemporalSpeedLimit("Graphics/AA/TAA/Speed Limit", 64.0f, 1.0f, 1024.0f, 1.0f);
    BoolVar TriggerReset("Graphics/AA/TAA/Reset", true);

    ComputePSO s_TemporalBlendCS(L"TAA: Temporal Blend CS");
    ComputePSO s_SharpenTAACS(L"TAA: Sharpen TAA CS");
    ComputePSO s_ResolveTAACS(L"TAA: Resolve TAA CS");

    uint32_t s_FrameIndex = 0;
    uint32_t s_FrameIndexMod2 = 0;
    float s_JitterX = 0.0f;
    float s_JitterY = 0.0f;
    float s_JitterDeltaX = 0.0f;
    float s_JitterDeltaY = 0.0f;
    bool s_HistoryValid = false;

    void ApplyTemporalAA(ComputeContext& Context);
    void SharpenImage(ComputeContext& Context, ColorBuffer& TemporalColor);
}

void TemporalEffects::Initialize( void )
{
#define CreatePSO( ObjName, ShaderByteCode ) \
    ObjName.SetRootSignature(g_CommonRS); \
    ObjName.SetComputeShader(ShaderByteCode, sizeof(ShaderByteCode) ); \
    ObjName.Finalize();

    CreatePSO( s_TemporalBlendCS, g_pTemporalBlendCS );
    CreatePSO( s_SharpenTAACS, g_pSharpenTAACS );
    CreatePSO( s_ResolveTAACS, g_pResolveTAACS );

#undef CreatePSO
}

void TemporalEffects::Shutdown( void )
{
}

void TemporalEffects::Update( uint64_t FrameIndex )
{
    s_FrameIndex = (uint32_t)FrameIndex;
    s_FrameIndexMod2 = s_FrameIndex % 2;

    if (EnableTAA)
    {
        static const float Halton23[8][2] =
        {
            { 0.0f / 8.0f, 0.0f / 9.0f }, { 4.0f / 8.0f, 3.0f / 9.0f },
            { 2.0f / 8.0f, 6.0f / 9.0f }, { 6.0f / 8.0f, 1.0f / 9.0f },
            { 1.0f / 8.0f, 4.0f / 9.0f }, { 5.0f / 8.0f, 7.0f / 9.0f },
            { 3.0f / 8.0f, 2.0f / 9.0f }, { 7.0f / 8.0f, 5.0f / 9.0f }
        };

        const float* Offset = Halton23[s_FrameIndex % 8];

        s_JitterX = Offset[0] - 0.5f;
        s_JitterY = Offset[1] - 0.5f;
    }
    else
    {
        s_JitterX = 0.0f;
        s_JitterY = 0.0f;
    }

}

uint32_t TemporalEffects::GetFrameIndexMod2( void )
{
    return s_FrameIndexMod2;
}

uint32_t TemporalEffects::GetFrameIndex(void)
{
    return s_FrameIndex;
}

void TemporalEffects::GetJitterOffset( float& JitterX, float& JitterY )
{
    JitterX = EnableTAA ? s_JitterX : 0.0f;
    JitterY = EnableTAA ? s_JitterY : 0.0f;
}

void TemporalEffects::ClearHistory( CommandContext& Context )
{
    s_HistoryValid = false;
    TriggerReset = true;
    GraphicsContext& gfxContext = Context.GetGraphicsContext();

    if (EnableTAA)
    {
        gfxContext.TransitionResource(g_TemporalColor[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        gfxContext.TransitionResource(g_TemporalColor[1], D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        gfxContext.ClearColor(g_TemporalColor[0]);
        gfxContext.ClearColor(g_TemporalColor[1]);
    }
}

bool TemporalEffects::PrepareFrame(CommandContext& Context, bool historyValid)
{
    static bool s_EnableTAA = false;
    historyValid = historyValid && EnableTAA == s_EnableTAA && !TriggerReset;
    if (!historyValid)
        ClearHistory(Context);
    s_EnableTAA = EnableTAA;
    TriggerReset = false;
    s_HistoryValid = historyValid;
    return historyValid;
}

void TemporalEffects::ResolveImage( CommandContext& BaseContext, float jitterDeltaX, float jitterDeltaY )
{
    ScopedTimer _prof(L"Temporal Resolve", BaseContext);
    ComputeContext& Context = BaseContext.GetComputeContext();
    s_JitterDeltaX = s_HistoryValid ? jitterDeltaX : 0.0f;
    s_JitterDeltaY = s_HistoryValid ? jitterDeltaY : 0.0f;

    if (!EnableTAA)
    {
        return;
    }

    uint32_t Src = s_FrameIndexMod2;
    uint32_t Dst = Src ^ 1;

    {
        ApplyTemporalAA(Context);
        SharpenImage(Context, g_TemporalColor[Dst]);
    }
}

void TemporalEffects::ApplyTemporalAA(ComputeContext& Context)
{
    ScopedTimer _prof(L"Resolve Image", Context);

    uint32_t Src = s_FrameIndexMod2;
    uint32_t Dst = Src ^ 1;

    Context.SetRootSignature(g_CommonRS);
    Context.SetPipelineState(s_TemporalBlendCS);

    __declspec(align(16)) struct ConstantBuffer
    {
        float RcpBufferDim[2];
        float RcpSpeedLimiter;
        uint32_t HistoryValid;
        float JitterDelta[2];
        uint32_t Padding[2];
    };
    static_assert(sizeof(ConstantBuffer) == 32, "TemporalBlendCS constant buffer layout must match HLSL.");
    ConstantBuffer cbv = {
        1.0f / g_SceneColorBuffer.GetWidth(), 1.0f / g_SceneColorBuffer.GetHeight(),
        1.0f / TemporalSpeedLimit, s_HistoryValid ? 1u : 0u,
        { s_JitterDeltaX, s_JitterDeltaY }, {}
    };

    Context.SetDynamicConstantBufferView(3, sizeof(cbv), &cbv);

    Context.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_TemporalColor[Src], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_TemporalColor[Dst], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(g_LinearDepth[Src], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_LinearDepth[Dst], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.SetDynamicDescriptor(1, 0, g_VelocityBuffer.GetSRV());
    Context.SetDynamicDescriptor(1, 1, g_SceneColorBuffer.GetSRV());
    Context.SetDynamicDescriptor(1, 2, g_TemporalColor[Src].GetSRV());
    Context.SetDynamicDescriptor(1, 3, g_LinearDepth[Src].GetSRV());
    Context.SetDynamicDescriptor(1, 4, g_LinearDepth[Dst].GetSRV());
    Context.SetDynamicDescriptor(2, 0, g_TemporalColor[Dst].GetUAV());

    Context.Dispatch2D(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight(), 16, 8);
}

void TemporalEffects::SharpenImage(ComputeContext& Context, ColorBuffer& TemporalColor)
{
    ScopedTimer _prof(L"Sharpen or Copy Image", Context);

    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(TemporalColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.SetPipelineState(Sharpness >= 0.001f ? s_SharpenTAACS : s_ResolveTAACS);
    Context.SetConstants(0, 1.0f + Sharpness, 0.25f * Sharpness);
    Context.SetDynamicDescriptor(1, 0, TemporalColor.GetSRV());
    Context.SetDynamicDescriptor(2, 0, g_SceneColorBuffer.GetUAV());
    Context.Dispatch2D(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());
}

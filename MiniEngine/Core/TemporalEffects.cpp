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
#include <cmath>

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

    TemporalFrameState s_Frame;
    TemporalFrameState s_SubmittedFrame;
    ResetReason s_PendingReset = ResetReason::None;
    bool s_HasSubmittedFrame = false;
    bool s_FramePrepared = false;

    float Halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float weight = 1.0f;
        while (index > 0)
        {
            weight /= base;
            result += weight * (index % base);
            index /= base;
        }
        return result;
    }

    void ClearTaaHistory(CommandContext& context)
    {
        if (s_Frame.mode != Mode::TAA)
            return;
        GraphicsContext& graphics = context.GetGraphicsContext();
        for (ColorBuffer& history : g_TemporalColor)
        {
            graphics.TransitionResource(history, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
            graphics.ClearColor(history);
        }
    }

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
    s_Frame = {};
    s_SubmittedFrame = {};
    s_PendingReset = ResetReason::None;
    s_HasSubmittedFrame = false;
    s_FramePrepared = false;
}

void TemporalEffects::BeginFrame(uint64_t frameIndex, Mode mode, const FrameResolution& resolution)
{
    s_Frame = {};
    s_Frame.frameIndex = frameIndex;
    s_Frame.mode = mode;
    s_Frame.resolution = resolution;
    s_FramePrepared = false;
    s_Frame.resetReasons = s_HasSubmittedFrame ? ResetReason::None : ResetReason::FirstFrame;
    if (s_HasSubmittedFrame)
    {
        if (frameIndex != s_SubmittedFrame.frameIndex + 1)
            s_Frame.resetReasons = s_Frame.resetReasons | ResetReason::RenderInterrupted;
        if (mode != s_SubmittedFrame.mode)
            s_Frame.resetReasons = s_Frame.resetReasons | ResetReason::ModeChanged;
        if (resolution.renderSize != s_SubmittedFrame.resolution.renderSize ||
            resolution.outputSize != s_SubmittedFrame.resolution.outputSize)
            s_Frame.resetReasons = s_Frame.resetReasons | ResetReason::ResolutionChanged;
    }

    if (mode == Mode::TAA)
    {
        static const float Halton23[8][2] =
        {
            { 0.0f / 8.0f, 0.0f / 9.0f }, { 4.0f / 8.0f, 3.0f / 9.0f },
            { 2.0f / 8.0f, 6.0f / 9.0f }, { 6.0f / 8.0f, 1.0f / 9.0f },
            { 1.0f / 8.0f, 4.0f / 9.0f }, { 5.0f / 8.0f, 7.0f / 9.0f },
            { 3.0f / 8.0f, 2.0f / 9.0f }, { 7.0f / 8.0f, 5.0f / 9.0f }
        };
        const float* offset = Halton23[frameIndex % 8];
        s_Frame.sequenceLength = 8;
        s_Frame.jitter = {offset[0] - 0.5f, offset[1] - 0.5f};
    }
    else if (UsesDlss() && resolution.renderSize.IsValid() && resolution.outputSize.IsValid())
    {
        const double renderPixels = double(resolution.renderSize.width) * resolution.renderSize.height;
        const double outputPixels = double(resolution.outputSize.width) * resolution.outputSize.height;
        s_Frame.sequenceLength = static_cast<uint32_t>(std::max(1.0, std::ceil(8.0 * outputPixels / renderPixels)));
        const uint32_t sample = static_cast<uint32_t>(frameIndex % s_Frame.sequenceLength) + 1;
        s_Frame.jitter = {Halton(sample, 2) - 0.5f, Halton(sample, 3) - 0.5f};
    }
    s_Frame.previousJitter = s_SubmittedFrame.jitter;
}

const TemporalEffects::TemporalFrameState& TemporalEffects::GetFrameState() { return s_Frame; }
bool TemporalEffects::UsesDlss() { return s_Frame.mode >= Mode::DLAA; }
bool TemporalEffects::IsTemporalReconstructionEnabled() { return s_Frame.mode == Mode::TAA || UsesDlss(); }
uint32_t TemporalEffects::GetFrameIndexMod2() { return static_cast<uint32_t>(s_Frame.frameIndex % 2); }
uint32_t TemporalEffects::GetFrameIndex() { return static_cast<uint32_t>(s_Frame.frameIndex); }

void TemporalEffects::GetJitterOffset(float& x, float& y)
{
    x = s_Frame.jitter.x;
    y = s_Frame.jitter.y;
}

void TemporalEffects::InvalidateHistory(ResetReason reason)
{
    s_PendingReset = s_PendingReset | reason;
}

void TemporalEffects::ClearHistory(CommandContext& context)
{
    InvalidateHistory(ResetReason::Manual);
    s_Frame.historyValid = false;
    s_Frame.previousJitter = s_Frame.jitter;
    ClearTaaHistory(context);
}

bool TemporalEffects::PrepareFrame(CommandContext& context, ResetReason reasons)
{
    s_Frame.resetReasons = s_Frame.resetReasons | s_PendingReset | reasons;
    if (TriggerReset)
        s_Frame.resetReasons = s_Frame.resetReasons | ResetReason::Manual;
    s_PendingReset = ResetReason::None;
    TriggerReset = false;
    s_Frame.historyValid = s_Frame.resetReasons == ResetReason::None;
    if (!s_Frame.historyValid)
    {
        s_Frame.previousJitter = s_Frame.jitter;
        ClearTaaHistory(context);
    }
    s_FramePrepared = true;
    return s_Frame.historyValid;
}

void TemporalEffects::CommitFrame()
{
    ASSERT(s_FramePrepared);
    s_SubmittedFrame = s_Frame;
    s_HasSubmittedFrame = true;
    s_FramePrepared = false;
}

void TemporalEffects::ResolveImage(CommandContext& baseContext)
{
    if (s_Frame.mode != Mode::TAA)
        return;

    ScopedTimer timer(L"Temporal Resolve", baseContext);
    ComputeContext& context = baseContext.GetComputeContext();
    ApplyTemporalAA(context);
    SharpenImage(context, g_TemporalColor[GetFrameIndexMod2() ^ 1]);
}

void TemporalEffects::ApplyTemporalAA(ComputeContext& Context)
{
    ScopedTimer _prof(L"Resolve Image", Context);

    uint32_t Src = GetFrameIndexMod2();
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
        1.0f / TemporalSpeedLimit, s_Frame.historyValid ? 1u : 0u,
        { s_Frame.previousJitter.x - s_Frame.jitter.x, s_Frame.previousJitter.y - s_Frame.jitter.y }, {}
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

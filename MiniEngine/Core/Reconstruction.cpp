#include "pch.h"
#include "Reconstruction.h"
#include "DlssBackend.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "PostEffects.h"
#include "FXAA.h"
#include "imgui.h"
#include "CompiledShaders/ReconstructionFallbackCS.h"
#include <array>
#include <cmath>

namespace Reconstruction
{
    using namespace Graphics;
    using TemporalEffects::Mode;
    namespace
    {
        std::unique_ptr<DlssBackend::Context> s_Backend;
        ReconstructionSettings s_Settings;
        Status s_Status;
        PixelSize s_Output = {}, s_Render = {};
        std::array<PixelSize, 8> s_SupportedSizes = {};
        bool s_Dirty = true;
        bool s_Failed = false;
        RootSignature s_FallbackRoot;
        ComputePSO s_FallbackPSO(L"Reconstruction: Spatial Fallback");
        const char* const ModeNames[] = {"Off", "FXAA", "TAA", "DLAA", "DLSS Quality",
            "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance"};

        void SpatialFallback()
        {
            auto& context = ComputeContext::Begin(L"Reconstruction Spatial Fallback");
            context.SetRootSignature(s_FallbackRoot);
            context.SetPipelineState(s_FallbackPSO);
            context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(g_ReconstructionOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.SetDynamicDescriptor(0, 0, g_SceneColorBuffer.GetSRV());
            context.SetDynamicDescriptor(1, 0, g_ReconstructionOutput.GetUAV());
            context.Dispatch2D(g_ReconstructionOutput.GetWidth(), g_ReconstructionOutput.GetHeight());
            context.Finish();
        }
    }

    void Initialize()
    {
        if (s_Backend)
            return;
        s_Backend = std::make_unique<DlssBackend::Context>();
        s_Settings = {};
        s_Status = {};
        s_Dirty = true;
        s_Failed = false;
        s_Output = s_Render = {};
        s_SupportedSizes = {};
        s_FallbackRoot.Reset(2, 1);
        s_FallbackRoot[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
        s_FallbackRoot[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
        s_FallbackRoot.InitStaticSampler(0, SamplerLinearClampDesc);
        s_FallbackRoot.Finalize(L"Reconstruction Spatial Fallback");
        s_FallbackPSO.SetRootSignature(s_FallbackRoot);
        s_FallbackPSO.SetComputeShader(g_pReconstructionFallbackCS, sizeof(g_pReconstructionFallbackCS));
        s_FallbackPSO.Finalize();
    }

    void Shutdown()
    {
        if (s_Backend)
            s_Backend->Shutdown();
        s_Backend.reset();
    }

    void SetSettings(const ReconstructionSettings& settings)
    {
        if (settings.mode < Mode::Off || settings.mode > Mode::DLSSUltraPerformance ||
            !std::isfinite(settings.materialMipBiasOffset))
            return;
        if (s_Settings.mode != settings.mode)
            s_Dirty = true;
        if (s_Settings.materialMipBiasOffset != settings.materialMipBiasOffset)
            TemporalEffects::InvalidateHistory(TemporalEffects::ResetReason::Manual);
        s_Settings = settings;
    }

    const ReconstructionSettings& GetSettings() { return s_Settings; }
    const Status& GetStatus() { return s_Status; }
    bool IsInitialized() { return s_Backend != nullptr; }

    float GetMaterialMipBias()
    {
        const auto& frame = TemporalEffects::GetFrameState();
        if (!TemporalEffects::UsesDlss() || !frame.resolution.renderSize.IsValid() ||
            !frame.resolution.outputSize.IsValid())
            return 0.0f;
        return std::log2(float(frame.resolution.renderSize.width) / frame.resolution.outputSize.width)
            - 1.0f + s_Settings.materialMipBiasOffset;
    }

    PixelSize PrepareFrame(PixelSize output, bool nativeDebugView)
    {
        ASSERT(s_Backend != nullptr);
        if (nativeDebugView != s_Status.nativeDebugView)
        {
            s_Status.nativeDebugView = nativeDebugView;
            s_Dirty = true;
            TemporalEffects::InvalidateHistory(TemporalEffects::ResetReason::DebugViewChanged);
        }
        const Mode effectiveMode = nativeDebugView ? Mode::Off : s_Settings.mode;
        if (s_Dirty || output != s_Output)
        {
            s_Failed = false;
            s_Status.reason.clear();
            // Keep the UI's capability results when switching between native modes.
            if (output != s_Output || effectiveMode >= Mode::DLAA)
                for (int i = int(Mode::DLAA); i <= int(Mode::DLSSUltraPerformance); ++i)
                    s_SupportedSizes[i] = s_Backend->QueryRenderSize(static_cast<Mode>(i), output);
            s_Output = output;
            s_Render = effectiveMode >= Mode::DLAA ? s_SupportedSizes[int(effectiveMode)] : PixelSize{};
            if (effectiveMode >= Mode::DLAA &&
                (!s_Render.IsValid() || !s_Backend->Configure(effectiveMode, {s_Render, output})))
            {
                s_Failed = true;
                s_Status.reason = s_Backend->GetStatus().reason;
                if (s_Status.reason.empty())
                    s_Status.reason = "This DLSS mode is unavailable at the selected output size.";
            }
            s_Dirty = false;
        }
        s_Status.spatialFallback = false;
        s_Status.actualMode = s_Failed ? Mode::TAA : effectiveMode;
        if (s_Status.actualMode < Mode::DLAA)
            s_Backend->ReleaseFeature();
        // Keep legacy controls/consumers consistent with this frame's mutually exclusive mode.
        TemporalEffects::EnableTAA = s_Status.actualMode == Mode::TAA;
        FXAA::Enable = s_Status.actualMode == Mode::FXAA;
        return s_Status.actualMode >= Mode::DLAA ? s_Render : PixelSize{};
    }

    void Render(bool hasScene)
    {
        if (!s_Backend || s_Status.actualMode < Mode::DLAA)
            return;
        if (!hasScene)
        {
            SpatialFallback();
            s_Status.spatialFallback = true;
            return;
        }
        if (!s_Backend->Evaluate(g_SceneColorBuffer, g_SceneDepthBuffer, g_ReconstructionVelocity,
            PostEffects::GetExposureTexture(), g_ReconstructionOutput,
            TemporalEffects::GetFrameState(), GetFrameTime()))
        {
            s_Failed = true;
            s_Status.reason = s_Backend->GetStatus().reason;
            s_Status.spatialFallback = true;
            // Finish this frame at the allocated output size; switch modes at the next frame boundary.
            SpatialFallback();
            TemporalEffects::InvalidateHistory(TemporalEffects::ResetReason::RenderInterrupted);
        }
    }

    void DrawSettings()
    {
        if (!s_Backend)
            return;
        if (ImGui::BeginCombo("Anti-aliasing", ModeNames[int(s_Settings.mode)]))
        {
            for (int i = 0; i < int(_countof(ModeNames)); ++i)
            {
                if (i >= int(Mode::DLAA) && !s_SupportedSizes[i].IsValid())
                    continue;
                if (ImGui::Selectable(ModeNames[i], i == int(s_Settings.mode)))
                {
                    auto settings = s_Settings;
                    settings.mode = static_cast<Mode>(i);
                    SetSettings(settings);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Text("Active: %s", s_Status.spatialFallback ? "Spatial fallback" : ModeNames[int(s_Status.actualMode)]);
        if (s_Status.nativeDebugView)
            ImGui::TextWrapped("Debug view uses native resolution with AA off. The selected mode resumes in Lit view.");
        if (s_Settings.mode >= Mode::DLAA)
        {
            auto settings = s_Settings;
            if (ImGui::SliderFloat("Material Mip Bias Offset", &settings.materialMipBiasOffset, -2.0f, 2.0f))
                SetSettings(settings);
            ImGui::Text("Active material mip bias: %.2f", GetMaterialMipBias());
        }
        const auto& reason = s_Status.reason.empty() ? s_Backend->GetStatus().reason : s_Status.reason;
        if (!reason.empty())
            ImGui::TextWrapped("%s", reason.c_str());
        if (s_Failed && ImGui::Button("Retry DLSS"))
        {
            s_Backend->Shutdown();
            s_Dirty = true;
        }
    }
}

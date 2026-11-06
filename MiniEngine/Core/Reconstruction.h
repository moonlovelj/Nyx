#pragma once

#include "TemporalEffects.h"
#include <string>

struct ReconstructionSettings
{
    TemporalEffects::Mode mode = TemporalEffects::Mode::DLSSQuality;
    float materialMipBiasOffset = 0.0f;
};

namespace Reconstruction
{
    struct Status
    {
        TemporalEffects::Mode actualMode = TemporalEffects::Mode::TAA;
        bool spatialFallback = false;
        bool nativeDebugView = false;
        std::string reason;
    };

    void Initialize();
    void Shutdown();
    bool IsInitialized();
    void SetSettings(const ReconstructionSettings& settings);
    const ReconstructionSettings& GetSettings();
    const Status& GetStatus();
    // Before Display::ApplyFrameResolution, camera updates and temporal BeginFrame.
    PixelSize PrepareFrame(PixelSize output, bool nativeDebugView = false);
    float GetMaterialMipBias();
    // After submitting the scene. Writes the output even on failure or an empty scene.
    void Render(bool hasScene = true);
    void DrawSettings();
}

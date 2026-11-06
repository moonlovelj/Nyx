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

#pragma once

#include "EngineTuning.h"
#include "Display.h"

class CommandContext;

namespace TemporalEffects
{
    enum class Mode
    {
        Off, FXAA, TAA, DLAA, DLSSQuality, DLSSBalanced, DLSSPerformance, DLSSUltraPerformance
    };

    enum class ResetReason : uint32_t
    {
        None = 0,
        FirstFrame = 1 << 0,
        ModeChanged = 1 << 1,
        ResolutionChanged = 1 << 2,
        CameraCut = 1 << 3,
        ProjectionChanged = 1 << 4,
        SceneReloaded = 1 << 5,
        RenderInterrupted = 1 << 6,
        Manual = 1 << 7,
        DebugViewChanged = 1 << 8
    };

    inline ResetReason operator|(ResetReason a, ResetReason b)
    {
        return static_cast<ResetReason>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    struct JitterOffset
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct TemporalFrameState
    {
        uint64_t frameIndex = 0;
        FrameResolution resolution = {};
        Mode mode = Mode::Off;
        JitterOffset jitter;
        JitterOffset previousJitter;
        uint32_t sequenceLength = 1;
        bool historyValid = false;
        ResetReason resetReasons = ResetReason::FirstFrame;
    };

    extern BoolVar EnableTAA;

    void Initialize();
    void Shutdown();

    // Begin before camera/view updates. Only CommitFrame advances submitted history.
    void BeginFrame(uint64_t frameIndex, Mode mode, const FrameResolution& resolution);
    bool PrepareFrame(CommandContext& context, ResetReason reasons = ResetReason::None);
    void CommitFrame();
    void InvalidateHistory(ResetReason reason);
    const TemporalFrameState& GetFrameState();
    bool IsTemporalReconstructionEnabled();
    bool UsesDlss();

    uint32_t GetFrameIndexMod2();
    uint32_t GetFrameIndex();

    // Centered render-pixel offsets, positive right/down, shared by projection and reconstruction.
    void GetJitterOffset(float& x, float& y);
    void ClearHistory(CommandContext& context);
    void ResolveImage(CommandContext& context);
}

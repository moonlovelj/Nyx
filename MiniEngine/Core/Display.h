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

#include <cstdint>

class ColorBuffer;

struct PixelSize
{
    uint32_t width, height;

    bool IsValid() const { return width != 0 && height != 0; }
    bool operator==(PixelSize other) const { return width == other.width && height == other.height; }
    bool operator!=(PixelSize other) const { return !(*this == other); }
};

struct FrameResolution
{
    PixelSize renderSize;
    PixelSize outputSize;
};

namespace Display
{
    void Initialize(void);
    // Apply the selected output resolution and window style, including before graphics startup.
    void ConfigureWindow(void);
    void Shutdown(void);
    // Window messages only record the latest non-minimized client size.
    void Resize(uint32_t width, uint32_t height);
    PixelSize GetRequestedOutputSize();
    // An empty reconstruction size renders at the output resolution.
    void ApplyFrameResolution(PixelSize outputSize, PixelSize reconstructionRenderSize);
    const FrameResolution& GetFrameResolution();
    void Present(ColorBuffer& frameColor);
}

namespace Graphics
{
    extern uint32_t g_DisplayWidth;
    extern uint32_t g_DisplayHeight;
    extern bool g_bEnableHDROutput;

    // Returns the number of elapsed frames since application start
    uint64_t GetFrameCount(void);

    // The amount of time elapsed during the last completed frame.  The CPU and/or
    // GPU may be idle during parts of the frame.  The frame time measures the time
    // between calls to present each frame.
    float GetFrameTime(void);

    // The total number of frames per second
    float GetFrameRate(void);

    extern bool g_bEnableHDROutput;
}

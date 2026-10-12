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

class CommandContext;

namespace TemporalEffects
{
    // Temporal antialiasing involves jittering sample positions and accumulating color over time to 
    // effectively supersample the image.
    extern BoolVar EnableTAA;
    extern BoolVar EnableIntelHistoryClipping;

    void Initialize( void );

    void Shutdown( void );

    // Call once per frame to increment the internal frame counter and, in the case of TAA, choosing the next
    // jittered sample position.
    void Update( uint64_t FrameIndex );

    // Returns whether the frame is odd or even, relevant to checkerboard rendering.
    uint32_t GetFrameIndexMod2( void );

    uint32_t GetFrameIndex(void);

    // Centered pixel offsets [-0.5, 0.5), positive right/down. Disabled TAA returns zero.
    void GetJitterOffset( float& JitterX, float& JitterY );

    void ClearHistory(CommandContext& Context);

    // Call before rendering/culling. Returns false when all temporal consumers must reset.
    bool PrepareFrame(CommandContext& Context, bool historyValid);

    // Delta is previous submitted jitter minus current jitter, in pixels.
    void ResolveImage(CommandContext& Context, float jitterDeltaX = 0.0f, float jitterDeltaY = 0.0f);

}

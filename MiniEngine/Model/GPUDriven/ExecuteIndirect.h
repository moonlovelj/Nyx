#pragma once

#include "../Core/CommandSignature.h"
#include "../Core/GpuBuffer.h"

#include <d3d12.h>

namespace GPUDriven
{

    extern BoolVar Enable;

    struct IndirectCommand
    {
        D3D12_DRAW_INDEXED_ARGUMENTS drawArguments;
    };

    struct IndirectArgsBufferWarp
    {
        std::shared_ptr<IndirectArgsBuffer> indirectArgsBuffer;
		uint32_t numCommands;
    };

    extern CommandSignature GPUDrivenDrawIndirectCommandSignature;

    void Initialize(const RootSignature* RootSignature);
    void Shutdown();
    void DrawIndirect(GraphicsContext& context, const IndirectArgsBufferWarp& IndirectArgsBufferWarp);
}
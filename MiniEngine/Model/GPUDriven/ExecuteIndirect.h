#pragma once

#include "../Core/CommandSignature.h"
#include "../Core/GpuBuffer.h"

#include <d3d12.h>

namespace GPUDriven
{

    extern BoolVar Enable;

    struct IndirectCommand
    {
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress;
        D3D12_DRAW_INDEXED_ARGUMENTS drawArguments;
    };

    extern CommandSignature GPUDrivenDrawIndirectCommandSignature;

    void Initialize(const RootSignature* RootSignature);
    void Shutdown();
    void DrawIndirect(GraphicsContext& context, GpuBuffer& ArgumentBuffer, uint32_t MaxCommands, uint64_t ArgumentStartOffset);
}
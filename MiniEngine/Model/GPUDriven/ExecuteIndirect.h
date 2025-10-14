#pragma once

#include "../Core/CommandSignature.h"
#include "../Core/GpuBuffer.h"

#include <d3d12.h>

namespace GPUDriven
{
    __declspec(align(16)) struct IndirectCommand
    {
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress;
        D3D12_DRAW_INDEXED_ARGUMENTS drawArguments;
    };

	static_assert(alignof(IndirectCommand) >= 16, "IndirectCommand must be >=16B aligned");
	static_assert(sizeof(IndirectCommand) % 16 == 0, "IndirectCommand size must be multiple of 16");

    extern CommandSignature GPUDrivenDrawIndirectCommandSignature;

    void Initialize(const RootSignature* RootSignature);
    void Shutdown();
    void DrawIndirect(GraphicsContext& context, GpuBuffer& ArgumentBuffer, uint32_t MaxCommands, uint64_t ArgumentStartOffset);
}
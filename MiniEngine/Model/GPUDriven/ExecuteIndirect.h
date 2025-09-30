#pragma once

#include "../Core/CommandSignature.h"
#include "../Core/GpuBuffer.h"

#include <d3d12.h>

namespace GPUDriven
{

    extern BoolVar Enable;

    struct IndirectCommand
    {
		D3D12_GPU_VIRTUAL_ADDRESS meshCBAddress;
		D3D12_GPU_VIRTUAL_ADDRESS materialCBAddress;
		D3D12_GPU_VIRTUAL_ADDRESS meshJointsAddress;
		D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
		D3D12_INDEX_BUFFER_VIEW indexBufferView;
        D3D12_DRAW_INDEXED_ARGUMENTS drawArguments;
    };

    extern CommandSignature GPUDrivenDrawIndirectCommandSignature;

    void Initialize(const RootSignature* RootSignature);
    void Shutdown();
    void DrawIndirect(GraphicsContext& context, GpuBuffer& ArgumentBuffer, uint32_t MaxCommands, uint64_t ArgumentStartOffset);
}
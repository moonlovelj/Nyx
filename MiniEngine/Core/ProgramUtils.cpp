//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"
#include "ProgramUtils.h"

#include "PipelineState.h"
#include "ProgramManager.h"
#include "Utility.h"

namespace ProgramUtils
{
    D3D12_ROOT_SIGNATURE_FLAGS GetBindlessRootSignatureFlags(BindlessMode bindlessMode)
    {
        switch (bindlessMode)
        {
        case BindlessMode::None:
            return D3D12_ROOT_SIGNATURE_FLAG_NONE;
        case BindlessMode::ResourceHeap:
            return D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        case BindlessMode::ResourceAndSamplerHeap:
            return D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        default:
            ASSERT(false);
            return D3D12_ROOT_SIGNATURE_FLAG_NONE;
        }
    }

    ProgramDesc& AddBindlessRootSignatureFlags(ProgramDesc& desc, BindlessMode bindlessMode)
    {
        const D3D12_ROOT_SIGNATURE_FLAGS flags = GetBindlessRootSignatureFlags(bindlessMode);
        if (flags != D3D12_ROOT_SIGNATURE_FLAG_NONE)
            desc.AddRootSignatureFlags(flags);
        return desc;
    }

    ProgramDesc MakeGraphicsDesc(
        const std::string& sourceFile,
        const std::string& vertexEntry,
        const std::string& pixelEntry,
        const std::string& meshEntry,
        BindlessMode bindlessMode)
    {
        ASSERT(!sourceFile.empty());
        ASSERT(!vertexEntry.empty() || !meshEntry.empty());
        ASSERT(vertexEntry.empty() || meshEntry.empty());

        ProgramDesc desc(sourceFile);
        if (!meshEntry.empty())
            desc.AddEntryPoint(ShaderStage::Mesh, meshEntry);
        if (!vertexEntry.empty())
            desc.AddEntryPoint(ShaderStage::Vertex, vertexEntry);
        if (!pixelEntry.empty())
            desc.AddEntryPoint(ShaderStage::Pixel, pixelEntry);

        AddBindlessRootSignatureFlags(desc, bindlessMode);
        return desc;
    }

    ProgramDesc MakeGraphicsDesc(
        const std::string& sourceFile,
        const std::string& vertexEntry,
        const std::string& pixelEntry,
        BindlessMode bindlessMode)
    {
        return MakeGraphicsDesc(sourceFile, vertexEntry, pixelEntry, "", bindlessMode);
    }

    ProgramDesc MakeComputeDesc(
        const std::string& sourceFile,
        const std::string& computeEntry,
        BindlessMode bindlessMode)
    {
        ASSERT(!sourceFile.empty());
        ASSERT(!computeEntry.empty());

        ProgramDesc desc(sourceFile);
        desc.AddEntryPoint(ShaderStage::Compute, computeEntry);
        AddBindlessRootSignatureFlags(desc, bindlessMode);
        return desc;
    }

    std::shared_ptr<Program> GetProgram(
        const ProgramDesc& desc,
        const char* debugName)
    {
        std::string buildLog;
        std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
        if (!program)
        {
            Utility::Printf("[%s] Slang program build failed:\n", debugName);
            Utility::Print(buildLog.c_str());

            ASSERT(false);
        }
        return program;
    }

    void SetProgram(GraphicsPSO& pso, const Program& program)
    {
        ASSERT(program.HasBytecode(ShaderStage::Vertex));
        pso.SetRootSignature(program.GetRootSignature());

        if (program.HasBytecode(ShaderStage::Vertex))
            pso.SetVertexShader(program.GetD3D12Bytecode(ShaderStage::Vertex));
        if (program.HasBytecode(ShaderStage::Pixel))
            pso.SetPixelShader(program.GetD3D12Bytecode(ShaderStage::Pixel));
        if (program.HasBytecode(ShaderStage::Geometry))
            pso.SetGeometryShader(program.GetD3D12Bytecode(ShaderStage::Geometry));
        if (program.HasBytecode(ShaderStage::Hull))
            pso.SetHullShader(program.GetD3D12Bytecode(ShaderStage::Hull));
        if (program.HasBytecode(ShaderStage::Domain))
            pso.SetDomainShader(program.GetD3D12Bytecode(ShaderStage::Domain));
    }

    void SetProgram(MeshShaderPSO& pso, const Program& program)
    {
        ASSERT(program.HasBytecode(ShaderStage::Mesh));
        pso.SetRootSignature(program.GetRootSignature());

        if (program.HasBytecode(ShaderStage::Mesh))
        {
            const Program::Bytecode& bytecode = program.GetBytecode(ShaderStage::Mesh);
            pso.SetMeshShader(bytecode.Data.data(), bytecode.Data.size());
        }

        if (program.HasBytecode(ShaderStage::Pixel))
        {
            const Program::Bytecode& bytecode = program.GetBytecode(ShaderStage::Pixel);
            pso.SetPixelShader(bytecode.Data.data(), bytecode.Data.size());
        }
    }

    void SetProgram(ComputePSO& pso, const Program& program)
    {
        ASSERT(program.HasBytecode(ShaderStage::Compute));
        pso.SetRootSignature(program.GetRootSignature());
        if (program.HasBytecode(ShaderStage::Compute))
            pso.SetComputeShader(program.GetD3D12Bytecode(ShaderStage::Compute));
    }
}

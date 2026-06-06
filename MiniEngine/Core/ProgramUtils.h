//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include "pch.h"
#include "Program.h"

class ComputePSO;
class GraphicsPSO;
class MeshShaderPSO;

namespace ProgramUtils
{
    enum class BindlessMode
    {
        None,
        ResourceHeap,
        ResourceAndSamplerHeap,
    };

    ProgramDesc MakeGraphicsDesc(
        const std::string& sourceFile,
        const std::string& vertexEntry = "",
        const std::string& pixelEntry = "",
        const std::string& meshEntry = "",
        BindlessMode bindlessMode = BindlessMode::None);

    ProgramDesc MakeGraphicsDesc(
        const std::string& sourceFile,
        const std::string& vertexEntry,
        const std::string& pixelEntry,
        BindlessMode bindlessMode);

    ProgramDesc MakeComputeDesc(
        const std::string& sourceFile,
        const std::string& computeEntry,
        BindlessMode bindlessMode = BindlessMode::None);

    D3D12_ROOT_SIGNATURE_FLAGS GetBindlessRootSignatureFlags(BindlessMode bindlessMode);
    ProgramDesc& AddBindlessRootSignatureFlags(ProgramDesc& desc, BindlessMode bindlessMode);

    std::shared_ptr<Program> GetProgram(
        const ProgramDesc& desc,
        const char* debugName);

    void SetProgram(GraphicsPSO& pso, const Program& program);
    void SetProgram(MeshShaderPSO& pso, const Program& program);
    void SetProgram(ComputePSO& pso, const Program& program);
}

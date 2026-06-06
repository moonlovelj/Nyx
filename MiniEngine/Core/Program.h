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
#include "ProgramDesc.h"
#include "ProgramReflection.h"
#include "RootSignature.h"

#include <array>

class Program
{
    friend class ProgramManager;

public:
    struct Bytecode
    {
        std::vector<uint8_t> Data;

        bool IsValid() const { return !Data.empty(); }

        D3D12_SHADER_BYTECODE GetD3D12Bytecode() const
        {
            D3D12_SHADER_BYTECODE bytecode = {};
            bytecode.pShaderBytecode = Data.empty() ? nullptr : Data.data();
            bytecode.BytecodeLength = Data.size();
            return bytecode;
        }
    };

    Program(const ProgramDesc& desc, uint64_t versionId, const std::string& buildLog)
        : m_Desc(desc), m_VersionId(versionId), m_BuildLog(buildLog)
    {
    }

    const ProgramDesc& GetDesc() const { return m_Desc; }
    uint64_t GetVersionId() const { return m_VersionId; }
    const std::string& GetBuildLog() const { return m_BuildLog; }
    const ProgramReflection& GetReflection() const { return m_Reflection; }
    const RootSignature& GetRootSignature() const { return m_RootSignature; }

    const ProgramBinding* FindBinding(const std::string& name) const
    {
        return m_Reflection.FindBinding(name);
    }

    const ProgramParameter* FindParameter(const std::string& path) const
    {
        return m_Reflection.FindParameter(path);
    }

    bool HasBytecode(ShaderStage stage) const
    {
        return GetBytecode(stage).IsValid();
    }

    const Bytecode& GetBytecode(ShaderStage stage) const
    {
        ASSERT(stage < ShaderStage::Count);
        return m_Bytecode[static_cast<size_t>(stage)];
    }

    D3D12_SHADER_BYTECODE GetD3D12Bytecode(ShaderStage stage) const
    {
        return GetBytecode(stage).GetD3D12Bytecode();
    }

private:
    void SetBuildLog(const std::string& buildLog)
    {
        m_BuildLog = buildLog;
    }

    ProgramReflection& GetMutableReflection()
    {
        return m_Reflection;
    }

    RootSignature& GetMutableRootSignature()
    {
        return m_RootSignature;
    }

    void SetBytecode(ShaderStage stage, const void* data, size_t size)
    {
        ASSERT(stage < ShaderStage::Count);
        Bytecode& bytecode = m_Bytecode[static_cast<size_t>(stage)];
        bytecode.Data.resize(size);
        if (size > 0)
            memcpy(bytecode.Data.data(), data, size);
    }

    ProgramDesc m_Desc;
    uint64_t m_VersionId = 0;
    std::string m_BuildLog;
    ProgramReflection m_Reflection;
    RootSignature m_RootSignature;
    std::array<Bytecode, static_cast<size_t>(ShaderStage::Count)> m_Bytecode;
};

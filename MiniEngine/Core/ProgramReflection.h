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

#include <unordered_map>

class ProgramDesc;

namespace slang
{
    struct ShaderReflection;
}

enum class ProgramBindingKind : uint8_t
{
    ConstantBuffer,
    RootConstants,
    SRV,
    UAV,
    RootBufferSRV,
    RootBufferUAV,
    Sampler,
    StaticSampler
};

// A physical D3D binding range. Resource arrays and arrays of resource-bearing
// structs share one range per reflected resource leaf.
struct ProgramBinding
{
    std::string Name;
    ProgramBindingKind Kind = ProgramBindingKind::SRV;
    uint32_t Register = 0;
    uint32_t Space = 0;
    uint32_t Count = 1;
    uint32_t RootIndex = UINT_MAX;
    uint32_t TableOffset = 0;
    uint32_t SizeInBytes = 0;
    uint32_t Num32BitValues = 0;

    bool IsDescriptorTableBinding() const
    {
        return Kind == ProgramBindingKind::SRV ||
            Kind == ProgramBindingKind::UAV ||
            Kind == ProgramBindingKind::Sampler;
    }
};

enum class ProgramParameterKind : uint8_t
{
    Root,
    Struct,
    Array,
    ConstantBuffer,
    ParameterBlock,
    Uniform,
    SRV,
    UAV,
    Sampler
};

enum class ProgramMatrixLayout : uint8_t
{
    None,
    RowMajor,
    ColumnMajor
};

// Runtime cursor offset. ResourceArrayIndex follows Falcor's flattened array
// model and is independent of whether the resource is an SRV, UAV, or sampler.
struct ProgramVarOffset
{
    uint32_t Uniform = 0;
    uint32_t ResourceArrayIndex = 0;
};

// A node in the logical shader parameter tree. Offsets are relative to the
// parent node; entering a constant-buffer/parameter-block starts a new uniform
// and resource address domain.
struct ProgramParameter
{
    std::string Name;
    std::string Path;
    ProgramParameterKind Kind = ProgramParameterKind::Uniform;
    uint32_t ParentIndex = UINT_MAX;
    uint32_t ElementParameterIndex = UINT_MAX;
    uint32_t BindingIndex = UINT_MAX;
    uint32_t UniformBindingIndex = UINT_MAX;
    uint32_t RelativeUniformOffset = 0;
    // Contiguous byte span that may be written without touching a following field.
    uint32_t UniformSize = 0;
    // Aligned type stride used for standalone/container allocation, not field writes.
    uint32_t UniformTypeStride = 0;
    uint32_t ArrayCount = 0;
    uint32_t UniformStride = 0;
    uint32_t ScalarType = 0;
    uint32_t RowCount = 0;
    uint32_t ColumnCount = 0;
    ProgramMatrixLayout MatrixLayout = ProgramMatrixLayout::None;
    uint32_t MatrixVectorStride = 0;
    std::vector<uint32_t> Children;
    std::unordered_map<std::string, uint32_t> ChildIndicesByName;
};

class ProgramReflection
{
public:
    static constexpr uint32_t RootParameterIndex = 0;

    void Clear()
    {
        m_Bindings.clear();
        m_BindingIndicesByName.clear();
        m_Parameters.clear();
        m_UsesDescriptorHeapIndexing = false;

        ProgramParameter root;
        root.Kind = ProgramParameterKind::Root;
        root.Path = "$Root";
        m_Parameters.push_back(root);
    }

    const ProgramBinding* FindBinding(const std::string& name) const
    {
        auto iter = m_BindingIndicesByName.find(name);
        if (iter == m_BindingIndicesByName.end())
            return nullptr;
        return &m_Bindings[iter->second];
    }

    bool HasBinding(const std::string& name) const { return FindBinding(name) != nullptr; }

    const ProgramParameter* FindParameter(const std::string& path) const
    {
        const uint32_t parameterIndex = FindParameterIndex(path);
        return parameterIndex == UINT_MAX ? nullptr : &m_Parameters[parameterIndex];
    }

    uint32_t FindParameterIndex(const std::string& path) const
    {
        if (m_Parameters.empty())
            return UINT_MAX;
        if (path.empty() || path == "$Root")
            return RootParameterIndex;

        uint32_t parameterIndex = RootParameterIndex;
        size_t nameStart = 0;
        while (nameStart < path.size())
        {
            size_t nameEnd = path.find('.', nameStart);
            if (nameEnd == std::string::npos)
                nameEnd = path.size();

            const std::string name = path.substr(nameStart, nameEnd - nameStart);
            if (name.empty())
                return UINT_MAX;

            const ProgramParameter* parameter = &m_Parameters[parameterIndex];
            auto childIter = parameter->ChildIndicesByName.find(name);
            while (childIter == parameter->ChildIndicesByName.end() &&
                parameter->Kind == ProgramParameterKind::Array &&
                parameter->ElementParameterIndex < m_Parameters.size())
            {
                parameterIndex = parameter->ElementParameterIndex;
                parameter = &m_Parameters[parameterIndex];
                childIter = parameter->ChildIndicesByName.find(name);
            }
            if (childIter == parameter->ChildIndicesByName.end())
                return UINT_MAX;

            parameterIndex = childIter->second;
            nameStart = nameEnd + 1;
        }
        return parameterIndex;
    }

    const ProgramParameter* GetParameter(uint32_t parameterIndex) const
    {
        return parameterIndex < m_Parameters.size() ? &m_Parameters[parameterIndex] : nullptr;
    }

    ProgramParameter* GetMutableParameter(uint32_t parameterIndex)
    {
        return parameterIndex < m_Parameters.size() ? &m_Parameters[parameterIndex] : nullptr;
    }

    const std::vector<ProgramBinding>& GetBindings() const { return m_Bindings; }
    const std::vector<ProgramParameter>& GetParameters() const { return m_Parameters; }
    size_t GetBindingCount() const { return m_Bindings.size(); }
    bool UsesDescriptorHeapIndexing() const { return m_UsesDescriptorHeapIndexing; }

    // These mutation helpers are intended for the reflection builder. Duplicate
    // names are rejected instead of silently replacing an existing layout.
    void SetUsesDescriptorHeapIndexing(bool value) { m_UsesDescriptorHeapIndexing = value; }

    uint32_t AddBinding(const ProgramBinding& binding)
    {
        if (binding.Name.empty() || m_BindingIndicesByName.find(binding.Name) != m_BindingIndicesByName.end())
            return UINT_MAX;

        const uint32_t bindingIndex = static_cast<uint32_t>(m_Bindings.size());
        m_BindingIndicesByName[binding.Name] = bindingIndex;
        m_Bindings.push_back(binding);
        return bindingIndex;
    }

    uint32_t AddParameter(uint32_t parentIndex, const ProgramParameter& parameter)
    {
        if (parentIndex >= m_Parameters.size())
            return UINT_MAX;

        ProgramParameter& parent = m_Parameters[parentIndex];
        if (!parameter.Name.empty() &&
            parent.ChildIndicesByName.find(parameter.Name) != parent.ChildIndicesByName.end())
        {
            return UINT_MAX;
        }

        const uint32_t parameterIndex = static_cast<uint32_t>(m_Parameters.size());
        m_Parameters.push_back(parameter);
        m_Parameters[parameterIndex].ParentIndex = parentIndex;
        m_Parameters[parentIndex].Children.push_back(parameterIndex);
        if (!parameter.Name.empty())
            m_Parameters[parentIndex].ChildIndicesByName[parameter.Name] = parameterIndex;
        return parameterIndex;
    }

private:
    friend class ProgramManager;

    std::vector<ProgramBinding>& GetMutableBindings() { return m_Bindings; }

    std::vector<ProgramBinding> m_Bindings;
    std::unordered_map<std::string, size_t> m_BindingIndicesByName;
    std::vector<ProgramParameter> m_Parameters;
    bool m_UsesDescriptorHeapIndexing = false;
};

bool ReflectProgramLayout(
    const ProgramDesc& desc,
    slang::ShaderReflection* layout,
    ProgramReflection& reflection,
    std::string& buildLog);

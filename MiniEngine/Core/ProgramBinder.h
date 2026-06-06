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

class ComputeContext;
class GraphicsContext;
class GpuBuffer;
class ProgramBinder;


struct SlangDescriptorHandle
{
    uint32_t X = 0;
    uint32_t Y = 0;
};

static_assert(sizeof(SlangDescriptorHandle) == sizeof(uint32_t) * 2);

class ProgramVar
{
public:
    ProgramVar() = default;

    ProgramVar operator[](const std::string& name) const;
    ProgramVar operator[](uint32_t index) const;

    bool IsValid() const;

    void Set(bool value) const;
    void Set(int32_t value) const;
    void Set(uint32_t value) const;
    void Set(float value) const;
    void Set(const Math::Scalar& value) const;
    void Set(const DirectX::XMINT2& value) const;
    void Set(const DirectX::XMINT3& value) const;
    void Set(const DirectX::XMINT4& value) const;
    void Set(const DirectX::XMUINT2& value) const;
    void Set(const DirectX::XMUINT3& value) const;
    void Set(const DirectX::XMUINT4& value) const;
    void Set(const DirectX::XMFLOAT2& value) const;
    void Set(const DirectX::XMFLOAT3& value) const;
    void Set(const DirectX::XMFLOAT4& value) const;
    void Set(const Math::Vector3& value) const;
    void Set(const Math::Vector4& value) const;
    void Set(const Math::Matrix3& value) const;
    void Set(const Math::Matrix4& value) const;
    void Set(const SlangDescriptorHandle& value) const;

    void SetSRV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetSampler(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetRootBufferSRV(const GpuBuffer& buffer, uint64_t offset = 0) const;
    void SetRootBufferUAV(const GpuBuffer& buffer, uint64_t offset = 0) const;
    void SetRootBufferSRV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const = delete;
    void SetRootBufferUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const = delete;

private:
    friend class ProgramBinder;

    ProgramVar(
        ProgramBinder* binder,
        uint32_t parameterIndex,
        const ProgramVarOffset& offset)
        : m_Binder(binder), m_ParameterIndex(parameterIndex), m_Offset(offset)
    {
    }

    ProgramBinder* m_Binder = nullptr;
    uint32_t m_ParameterIndex = UINT_MAX;
    ProgramVarOffset m_Offset;
};

class ProgramBinder
{
public:
    ProgramBinder(const Program& program, ComputeContext& context);
    ProgramBinder(const Program& program, GraphicsContext& context);

    ProgramVar GetRootVar();
    ProgramVar operator[](const std::string& name);

    void SetRootSignature() const;
    void Apply();

    void SetSRV(const std::string& name, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetUAV(const std::string& name, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetSampler(const std::string& name, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetRootBufferSRV(const std::string& name, const GpuBuffer& buffer, uint64_t offset = 0) const;
    void SetRootBufferUAV(const std::string& name, const GpuBuffer& buffer, uint64_t offset = 0) const;
    void SetRootBufferSRV(const std::string& name, D3D12_CPU_DESCRIPTOR_HANDLE handle) const = delete;
    void SetRootBufferUAV(const std::string& name, D3D12_CPU_DESCRIPTOR_HANDLE handle) const = delete;

private:
    friend class ProgramVar;

    struct UniformBindingData
    {
        std::vector<uint8_t> Data;
        bool Dirty = false;
    };

    void InitializeUniformData();
    ProgramVar FindChild(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        const std::string& name);
    ProgramVar FindElement(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        uint32_t index);
    void SetParameterData(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        const void* data,
        size_t sizeInBytes);
    void SetParameterValue(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        uint32_t scalarType,
        uint32_t rowCount,
        uint32_t columnCount,
        const void* data,
        size_t sizeInBytes);
    void SetParameterMatrix(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        uint32_t rowCount,
        uint32_t columnCount,
        const float* columnMajorValues);
    void SetParameterDescriptor(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        ProgramBindingKind expectedKind,
        D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void SetParameterRootBufferDescriptor(
        uint32_t parameterIndex,
        const ProgramVarOffset& offset,
        ProgramBindingKind expectedKind,
        const GpuBuffer& buffer,
        uint64_t bufferOffset);

    const ProgramBinding* RequireBinding(
        const std::string& name,
        ProgramBindingKind expectedKind) const;

    void SetDynamicDescriptor(
        const ProgramBinding& binding,
        uint32_t arrayOffset,
        D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    void SetRootBufferDescriptor(
        const ProgramBinding& binding,
        const GpuBuffer& buffer,
        uint64_t bufferOffset) const;

    void SetCBV(const std::string& name, size_t bufferSize, const void* bufferData) const;
    void SetRootConstants(const std::string& name, size_t sizeInBytes, const void* data) const;

    const Program& m_Program;
    ComputeContext* m_ComputeContext = nullptr;
    GraphicsContext* m_GraphicsContext = nullptr;
    std::vector<UniformBindingData> m_UniformData;
};

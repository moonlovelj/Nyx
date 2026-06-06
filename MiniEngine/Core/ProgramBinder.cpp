//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"
#include "ProgramBinder.h"

#include "CommandContext.h"
#include "slang.h"

#include <array>

namespace
{
    const char* BindingKindToString(ProgramBindingKind kind)
    {
        switch (kind)
        {
        case ProgramBindingKind::ConstantBuffer: return "ConstantBuffer";
        case ProgramBindingKind::RootConstants: return "RootConstants";
        case ProgramBindingKind::SRV: return "SRV";
        case ProgramBindingKind::UAV: return "UAV";
        case ProgramBindingKind::RootBufferSRV: return "RootBufferSRV";
        case ProgramBindingKind::RootBufferUAV: return "RootBufferUAV";
        case ProgramBindingKind::Sampler: return "Sampler";
        case ProgramBindingKind::StaticSampler: return "StaticSampler";
        default: return "Unknown";
        }
    }

    uint32_t GetScalarTypeValue(slang::TypeReflection::ScalarType type)
    {
        return static_cast<uint32_t>(type);
    }

    const char* ScalarTypeToString(uint32_t scalarType)
    {
        switch (static_cast<slang::TypeReflection::ScalarType>(scalarType))
        {
        case slang::TypeReflection::ScalarType::Bool: return "bool";
        case slang::TypeReflection::ScalarType::Int32: return "int32";
        case slang::TypeReflection::ScalarType::UInt32: return "uint32";
        case slang::TypeReflection::ScalarType::Float32: return "float32";
        default: return "unsupported";
        }
    }
}

ProgramVar ProgramVar::operator[](const std::string& name) const
{
    if (m_Binder == nullptr)
        return ProgramVar();
    return m_Binder->FindChild(m_ParameterIndex, m_Offset, name);
}

ProgramVar ProgramVar::operator[](uint32_t index) const
{
    if (m_Binder == nullptr)
        return ProgramVar();
    return m_Binder->FindElement(m_ParameterIndex, m_Offset, index);
}

bool ProgramVar::IsValid() const
{
    return m_Binder != nullptr && m_ParameterIndex != UINT_MAX;
}

void ProgramVar::Set(bool value) const
{
    if (m_Binder != nullptr)
    {
        const uint32_t packedValue = value ? 1u : 0u;
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Bool),
            1,
            1,
            &packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(int32_t value) const
{
    if (m_Binder != nullptr)
    {
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Int32),
            1,
            1,
            &value,
            sizeof(value));
    }
}

void ProgramVar::Set(uint32_t value) const
{
    if (m_Binder != nullptr)
    {
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::UInt32),
            1,
            1,
            &value,
            sizeof(value));
    }
}

void ProgramVar::Set(float value) const
{
    if (m_Binder != nullptr)
    {
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Float32),
            1,
            1,
            &value,
            sizeof(value));
    }
}

void ProgramVar::Set(const Math::Scalar& value) const
{
    Set(static_cast<float>(value));
}

void ProgramVar::Set(const DirectX::XMINT2& value) const
{
    if (m_Binder != nullptr)
    {
        const int32_t packedValue[2] = { value.x, value.y };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Int32),
            1,
            2,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMINT3& value) const
{
    if (m_Binder != nullptr)
    {
        const int32_t packedValue[3] = { value.x, value.y, value.z };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Int32),
            1,
            3,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMINT4& value) const
{
    if (m_Binder != nullptr)
    {
        const int32_t packedValue[4] = { value.x, value.y, value.z, value.w };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Int32),
            1,
            4,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMUINT2& value) const
{
    if (m_Binder != nullptr)
    {
        const uint32_t packedValue[2] = { value.x, value.y };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::UInt32),
            1,
            2,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMUINT3& value) const
{
    if (m_Binder != nullptr)
    {
        const uint32_t packedValue[3] = { value.x, value.y, value.z };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::UInt32),
            1,
            3,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMUINT4& value) const
{
    if (m_Binder != nullptr)
    {
        const uint32_t packedValue[4] = { value.x, value.y, value.z, value.w };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::UInt32),
            1,
            4,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMFLOAT2& value) const
{
    if (m_Binder != nullptr)
    {
        const float packedValue[2] = { value.x, value.y };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Float32),
            1,
            2,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMFLOAT3& value) const
{
    if (m_Binder != nullptr)
    {
        const float packedValue[3] = { value.x, value.y, value.z };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Float32),
            1,
            3,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const DirectX::XMFLOAT4& value) const
{
    if (m_Binder != nullptr)
    {
        const float packedValue[4] = { value.x, value.y, value.z, value.w };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::Float32),
            1,
            4,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::Set(const Math::Vector3& value) const
{
    const DirectX::XMFLOAT3 packedValue(
        static_cast<float>(value.GetX()),
        static_cast<float>(value.GetY()),
        static_cast<float>(value.GetZ()));
    Set(packedValue);
}

void ProgramVar::Set(const Math::Vector4& value) const
{
    const DirectX::XMFLOAT4 packedValue(
        static_cast<float>(value.GetX()),
        static_cast<float>(value.GetY()),
        static_cast<float>(value.GetZ()),
        static_cast<float>(value.GetW()));
    Set(packedValue);
}

void ProgramVar::Set(const Math::Matrix3& value) const
{
    if (m_Binder == nullptr)
        return;

    const Math::Vector3 x = value.GetX();
    const Math::Vector3 y = value.GetY();
    const Math::Vector3 z = value.GetZ();
    const float columnMajorValues[9] = {
        static_cast<float>(x.GetX()), static_cast<float>(x.GetY()), static_cast<float>(x.GetZ()),
        static_cast<float>(y.GetX()), static_cast<float>(y.GetY()), static_cast<float>(y.GetZ()),
        static_cast<float>(z.GetX()), static_cast<float>(z.GetY()), static_cast<float>(z.GetZ())
    };
    m_Binder->SetParameterMatrix(
        m_ParameterIndex,
        m_Offset,
        3,
        3,
        columnMajorValues);
}

void ProgramVar::Set(const Math::Matrix4& value) const
{
    if (m_Binder == nullptr)
        return;

    const Math::Vector4 x = value.GetX();
    const Math::Vector4 y = value.GetY();
    const Math::Vector4 z = value.GetZ();
    const Math::Vector4 w = value.GetW();
    const float columnMajorValues[16] = {
        static_cast<float>(x.GetX()), static_cast<float>(x.GetY()), static_cast<float>(x.GetZ()), static_cast<float>(x.GetW()),
        static_cast<float>(y.GetX()), static_cast<float>(y.GetY()), static_cast<float>(y.GetZ()), static_cast<float>(y.GetW()),
        static_cast<float>(z.GetX()), static_cast<float>(z.GetY()), static_cast<float>(z.GetZ()), static_cast<float>(z.GetW()),
        static_cast<float>(w.GetX()), static_cast<float>(w.GetY()), static_cast<float>(w.GetZ()), static_cast<float>(w.GetW())
    };
    m_Binder->SetParameterMatrix(
        m_ParameterIndex,
        m_Offset,
        4,
        4,
        columnMajorValues);
}

void ProgramVar::Set(const SlangDescriptorHandle& value) const
{
    if (m_Binder != nullptr)
    {
        const uint32_t packedValue[2] = { value.X, value.Y };
        m_Binder->SetParameterValue(
            m_ParameterIndex,
            m_Offset,
            GetScalarTypeValue(slang::TypeReflection::ScalarType::UInt32),
            1,
            2,
            packedValue,
            sizeof(packedValue));
    }
}

void ProgramVar::SetSRV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    if (m_Binder != nullptr)
        m_Binder->SetParameterDescriptor(
            m_ParameterIndex,
            m_Offset,
            ProgramBindingKind::SRV,
            handle);
}

void ProgramVar::SetUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    if (m_Binder != nullptr)
        m_Binder->SetParameterDescriptor(
            m_ParameterIndex,
            m_Offset,
            ProgramBindingKind::UAV,
            handle);
}

void ProgramVar::SetSampler(D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    if (m_Binder != nullptr)
        m_Binder->SetParameterDescriptor(
            m_ParameterIndex,
            m_Offset,
            ProgramBindingKind::Sampler,
            handle);
}

void ProgramVar::SetRootBufferSRV(const GpuBuffer& buffer, uint64_t offset) const
{
    if (m_Binder != nullptr)
        m_Binder->SetParameterRootBufferDescriptor(
            m_ParameterIndex,
            m_Offset,
            ProgramBindingKind::RootBufferSRV,
            buffer,
            offset);
}

void ProgramVar::SetRootBufferUAV(const GpuBuffer& buffer, uint64_t offset) const
{
    if (m_Binder != nullptr)
        m_Binder->SetParameterRootBufferDescriptor(
            m_ParameterIndex,
            m_Offset,
            ProgramBindingKind::RootBufferUAV,
            buffer,
            offset);
}

ProgramBinder::ProgramBinder(const Program& program, ComputeContext& context)
    : m_Program(program), m_ComputeContext(&context)
{
    InitializeUniformData();
}

ProgramBinder::ProgramBinder(const Program& program, GraphicsContext& context)
    : m_Program(program), m_GraphicsContext(&context)
{
    InitializeUniformData();
}

void ProgramBinder::InitializeUniformData()
{
    const std::vector<ProgramBinding>& bindings = m_Program.GetReflection().GetBindings();
    m_UniformData.resize(bindings.size());
    for (size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex)
    {
        const ProgramBinding& binding = bindings[bindingIndex];
        if (binding.Kind == ProgramBindingKind::ConstantBuffer ||
            binding.Kind == ProgramBindingKind::RootConstants)
        {
            m_UniformData[bindingIndex].Data.resize(binding.SizeInBytes, 0);
        }
    }
}

ProgramVar ProgramBinder::GetRootVar()
{
    return ProgramVar(this, ProgramReflection::RootParameterIndex, ProgramVarOffset());
}

ProgramVar ProgramBinder::operator[](const std::string& name)
{
    return GetRootVar()[name];
}

ProgramVar ProgramBinder::FindChild(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    const std::string& name)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr)
        return ProgramVar();

    auto childIter = parameter->ChildIndicesByName.find(name);
    if (childIter == parameter->ChildIndicesByName.end())
    {
        Utility::Printf(
            "Program parameter '%s' has no child named '%.*s'.\n",
            parameter->Path.c_str(),
            static_cast<int>(name.size()),
            name.c_str());
        ASSERT(false);
        return ProgramVar();
    }

    const ProgramParameter* child =
        m_Program.GetReflection().GetParameter(childIter->second);
    if (child == nullptr)
    {
        ASSERT(false);
        return ProgramVar();
    }

    ProgramVarOffset childOffset = offset;
    if (parameter->Kind == ProgramParameterKind::ConstantBuffer ||
        parameter->Kind == ProgramParameterKind::ParameterBlock)
    {
        childOffset = ProgramVarOffset();
    }

    const uint64_t uniformOffset =
        static_cast<uint64_t>(childOffset.Uniform) + child->RelativeUniformOffset;
    if (uniformOffset > UINT_MAX)
    {
        Utility::Printf("Program parameter uniform offset overflow at '%s'.\n", child->Path.c_str());
        ASSERT(false);
        return ProgramVar();
    }
    childOffset.Uniform = static_cast<uint32_t>(uniformOffset);
    return ProgramVar(this, childIter->second, childOffset);
}

ProgramVar ProgramBinder::FindElement(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    uint32_t index)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr || parameter->Kind != ProgramParameterKind::Array ||
        parameter->ElementParameterIndex == UINT_MAX)
    {
        Utility::Printf("Program parameter is not an array.\n");
        ASSERT(false);
        return ProgramVar();
    }

    if (index >= parameter->ArrayCount)
    {
        Utility::Printf(
            "Program parameter array '%s' has %u elements; index %u is out of range.\n",
            parameter->Path.c_str(),
            parameter->ArrayCount,
            index);
        ASSERT(false);
        return ProgramVar();
    }

    ProgramVarOffset elementOffset = offset;
    const uint64_t uniformOffset = static_cast<uint64_t>(elementOffset.Uniform) +
        static_cast<uint64_t>(index) * parameter->UniformStride;
    const uint64_t resourceArrayIndex =
        static_cast<uint64_t>(elementOffset.ResourceArrayIndex) * parameter->ArrayCount + index;
    if (uniformOffset > UINT_MAX || resourceArrayIndex > UINT_MAX)
    {
        Utility::Printf("Program parameter array offset overflow at '%s'.\n", parameter->Path.c_str());
        ASSERT(false);
        return ProgramVar();
    }
    elementOffset.Uniform = static_cast<uint32_t>(uniformOffset);
    elementOffset.ResourceArrayIndex = static_cast<uint32_t>(resourceArrayIndex);
    return ProgramVar(this, parameter->ElementParameterIndex, elementOffset);
}

void ProgramBinder::SetParameterValue(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    uint32_t scalarType,
    uint32_t rowCount,
    uint32_t columnCount,
    const void* data,
    size_t sizeInBytes)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr || parameter->Kind != ProgramParameterKind::Uniform)
    {
        Utility::Printf("Program parameter is not ordinary uniform data.\n");
        ASSERT(false);
        return;
    }

    const uint32_t reflectedRowCount = parameter->RowCount == 0 ? 1 : parameter->RowCount;
    const uint32_t reflectedColumnCount = parameter->ColumnCount == 0 ? 1 : parameter->ColumnCount;
    if (parameter->ScalarType != scalarType ||
        reflectedRowCount != rowCount ||
        reflectedColumnCount != columnCount ||
        parameter->MatrixLayout != ProgramMatrixLayout::None)
    {
        Utility::Printf(
            "Program parameter '%s' is %s %ux%u, but the supplied value is %s %ux%u.\n",
            parameter->Path.c_str(),
            ScalarTypeToString(parameter->ScalarType),
            reflectedRowCount,
            reflectedColumnCount,
            ScalarTypeToString(scalarType),
            rowCount,
            columnCount);
        ASSERT(false);
        return;
    }

    const uint64_t expectedSize =
        static_cast<uint64_t>(rowCount) * columnCount * sizeof(uint32_t);
    if (data == nullptr || expectedSize != sizeInBytes || parameter->UniformSize != sizeInBytes)
    {
        Utility::Printf(
            "Program parameter '%s' has an incompatible scalar/vector layout.\n",
            parameter->Path.c_str());
        ASSERT(false);
        return;
    }

    SetParameterData(parameterIndex, offset, data, sizeInBytes);
}

void ProgramBinder::SetParameterMatrix(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    uint32_t rowCount,
    uint32_t columnCount,
    const float* columnMajorValues)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    const uint32_t floatScalarType =
        GetScalarTypeValue(slang::TypeReflection::ScalarType::Float32);
    if (parameter == nullptr || parameter->Kind != ProgramParameterKind::Uniform ||
        parameter->ScalarType != floatScalarType ||
        parameter->RowCount != rowCount ||
        parameter->ColumnCount != columnCount ||
        parameter->MatrixLayout == ProgramMatrixLayout::None)
    {
        Utility::Printf("Program parameter is not a matching float%ux%u matrix.\n", rowCount, columnCount);
        ASSERT(false);
        return;
    }

    const bool isColumnMajor = parameter->MatrixLayout == ProgramMatrixLayout::ColumnMajor;
    const uint32_t majorCount = isColumnMajor ? columnCount : rowCount;
    const uint32_t minorCount = isColumnMajor ? rowCount : columnCount;
    const uint32_t vectorDataSize = minorCount * sizeof(float);
    if (columnMajorValues == nullptr || parameter->MatrixVectorStride < vectorDataSize ||
        parameter->UniformTypeStride > sizeof(float) * 16 ||
        static_cast<uint64_t>(parameter->MatrixVectorStride) * majorCount !=
            parameter->UniformTypeStride)
    {
        Utility::Printf(
            "Program parameter '%s' has an unsupported matrix storage layout.\n",
            parameter->Path.c_str());
        ASSERT(false);
        return;
    }

    std::array<uint8_t, sizeof(float) * 16> packedData = {};
    for (uint32_t row = 0; row < rowCount; ++row)
    {
        for (uint32_t column = 0; column < columnCount; ++column)
        {
            const uint32_t majorIndex = isColumnMajor ? column : row;
            const uint32_t minorIndex = isColumnMajor ? row : column;
            const uint32_t destinationOffset =
                majorIndex * parameter->MatrixVectorStride + minorIndex * sizeof(float);
            if (destinationOffset + sizeof(float) > parameter->UniformSize)
            {
                ASSERT(false);
                return;
            }

            const float value = columnMajorValues[column * rowCount + row];
            memcpy(packedData.data() + destinationOffset, &value, sizeof(value));
        }
    }

    SetParameterData(
        parameterIndex,
        offset,
        packedData.data(),
        parameter->UniformSize);
}

void ProgramBinder::SetParameterData(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    const void* data,
    size_t sizeInBytes)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr || parameter->Kind != ProgramParameterKind::Uniform ||
        parameter->UniformBindingIndex == UINT_MAX)
    {
        Utility::Printf("Program parameter is not ordinary uniform data.\n");
        ASSERT(false);
        return;
    }

    if (data == nullptr || sizeInBytes != parameter->UniformSize)
    {
        Utility::Printf(
            "Program parameter '%s' requires %u bytes, received %llu bytes.\n",
            parameter->Path.c_str(),
            parameter->UniformSize,
            static_cast<unsigned long long>(sizeInBytes));
        ASSERT(false);
        return;
    }

    if (parameter->UniformBindingIndex >= m_UniformData.size())
    {
        ASSERT(false);
        return;
    }

    UniformBindingData& bindingData = m_UniformData[parameter->UniformBindingIndex];
    const uint64_t destinationOffset = offset.Uniform;
    if (destinationOffset + sizeInBytes > bindingData.Data.size())
    {
        Utility::Printf(
            "Program parameter '%s' writes outside its constant buffer.\n",
            parameter->Path.c_str());
        ASSERT(false);
        return;
    }

    memcpy(bindingData.Data.data() + destinationOffset, data, sizeInBytes);
    bindingData.Dirty = true;
}

void ProgramBinder::SetParameterDescriptor(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    ProgramBindingKind expectedKind,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr || parameter->BindingIndex == UINT_MAX)
    {
        Utility::Printf("Program parameter has no descriptor binding.\n");
        ASSERT(false);
        return;
    }

    const std::vector<ProgramBinding>& bindings = m_Program.GetReflection().GetBindings();
    if (parameter->BindingIndex >= bindings.size())
    {
        ASSERT(false);
        return;
    }

    const ProgramBinding& binding = bindings[parameter->BindingIndex];
    if (binding.Kind != expectedKind)
    {
        Utility::Printf(
            "Program parameter '%s' is %s, expected %s.\n",
            parameter->Path.c_str(),
            BindingKindToString(binding.Kind),
            BindingKindToString(expectedKind));
        ASSERT(false);
        return;
    }

    SetDynamicDescriptor(binding, offset.ResourceArrayIndex, handle);
}

void ProgramBinder::SetParameterRootBufferDescriptor(
    uint32_t parameterIndex,
    const ProgramVarOffset& offset,
    ProgramBindingKind expectedKind,
    const GpuBuffer& buffer,
    uint64_t bufferOffset)
{
    const ProgramParameter* parameter = m_Program.GetReflection().GetParameter(parameterIndex);
    if (parameter == nullptr || parameter->BindingIndex == UINT_MAX)
    {
        Utility::Printf("Program parameter has no root buffer binding.\n");
        ASSERT(false);
        return;
    }

    if (offset.ResourceArrayIndex != 0)
    {
        Utility::Printf(
            "Root buffer parameter '%s' does not support array offsets.\n",
            parameter->Path.c_str());
        ASSERT(false);
        return;
    }

    const std::vector<ProgramBinding>& bindings = m_Program.GetReflection().GetBindings();
    if (parameter->BindingIndex >= bindings.size())
    {
        ASSERT(false);
        return;
    }

    const ProgramBinding& binding = bindings[parameter->BindingIndex];
    if (binding.Kind != expectedKind)
    {
        Utility::Printf(
            "Program parameter '%s' is %s, expected %s.\n",
            parameter->Path.c_str(),
            BindingKindToString(binding.Kind),
            BindingKindToString(expectedKind));
        ASSERT(false);
        return;
    }

    SetRootBufferDescriptor(binding, buffer, bufferOffset);
}

void ProgramBinder::SetRootSignature() const
{
    if (m_ComputeContext != nullptr)
    {
        m_ComputeContext->SetRootSignature(m_Program.GetRootSignature());
        return;
    }

    ASSERT(m_GraphicsContext != nullptr);
    m_GraphicsContext->SetRootSignature(m_Program.GetRootSignature());
}

void ProgramBinder::Apply()
{
    const std::vector<ProgramBinding>& bindings = m_Program.GetReflection().GetBindings();
    for (size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex)
    {
        UniformBindingData& bindingData = m_UniformData[bindingIndex];
        if (!bindingData.Dirty)
            continue;

        const ProgramBinding& binding = bindings[bindingIndex];
        if (binding.Kind == ProgramBindingKind::ConstantBuffer)
        {
            SetCBV(binding.Name, bindingData.Data.size(), bindingData.Data.data());
        }
        else if (binding.Kind == ProgramBindingKind::RootConstants)
        {
            SetRootConstants(binding.Name, bindingData.Data.size(), bindingData.Data.data());
        }
        bindingData.Dirty = false;
    }
}

void ProgramBinder::SetCBV(
    const std::string& name,
    size_t bufferSize,
    const void* bufferData) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::ConstantBuffer);
    if (binding == nullptr)
        return;

    if (bufferData == nullptr || bufferSize != binding->SizeInBytes)
    {
        Utility::Printf(
            "Constant buffer '%s' requires %u bytes, received %llu bytes.\n",
            name.c_str(),
            binding->SizeInBytes,
            static_cast<unsigned long long>(bufferSize));
        ASSERT(false);
        return;
    }

    if (m_ComputeContext != nullptr)
    {
        m_ComputeContext->SetDynamicConstantBufferView(binding->RootIndex, bufferSize, bufferData);
        return;
    }

    ASSERT(m_GraphicsContext != nullptr);
    m_GraphicsContext->SetDynamicConstantBufferView(binding->RootIndex, bufferSize, bufferData);
}

void ProgramBinder::SetRootConstants(
    const std::string& name,
    size_t sizeInBytes,
    const void* data) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::RootConstants);
    if (binding == nullptr)
        return;

    if (data == nullptr || sizeInBytes != binding->SizeInBytes || sizeInBytes % sizeof(uint32_t) != 0)
    {
        Utility::Printf(
            "Root constants '%s' require %u bytes, received %llu bytes.\n",
            name.c_str(),
            binding->SizeInBytes,
            static_cast<unsigned long long>(sizeInBytes));
        ASSERT(false);
        return;
    }

    if (m_ComputeContext != nullptr)
    {
        m_ComputeContext->SetConstantArray(binding->RootIndex, binding->Num32BitValues, data);
        return;
    }

    ASSERT(m_GraphicsContext != nullptr);
    m_GraphicsContext->SetConstantArray(binding->RootIndex, binding->Num32BitValues, data);
}

void ProgramBinder::SetSRV(
    const std::string& name,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::SRV);
    if (binding != nullptr)
        SetDynamicDescriptor(*binding, 0, handle);
}

void ProgramBinder::SetUAV(
    const std::string& name,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::UAV);
    if (binding != nullptr)
        SetDynamicDescriptor(*binding, 0, handle);
}

void ProgramBinder::SetSampler(
    const std::string& name,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::Sampler);
    if (binding != nullptr)
        SetDynamicDescriptor(*binding, 0, handle);
}

void ProgramBinder::SetRootBufferSRV(
    const std::string& name,
    const GpuBuffer& buffer,
    uint64_t offset) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::RootBufferSRV);
    if (binding != nullptr)
        SetRootBufferDescriptor(*binding, buffer, offset);
}

void ProgramBinder::SetRootBufferUAV(
    const std::string& name,
    const GpuBuffer& buffer,
    uint64_t offset) const
{
    const ProgramBinding* binding = RequireBinding(name, ProgramBindingKind::RootBufferUAV);
    if (binding != nullptr)
        SetRootBufferDescriptor(*binding, buffer, offset);
}

const ProgramBinding* ProgramBinder::RequireBinding(
    const std::string& name,
    ProgramBindingKind expectedKind) const
{
    const ProgramBinding* binding = m_Program.FindBinding(name);
    if (binding == nullptr)
    {
        Utility::Printf("Program binding '%s' was not found.\n", name.c_str());
        ASSERT(false);
        return nullptr;
    }

    if (binding->Kind != expectedKind)
    {
        Utility::Printf(
            "Program binding '%s' is %s, expected %s.\n",
            name.c_str(),
            BindingKindToString(binding->Kind),
            BindingKindToString(expectedKind));
        ASSERT(false);
        return nullptr;
    }

    return binding;
}

void ProgramBinder::SetDynamicDescriptor(
    const ProgramBinding& binding,
    uint32_t arrayOffset,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    ASSERT(binding.IsDescriptorTableBinding());
    if (arrayOffset >= binding.Count)
    {
        Utility::Printf(
            "Descriptor array offset %u is outside binding '%s' with count %u.\n",
            arrayOffset,
            binding.Name.c_str(),
            binding.Count);
        ASSERT(false);
        return;
    }

    const uint32_t tableOffset = binding.TableOffset + arrayOffset;
    if (binding.Kind == ProgramBindingKind::Sampler)
    {
        if (m_ComputeContext != nullptr)
        {
            m_ComputeContext->SetDynamicSampler(binding.RootIndex, tableOffset, handle);
            return;
        }

        ASSERT(m_GraphicsContext != nullptr);
        m_GraphicsContext->SetDynamicSampler(binding.RootIndex, tableOffset, handle);
        return;
    }

    if (m_ComputeContext != nullptr)
    {
        m_ComputeContext->SetDynamicDescriptor(binding.RootIndex, tableOffset, handle);
        return;
    }

    ASSERT(m_GraphicsContext != nullptr);
    m_GraphicsContext->SetDynamicDescriptor(binding.RootIndex, tableOffset, handle);
}

void ProgramBinder::SetRootBufferDescriptor(
    const ProgramBinding& binding,
    const GpuBuffer& buffer,
    uint64_t bufferOffset) const
{
    if (binding.Count != 1 ||
        (binding.Kind != ProgramBindingKind::RootBufferSRV &&
            binding.Kind != ProgramBindingKind::RootBufferUAV))
    {
        Utility::Printf(
            "Program binding '%s' is not a single root buffer descriptor.\n",
            binding.Name.c_str());
        ASSERT(false);
        return;
    }

    if (binding.Kind == ProgramBindingKind::RootBufferSRV)
    {
        if (m_ComputeContext != nullptr)
        {
            m_ComputeContext->SetBufferSRV(binding.RootIndex, buffer, bufferOffset);
            return;
        }

        ASSERT(m_GraphicsContext != nullptr);
        m_GraphicsContext->SetBufferSRV(binding.RootIndex, buffer, bufferOffset);
        return;
    }

    if (m_ComputeContext != nullptr)
    {
        m_ComputeContext->SetBufferUAV(binding.RootIndex, buffer, bufferOffset);
        return;
    }

    ASSERT(m_GraphicsContext != nullptr);
    m_GraphicsContext->SetBufferUAV(binding.RootIndex, buffer, bufferOffset);
}

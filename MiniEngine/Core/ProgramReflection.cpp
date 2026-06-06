//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"
#include "ProgramReflection.h"

#include "ProgramDesc.h"
#include "slang.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace
{
    using ReflectionPath = std::vector<slang::VariableLayoutReflection*>;

    void AppendLine(std::string& buildLog, const std::string& line)
    {
        buildLog += line;
        buildLog += "\n";
    }

    bool IsKnownSlangValue(size_t value)
    {
        return value != SLANG_UNKNOWN_SIZE && value != SLANG_UNBOUNDED_SIZE;
    }

    bool IsKnownSlangValue(SlangInt value)
    {
        return value >= 0 &&
            value != static_cast<SlangInt>(SLANG_UNKNOWN_SIZE) &&
            value != static_cast<SlangInt>(SLANG_UNBOUNDED_SIZE);
    }

    template <typename T>
    bool TryConvertToUint32(T value, uint32_t& result)
    {
        if (!IsKnownSlangValue(value) ||
            static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        result = static_cast<uint32_t>(value);
        return true;
    }

    bool TryAdd(uint32_t value, uint64_t& total)
    {
        total += value;
        return total <= std::numeric_limits<uint32_t>::max();
    }

    bool HasCategory(
        slang::VariableLayoutReflection* variable,
        slang::ParameterCategory category)
    {
        if (variable == nullptr)
            return false;

        const unsigned int categoryCount = variable->getCategoryCount();
        for (unsigned int categoryIndex = 0; categoryIndex < categoryCount; ++categoryIndex)
        {
            if (variable->getCategoryByIndex(categoryIndex) == category)
                return true;
        }
        return variable->getCategory() == category;
    }

    bool HasCategory(
        slang::TypeLayoutReflection* typeLayout,
        slang::ParameterCategory category)
    {
        if (typeLayout == nullptr)
            return false;

        const unsigned int categoryCount = typeLayout->getCategoryCount();
        for (unsigned int categoryIndex = 0; categoryIndex < categoryCount; ++categoryIndex)
        {
            if (typeLayout->getCategoryByIndex(categoryIndex) == category)
                return true;
        }
        return false;
    }

    bool TryGetOffset(
        slang::VariableLayoutReflection* variable,
        slang::ParameterCategory category,
        uint32_t& result,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        result = 0;
        if (!HasCategory(variable, category))
            return true;

        if (!TryConvertToUint32(variable->getOffset(category), result))
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Unknown or overflowing layout offset at '" +
                parameterPath + "'.");
            return false;
        }
        return true;
    }

    bool TryGetBindingSpaceOffset(
        slang::VariableLayoutReflection* variable,
        slang::ParameterCategory category,
        uint32_t& result,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        result = 0;
        if (!HasCategory(variable, category))
            return true;

        if (!TryConvertToUint32(variable->getBindingSpace(category), result))
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Unknown or overflowing register-space offset at '" +
                parameterPath + "'.");
            return false;
        }
        return true;
    }

    bool TryGetTypeSize(
        slang::TypeLayoutReflection* typeLayout,
        slang::ParameterCategory category,
        uint32_t& result,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        result = 0;
        if (typeLayout == nullptr || !HasCategory(typeLayout, category))
            return true;

        if (!TryConvertToUint32(typeLayout->getSize(category), result))
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Unknown or overflowing layout size at '" +
                parameterPath + "'.");
            return false;
        }
        return true;
    }

    bool TryGetTypeStride(
        slang::TypeLayoutReflection* typeLayout,
        slang::ParameterCategory category,
        uint32_t& result,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        result = 0;
        if (typeLayout == nullptr || !HasCategory(typeLayout, category))
            return true;

        if (!TryConvertToUint32(typeLayout->getStride(category), result))
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Unknown or overflowing layout stride at '" +
                parameterPath + "'.");
            return false;
        }
        return true;
    }

    bool TryGetElementStride(
        slang::TypeLayoutReflection* typeLayout,
        slang::ParameterCategory category,
        uint32_t& result,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        result = 0;
        if (typeLayout == nullptr || !HasCategory(typeLayout, category))
            return true;

        if (!TryConvertToUint32(
            typeLayout->getElementStride(static_cast<SlangParameterCategory>(category)),
            result))
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Unknown or overflowing array stride at '" +
                parameterPath + "'.");
            return false;
        }
        return true;
    }

    bool IsParameterBlockWithSpace(slang::VariableLayoutReflection* variable)
    {
        if (variable == nullptr || variable->getTypeLayout() == nullptr ||
            variable->getTypeLayout()->getKind() != slang::TypeReflection::Kind::ParameterBlock)
        {
            return false;
        }

        slang::TypeLayoutReflection* typeLayout = variable->getTypeLayout();
        return HasCategory(typeLayout, slang::ParameterCategory::SubElementRegisterSpace) ||
            HasCategory(typeLayout, slang::ParameterCategory::RegisterSpace) ||
            HasCategory(variable, slang::ParameterCategory::SubElementRegisterSpace) ||
            HasCategory(variable, slang::ParameterCategory::RegisterSpace);
    }

    bool TryGetBindingLocation(
        const ReflectionPath& path,
        slang::ParameterCategory category,
        const std::string& parameterPath,
        uint32_t& registerIndex,
        uint32_t& registerSpace,
        std::string& buildLog)
    {
        registerIndex = 0;
        registerSpace = 0;
        if (path.empty())
        {
            AppendLine(buildLog, "[ProgramReflection] Empty reflection path at '" + parameterPath + "'.");
            return false;
        }

        size_t deepestParameterBlock = SIZE_MAX;
        for (size_t pathIndex = path.size(); pathIndex > 0; --pathIndex)
        {
            if (IsParameterBlockWithSpace(path[pathIndex - 1]))
            {
                deepestParameterBlock = pathIndex - 1;
                break;
            }
        }

        uint64_t accumulatedRegister = 0;
        bool foundRegisterCategory = false;
        const size_t registerStop = deepestParameterBlock == SIZE_MAX
            ? 0
            : deepestParameterBlock + 1;
        for (size_t pathIndex = path.size(); pathIndex > registerStop; --pathIndex)
        {
            foundRegisterCategory = foundRegisterCategory ||
                HasCategory(path[pathIndex - 1], category);
            uint32_t relativeOffset = 0;
            if (!TryGetOffset(path[pathIndex - 1], category, relativeOffset, parameterPath, buildLog) ||
                !TryAdd(relativeOffset, accumulatedRegister))
            {
                AppendLine(buildLog, "[ProgramReflection] Register index overflow at '" + parameterPath + "'.");
                return false;
            }
        }
        if (!foundRegisterCategory)
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] No physical binding category was found at '" +
                parameterPath + "'.");
            return false;
        }

        uint64_t accumulatedSpace = 0;
        if (deepestParameterBlock == SIZE_MAX)
        {
            for (size_t pathIndex = path.size(); pathIndex > 0; --pathIndex)
            {
                uint32_t relativeSpace = 0;
                if (!TryGetBindingSpaceOffset(
                    path[pathIndex - 1], category, relativeSpace, parameterPath, buildLog) ||
                    !TryAdd(relativeSpace, accumulatedSpace))
                {
                    AppendLine(buildLog, "[ProgramReflection] Register space overflow at '" + parameterPath + "'.");
                    return false;
                }
            }
        }
        else
        {
            // Binding-space contributions below the deepest parameter block are
            // local to that block.
            for (size_t pathIndex = path.size(); pathIndex > deepestParameterBlock + 1; --pathIndex)
            {
                uint32_t relativeSpace = 0;
                if (!TryGetBindingSpaceOffset(
                    path[pathIndex - 1], category, relativeSpace, parameterPath, buildLog) ||
                    !TryAdd(relativeSpace, accumulatedSpace))
                {
                    AppendLine(buildLog, "[ProgramReflection] Register space overflow at '" + parameterPath + "'.");
                    return false;
                }
            }

            // Slang 2026 uses SubElementRegisterSpace for ParameterBlock. Keep a
            // category-based legacy fallback without treating a valid space0 as missing.
            for (size_t pathIndex = deepestParameterBlock + 1; pathIndex > 0; --pathIndex)
            {
                slang::VariableLayoutReflection* variable = path[pathIndex - 1];
                slang::ParameterCategory spaceCategory = slang::ParameterCategory::None;
                if (HasCategory(variable, slang::ParameterCategory::SubElementRegisterSpace))
                    spaceCategory = slang::ParameterCategory::SubElementRegisterSpace;
                else if (HasCategory(variable, slang::ParameterCategory::RegisterSpace))
                    spaceCategory = slang::ParameterCategory::RegisterSpace;

                if (spaceCategory == slang::ParameterCategory::None)
                    continue;

                uint32_t relativeSpace = 0;
                if (!TryGetOffset(variable, spaceCategory, relativeSpace, parameterPath, buildLog) ||
                    !TryAdd(relativeSpace, accumulatedSpace))
                {
                    AppendLine(buildLog, "[ProgramReflection] Parameter-block space overflow at '" + parameterPath + "'.");
                    return false;
                }
            }
        }

        registerIndex = static_cast<uint32_t>(accumulatedRegister);
        registerSpace = static_cast<uint32_t>(accumulatedSpace);
        return true;
    }

    std::string JoinParameterPath(const std::string& parentPath, const std::string& name)
    {
        if (parentPath.empty())
            return name;
        if (name.empty())
            return parentPath;
        return parentPath + "." + name;
    }

    ProgramParameterKind GetParameterKind(slang::TypeReflection::Kind kind)
    {
        switch (kind)
        {
        case slang::TypeReflection::Kind::Struct: return ProgramParameterKind::Struct;
        case slang::TypeReflection::Kind::Array: return ProgramParameterKind::Array;
        case slang::TypeReflection::Kind::ConstantBuffer: return ProgramParameterKind::ConstantBuffer;
        case slang::TypeReflection::Kind::ParameterBlock: return ProgramParameterKind::ParameterBlock;
        case slang::TypeReflection::Kind::SamplerState: return ProgramParameterKind::Sampler;
        case slang::TypeReflection::Kind::Resource:
        case slang::TypeReflection::Kind::TextureBuffer:
        case slang::TypeReflection::Kind::ShaderStorageBuffer:
        case slang::TypeReflection::Kind::DynamicResource:
            return ProgramParameterKind::SRV;
        default:
            return ProgramParameterKind::Uniform;
        }
    }

    bool TryMapResourceKind(
        slang::TypeLayoutReflection* typeLayout,
        const ReflectionPath& path,
        slang::ParameterCategory& category,
        ProgramBindingKind& kind,
        const std::string& parameterPath,
        std::string& buildLog)
    {
        if (typeLayout->getKind() == slang::TypeReflection::Kind::SamplerState)
        {
            category = slang::ParameterCategory::SamplerState;
            kind = ProgramBindingKind::Sampler;
            return true;
        }

        switch (typeLayout->getResourceAccess())
        {
        case SLANG_RESOURCE_ACCESS_READ:
            category = slang::ParameterCategory::ShaderResource;
            kind = ProgramBindingKind::SRV;
            return true;
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
        case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
        case SLANG_RESOURCE_ACCESS_APPEND:
        case SLANG_RESOURCE_ACCESS_CONSUME:
        case SLANG_RESOURCE_ACCESS_WRITE:
        case SLANG_RESOURCE_ACCESS_FEEDBACK:
            category = slang::ParameterCategory::UnorderedAccess;
            kind = ProgramBindingKind::UAV;
            return true;
        default:
            break;
        }

        // Some wrapper/dynamic resource layouts do not expose a useful access
        // mode. Prefer the nearest unambiguous variable layout category.
        for (size_t pathIndex = path.size(); pathIndex > 0; --pathIndex)
        {
            slang::VariableLayoutReflection* variable = path[pathIndex - 1];
            const bool isSrv = HasCategory(variable, slang::ParameterCategory::ShaderResource);
            const bool isUav = HasCategory(variable, slang::ParameterCategory::UnorderedAccess);
            if (isSrv == isUav)
                continue;

            category = isSrv
                ? slang::ParameterCategory::ShaderResource
                : slang::ParameterCategory::UnorderedAccess;
            kind = isSrv ? ProgramBindingKind::SRV : ProgramBindingKind::UAV;
            return true;
        }

        AppendLine(
            buildLog,
            "[ProgramReflection] Unable to classify resource parameter '" +
            parameterPath + "'.");
        return false;
    }

    SlangResourceShape GetBaseResourceShape(SlangResourceShape shape)
    {
        return static_cast<SlangResourceShape>(shape & SLANG_RESOURCE_BASE_SHAPE_MASK);
    }

    const char* ResourceShapeToString(SlangResourceShape shape)
    {
        switch (GetBaseResourceShape(shape))
        {
        case SLANG_TEXTURE_1D: return "Texture1D";
        case SLANG_TEXTURE_2D: return "Texture2D";
        case SLANG_TEXTURE_3D: return "Texture3D";
        case SLANG_TEXTURE_CUBE: return "TextureCube";
        case SLANG_TEXTURE_BUFFER: return "TextureBuffer";
        case SLANG_STRUCTURED_BUFFER: return "StructuredBuffer";
        case SLANG_BYTE_ADDRESS_BUFFER: return "ByteAddressBuffer";
        case SLANG_RESOURCE_UNKNOWN: return "Unknown";
        default: return "None";
        }
    }

    bool IsRootBufferResourceShape(slang::TypeLayoutReflection* typeLayout)
    {
        if (typeLayout == nullptr)
            return false;

        const SlangResourceShape baseShape = GetBaseResourceShape(typeLayout->getResourceShape());
        return baseShape == SLANG_STRUCTURED_BUFFER ||
            baseShape == SLANG_BYTE_ADDRESS_BUFFER;
    }

    bool UsesExplicitDescriptorHeapIndexing(const ProgramDesc& desc)
    {
        const D3D12_ROOT_SIGNATURE_FLAGS flags = desc.GetRootSignatureFlags();
        return (flags & (
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)) != 0;
    }

    class ProgramReflectionBuilder
    {
    public:
        ProgramReflectionBuilder(
            const ProgramDesc& desc,
            ProgramReflection& reflection,
            std::string& buildLog)
            : m_Desc(desc), m_Reflection(reflection), m_BuildLog(buildLog)
        {
        }

        bool Build(slang::ShaderReflection* layout)
        {
            m_Reflection.Clear();
            if (layout == nullptr)
            {
                AppendLine(m_BuildLog, "[ProgramReflection] Missing program layout.");
                return false;
            }
            m_Reflection.SetUsesDescriptorHeapIndexing(UsesExplicitDescriptorHeapIndexing(m_Desc));

            slang::VariableLayoutReflection* globalVariable = layout->getGlobalParamsVarLayout();
            if (globalVariable == nullptr || globalVariable->getTypeLayout() == nullptr)
            {
                AppendLine(m_BuildLog, "[ProgramReflection] Missing global parameter layout.");
                return false;
            }

            ReflectionPath globalPath(1, globalVariable);
            slang::TypeLayoutReflection* globalType = globalVariable->getTypeLayout();
            if (globalType->getKind() == slang::TypeReflection::Kind::ConstantBuffer ||
                globalType->getKind() == slang::TypeReflection::Kind::ParameterBlock)
            {
                if (!ReflectContainerContents(
                    globalType,
                    ProgramReflection::RootParameterIndex,
                    globalPath,
                    "$Globals",
                    1))
                {
                    return false;
                }
            }
            else if (!ReflectTypeContents(
                globalType,
                ProgramReflection::RootParameterIndex,
                globalPath,
                UINT_MAX,
                1,
                ""))
            {
                return false;
            }

            for (const std::string& parameterName : m_Desc.GetRootConstantParameters())
            {
                if (m_ReflectedRootConstants.find(parameterName) == m_ReflectedRootConstants.end())
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Root constants parameter was not found or has no ordinary data: '" +
                        parameterName + "'.");
                    return false;
                }
            }

            for (const ProgramDesc::StaticSampler& sampler : m_Desc.GetStaticSamplers())
            {
                if (m_ReflectedStaticSamplers.find(sampler.ParameterName) == m_ReflectedStaticSamplers.end())
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Static sampler parameter was not found: '" +
                        sampler.ParameterName + "'.");
                    return false;
                }
            }

            for (const std::string& parameterName : m_Desc.GetRootBufferSRVParameters())
            {
                if (m_ReflectedRootBufferSRVs.find(parameterName) == m_ReflectedRootBufferSRVs.end())
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] RootBufferSRV parameter was not found or is not a valid buffer SRV: '" +
                        parameterName + "'.");
                    return false;
                }
            }

            for (const std::string& parameterName : m_Desc.GetRootBufferUAVParameters())
            {
                if (m_ReflectedRootBufferUAVs.find(parameterName) == m_ReflectedRootBufferUAVs.end())
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] RootBufferUAV parameter was not found or is not a valid buffer UAV: '" +
                        parameterName + "'.");
                    return false;
                }
            }
            return true;
        }

    private:
        bool ReflectTypeContents(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parentIndex,
            const ReflectionPath& path,
            uint32_t uniformBindingIndex,
            uint32_t resourceArrayCount,
            const std::string& parentPath)
        {
            if (typeLayout == nullptr || typeLayout->getKind() != slang::TypeReflection::Kind::Struct)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Expected a struct layout at '" + parentPath + "'.");
                return false;
            }

            for (unsigned int fieldIndex = 0; fieldIndex < typeLayout->getFieldCount(); ++fieldIndex)
            {
                slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(fieldIndex);
                if (!ReflectVariable(
                    field,
                    parentIndex,
                    path,
                    uniformBindingIndex,
                    resourceArrayCount,
                    parentPath))
                {
                    return false;
                }
            }
            return true;
        }

        bool ReflectVariable(
            slang::VariableLayoutReflection* variable,
            uint32_t parentIndex,
            const ReflectionPath& parentPath,
            uint32_t uniformBindingIndex,
            uint32_t resourceArrayCount,
            const std::string& parentName)
        {
            if (variable == nullptr || variable->getTypeLayout() == nullptr)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Encountered a variable without a type layout under '" +
                    parentName + "'.");
                return false;
            }

            const char* reflectedName = variable->getName();
            const std::string name = reflectedName != nullptr ? reflectedName : "";
            if (name.empty())
            {
                AppendLine(m_BuildLog, "[ProgramReflection] Encountered an unnamed shader parameter.");
                return false;
            }

            const std::string parameterPath = JoinParameterPath(parentName, name);
            uint32_t relativeUniformOffset = 0;
            uint32_t uniformSize = 0;
            if (!TryGetTypeSize(
                variable->getTypeLayout(),
                slang::ParameterCategory::Uniform,
                uniformSize,
                parameterPath,
                m_BuildLog))
            {
                return false;
            }
            if (uniformSize > 0 && !TryGetOffset(
                variable,
                slang::ParameterCategory::Uniform,
                relativeUniformOffset,
                parameterPath,
                m_BuildLog))
            {
                return false;
            }

            ReflectionPath path = parentPath;
            path.push_back(variable);
            return ReflectTypeNode(
                variable->getTypeLayout(),
                parentIndex,
                path,
                name,
                parameterPath,
                relativeUniformOffset,
                uniformBindingIndex,
                resourceArrayCount);
        }

        bool ReflectTypeNode(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parentIndex,
            const ReflectionPath& path,
            const std::string& name,
            const std::string& parameterPath,
            uint32_t relativeUniformOffset,
            uint32_t uniformBindingIndex,
            uint32_t resourceArrayCount)
        {
            if (typeLayout == nullptr)
            {
                AppendLine(m_BuildLog, "[ProgramReflection] Missing type layout at '" + parameterPath + "'.");
                return false;
            }

            const slang::TypeReflection::Kind typeKind = typeLayout->getKind();
            ProgramParameter parameter;
            parameter.Name = name;
            parameter.Path = parameterPath;
            parameter.Kind = GetParameterKind(typeKind);
            parameter.RelativeUniformOffset = relativeUniformOffset;
            if (typeKind == slang::TypeReflection::Kind::Scalar ||
                typeKind == slang::TypeReflection::Kind::Vector ||
                typeKind == slang::TypeReflection::Kind::Matrix ||
                typeKind == slang::TypeReflection::Kind::Enum)
            {
                parameter.ScalarType = static_cast<uint32_t>(typeLayout->getScalarType());
                parameter.RowCount = typeLayout->getRowCount();
                parameter.ColumnCount = typeLayout->getColumnCount();
            }
            if (typeKind == slang::TypeReflection::Kind::Matrix)
            {
                const SlangMatrixLayoutMode layoutMode = typeLayout->getMatrixLayoutMode();
                if (layoutMode == SLANG_MATRIX_LAYOUT_ROW_MAJOR)
                    parameter.MatrixLayout = ProgramMatrixLayout::RowMajor;
                else if (layoutMode == SLANG_MATRIX_LAYOUT_COLUMN_MAJOR)
                    parameter.MatrixLayout = ProgramMatrixLayout::ColumnMajor;
                else
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Matrix layout is unknown for '" +
                        parameterPath + "'.");
                    return false;
                }
            }

            const uint32_t parameterIndex = m_Reflection.AddParameter(parentIndex, parameter);
            if (parameterIndex == UINT_MAX)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Duplicate or invalid parameter node at '" +
                    parameterPath + "'.");
                return false;
            }

            switch (typeKind)
            {
            case slang::TypeReflection::Kind::Struct:
                return ReflectTypeContents(
                    typeLayout,
                    parameterIndex,
                    path,
                    uniformBindingIndex,
                    resourceArrayCount,
                    parameterPath);

            case slang::TypeReflection::Kind::Array:
                return ReflectArray(
                    typeLayout,
                    parameterIndex,
                    path,
                    parameterPath,
                    uniformBindingIndex,
                    resourceArrayCount);

            case slang::TypeReflection::Kind::ConstantBuffer:
            case slang::TypeReflection::Kind::ParameterBlock:
                return ReflectContainerContents(
                    typeLayout,
                    parameterIndex,
                    path,
                    parameterPath,
                    resourceArrayCount);

            case slang::TypeReflection::Kind::Resource:
            case slang::TypeReflection::Kind::TextureBuffer:
            case slang::TypeReflection::Kind::ShaderStorageBuffer:
            case slang::TypeReflection::Kind::DynamicResource:
            case slang::TypeReflection::Kind::SamplerState:
                return ReflectResource(
                    typeLayout,
                    parameterIndex,
                    path,
                    parameterPath,
                    resourceArrayCount);

            case slang::TypeReflection::Kind::Scalar:
            case slang::TypeReflection::Kind::Vector:
            case slang::TypeReflection::Kind::Matrix:
            case slang::TypeReflection::Kind::Enum:
                return ReflectUniform(typeLayout, parameterIndex, uniformBindingIndex, parameterPath);

            default:
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Unsupported parameter type at '" + parameterPath + "'.");
                return false;
            }
        }

        bool ReflectArray(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parameterIndex,
            const ReflectionPath& path,
            const std::string& parameterPath,
            uint32_t uniformBindingIndex,
            uint32_t resourceArrayCount)
        {
            uint32_t elementCount = 0;
            if (!TryConvertToUint32(typeLayout->getElementCount(), elementCount) || elementCount == 0)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Unbounded or unresolved arrays are not supported: '" +
                    parameterPath + "'.");
                return false;
            }

            slang::TypeLayoutReflection* elementType = typeLayout->getElementTypeLayout();
            if (elementType == nullptr)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Array element layout is unavailable: '" +
                    parameterPath + "'.");
                return false;
            }

            if (elementType->getKind() == slang::TypeReflection::Kind::ConstantBuffer ||
                elementType->getKind() == slang::TypeReflection::Kind::ParameterBlock)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Arrays of ConstantBuffer or ParameterBlock are not supported: '" +
                    parameterPath + "'.");
                return false;
            }

            const uint64_t combinedArrayCount =
                static_cast<uint64_t>(resourceArrayCount) * elementCount;
            if (combinedArrayCount > std::numeric_limits<uint32_t>::max())
            {
                AppendLine(m_BuildLog, "[ProgramReflection] Array is too large: '" + parameterPath + "'.");
                return false;
            }

            ProgramParameter* arrayParameter = m_Reflection.GetMutableParameter(parameterIndex);
            if (arrayParameter == nullptr)
                return false;

            arrayParameter->ArrayCount = elementCount;
            if (!TryGetElementStride(
                typeLayout,
                slang::ParameterCategory::Uniform,
                arrayParameter->UniformStride,
                parameterPath,
                m_BuildLog))
            {
                return false;
            }

            const uint32_t elementParameterIndex =
                static_cast<uint32_t>(m_Reflection.GetParameters().size());
            if (!ReflectTypeNode(
                elementType,
                parameterIndex,
                path,
                "",
                parameterPath,
                0,
                uniformBindingIndex,
                static_cast<uint32_t>(combinedArrayCount)))
            {
                return false;
            }

            m_Reflection.GetMutableParameter(parameterIndex)->ElementParameterIndex = elementParameterIndex;
            return true;
        }

        bool ReflectContainerContents(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parameterIndex,
            const ReflectionPath& path,
            const std::string& bindingName,
            uint32_t resourceArrayCount)
        {
            slang::VariableLayoutReflection* containerVariable = typeLayout->getContainerVarLayout();
            slang::VariableLayoutReflection* elementVariable = typeLayout->getElementVarLayout();
            if (containerVariable == nullptr || elementVariable == nullptr ||
                elementVariable->getTypeLayout() == nullptr)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Invalid parameter container layout: '" +
                    bindingName + "'.");
                return false;
            }

            ReflectionPath containerPath = path;
            containerPath.push_back(containerVariable);
            ReflectionPath elementPath = path;
            elementPath.push_back(elementVariable);

            slang::TypeLayoutReflection* elementType = elementVariable->getTypeLayout();
            uint32_t uniformSize = 0;
            uint32_t uniformTypeStride = 0;
            if (!TryGetTypeSize(
                elementType,
                slang::ParameterCategory::Uniform,
                uniformSize,
                bindingName,
                m_BuildLog) ||
                !TryGetTypeStride(
                    elementType,
                    slang::ParameterCategory::Uniform,
                    uniformTypeStride,
                    bindingName,
                    m_BuildLog) ||
                uniformTypeStride < uniformSize)
            {
                return false;
            }

            uint32_t uniformBindingIndex = UINT_MAX;
            if (uniformSize > 0)
            {
                ProgramBinding binding;
                binding.Name = bindingName;
                binding.Kind = ProgramBindingKind::ConstantBuffer;
                binding.SizeInBytes = uniformTypeStride;
                if (!TryGetBindingLocation(
                    containerPath,
                    slang::ParameterCategory::ConstantBuffer,
                    bindingName,
                    binding.Register,
                    binding.Space,
                    m_BuildLog))
                {
                    return false;
                }

                if (m_Desc.UsesRootConstants(bindingName))
                {
                    if (uniformTypeStride % sizeof(uint32_t) != 0)
                    {
                        AppendLine(
                            m_BuildLog,
                            "[ProgramReflection] Root constants parameter '" + bindingName +
                            "' must have a known 4-byte-aligned size.");
                        return false;
                    }
                    binding.Kind = ProgramBindingKind::RootConstants;
                    binding.Num32BitValues = uniformTypeStride / sizeof(uint32_t);
                    m_ReflectedRootConstants.insert(bindingName);
                }

                uniformBindingIndex = m_Reflection.AddBinding(binding);
                if (uniformBindingIndex == UINT_MAX)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Duplicate constant-data binding name: '" +
                        bindingName + "'.");
                    return false;
                }

                ProgramParameter* parameter = m_Reflection.GetMutableParameter(parameterIndex);
                if (parameter == nullptr)
                    return false;
                parameter->BindingIndex = uniformBindingIndex;
            }

            if (elementType->getKind() == slang::TypeReflection::Kind::Struct)
            {
                return ReflectTypeContents(
                    elementType,
                    parameterIndex,
                    elementPath,
                    uniformBindingIndex,
                    resourceArrayCount,
                    bindingName == "$Globals" ? "" : bindingName);
            }

            return ReflectTypeNode(
                elementType,
                parameterIndex,
                elementPath,
                "$value",
                JoinParameterPath(bindingName, "$value"),
                0,
                uniformBindingIndex,
                resourceArrayCount);
        }

        bool ReflectResource(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parameterIndex,
            const ReflectionPath& path,
            const std::string& parameterPath,
            uint32_t resourceArrayCount)
        {
            slang::ParameterCategory category = slang::ParameterCategory::None;
            ProgramBindingKind kind = ProgramBindingKind::SRV;
            if (!TryMapResourceKind(
                typeLayout,
                path,
                category,
                kind,
                parameterPath,
                m_BuildLog))
            {
                return false;
            }

            ProgramBinding binding;
            binding.Name = parameterPath;
            binding.Kind = kind;
            binding.Count = std::max(1u, resourceArrayCount);
            if (!TryGetBindingLocation(
                path,
                category,
                parameterPath,
                binding.Register,
                binding.Space,
                m_BuildLog))
            {
                return false;
            }

            if (m_Desc.FindStaticSampler(parameterPath) != nullptr)
            {
                if (binding.Kind != ProgramBindingKind::Sampler || binding.Count != 1)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Static sampler must be a non-array SamplerState: '" +
                        parameterPath + "'.");
                    return false;
                }
                binding.Kind = ProgramBindingKind::StaticSampler;
                m_ReflectedStaticSamplers.insert(parameterPath);
            }
            else if (m_Desc.UsesRootBufferSRV(parameterPath) ||
                m_Desc.UsesRootBufferUAV(parameterPath))
            {
                const bool wantsRootSRV = m_Desc.UsesRootBufferSRV(parameterPath);
                const bool wantsRootUAV = m_Desc.UsesRootBufferUAV(parameterPath);
                if (wantsRootSRV == wantsRootUAV)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Root buffer parameter must be either SRV or UAV, not both: '" +
                        parameterPath + "'.");
                    return false;
                }

                const ProgramBindingKind expectedKind = wantsRootSRV
                    ? ProgramBindingKind::SRV
                    : ProgramBindingKind::UAV;
                if (binding.Kind != expectedKind)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] " +
                        std::string(wantsRootSRV ? "RootBufferSRV" : "RootBufferUAV") +
                        " parameter '" + parameterPath + "' reflected as " +
                        (binding.Kind == ProgramBindingKind::SRV ? "SRV" :
                            binding.Kind == ProgramBindingKind::UAV ? "UAV" : "non-buffer resource") +
                        ".");
                    return false;
                }

                if (binding.Count != 1)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] " +
                        std::string(wantsRootSRV ? "RootBufferSRV" : "RootBufferUAV") +
                        " parameter must not be an array: '" + parameterPath + "'.");
                    return false;
                }

                if (!IsRootBufferResourceShape(typeLayout))
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] " +
                        std::string(wantsRootSRV ? "RootBufferSRV" : "RootBufferUAV") +
                        " parameter '" + parameterPath +
                        "' is not a buffer resource; reflected shape is " +
                        ResourceShapeToString(typeLayout->getResourceShape()) + ".");
                    return false;
                }

                binding.Kind = wantsRootSRV
                    ? ProgramBindingKind::RootBufferSRV
                    : ProgramBindingKind::RootBufferUAV;
                if (wantsRootSRV)
                    m_ReflectedRootBufferSRVs.insert(parameterPath);
                else
                    m_ReflectedRootBufferUAVs.insert(parameterPath);
            }

            const uint32_t bindingIndex = m_Reflection.AddBinding(binding);
            if (bindingIndex == UINT_MAX)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Duplicate resource binding name: '" +
                    parameterPath + "'.");
                return false;
            }

            ProgramParameter* parameter = m_Reflection.GetMutableParameter(parameterIndex);
            if (parameter == nullptr)
                return false;
            parameter->BindingIndex = bindingIndex;
            switch (binding.Kind)
            {
            case ProgramBindingKind::SRV:
            case ProgramBindingKind::RootBufferSRV:
                parameter->Kind = ProgramParameterKind::SRV;
                break;
            case ProgramBindingKind::UAV:
            case ProgramBindingKind::RootBufferUAV:
                parameter->Kind = ProgramParameterKind::UAV;
                break;
            case ProgramBindingKind::Sampler:
            case ProgramBindingKind::StaticSampler:
                parameter->Kind = ProgramParameterKind::Sampler;
                break;
            default:
                break;
            }
            return true;
        }

        bool ReflectUniform(
            slang::TypeLayoutReflection* typeLayout,
            uint32_t parameterIndex,
            uint32_t uniformBindingIndex,
            const std::string& parameterPath)
        {
            if (uniformBindingIndex == UINT_MAX)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Ordinary data has no owning constant buffer: '" +
                    parameterPath + "'.");
                return false;
            }

            uint32_t uniformSize = 0;
            uint32_t uniformTypeStride = 0;
            if (!TryGetTypeSize(
                typeLayout,
                slang::ParameterCategory::Uniform,
                uniformSize,
                parameterPath,
                m_BuildLog) ||
                !TryGetTypeStride(
                    typeLayout,
                    slang::ParameterCategory::Uniform,
                    uniformTypeStride,
                    parameterPath,
                    m_BuildLog) ||
                uniformSize == 0 || uniformTypeStride < uniformSize)
            {
                AppendLine(
                    m_BuildLog,
                    "[ProgramReflection] Invalid ordinary-data layout for '" +
                    parameterPath + "'.");
                return false;
            }

            ProgramParameter* parameter = m_Reflection.GetMutableParameter(parameterIndex);
            if (parameter == nullptr)
                return false;
            parameter->UniformBindingIndex = uniformBindingIndex;
            parameter->UniformSize = uniformSize;
            parameter->UniformTypeStride = uniformTypeStride;
            if (parameter->MatrixLayout != ProgramMatrixLayout::None)
            {
                const uint32_t majorCount =
                    parameter->MatrixLayout == ProgramMatrixLayout::RowMajor
                    ? parameter->RowCount
                    : parameter->ColumnCount;
                if (majorCount == 0 || uniformTypeStride % majorCount != 0)
                {
                    AppendLine(
                        m_BuildLog,
                        "[ProgramReflection] Invalid matrix storage layout for '" +
                        parameterPath + "'.");
                    return false;
                }
                parameter->MatrixVectorStride = uniformTypeStride / majorCount;
            }
            return true;
        }

        const ProgramDesc& m_Desc;
        ProgramReflection& m_Reflection;
        std::string& m_BuildLog;
        std::unordered_set<std::string> m_ReflectedRootConstants;
        std::unordered_set<std::string> m_ReflectedStaticSamplers;
        std::unordered_set<std::string> m_ReflectedRootBufferSRVs;
        std::unordered_set<std::string> m_ReflectedRootBufferUAVs;
    };
}

bool ReflectProgramLayout(
    const ProgramDesc& desc,
    slang::ShaderReflection* layout,
    ProgramReflection& reflection,
    std::string& buildLog)
{
    ProgramReflectionBuilder builder(desc, reflection, buildLog);
    return builder.Build(layout);
}

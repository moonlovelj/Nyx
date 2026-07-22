//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"
#include "ProgramManager.h"

#include "ProgramReflection.h"
#include "slang-com-ptr.h"
#include "slang.h"

#include <mutex>
#include <unordered_map>

#ifndef NYX_DUMP_PROGRAM_REFLECTION
#define NYX_DUMP_PROGRAM_REFLECTION 0
#endif

using Slang::ComPtr;

struct ProgramManager::SessionState
{
    ComPtr<slang::IGlobalSession> GlobalSession;
};

namespace
{
    void AppendDiagnostics(slang::IBlob* diagnostics, std::string& buildLog)
    {
        if (diagnostics == nullptr || diagnostics->getBufferPointer() == nullptr)
            return;

        const char* text = static_cast<const char*>(diagnostics->getBufferPointer());
        if (text[0] == 0)
            return;

        buildLog += text;
        if (buildLog.empty() || buildLog.back() != '\n')
            buildLog += "\n";
    }

    void AppendLine(std::string& buildLog, const std::string& line)
    {
        buildLog += line;
        buildLog += "\n";
    }

    slang::CompilerOptionEntry MakeIntOption(
        slang::CompilerOptionName name,
        int32_t value)
    {
        slang::CompilerOptionEntry entry = {};
        entry.name = name;
        entry.value.kind = slang::CompilerOptionValueKind::Int;
        entry.value.intValue0 = value;
        return entry;
    }

    const char* ParameterKindToString(ProgramParameterKind kind)
    {
        switch (kind)
        {
        case ProgramParameterKind::Root: return "Root";
        case ProgramParameterKind::Struct: return "Struct";
        case ProgramParameterKind::Array: return "Array";
        case ProgramParameterKind::ConstantBuffer: return "ConstantBuffer";
        case ProgramParameterKind::ParameterBlock: return "ParameterBlock";
        case ProgramParameterKind::Uniform: return "Uniform";
        case ProgramParameterKind::SRV: return "SRV";
        case ProgramParameterKind::UAV: return "UAV";
        case ProgramParameterKind::Sampler: return "Sampler";
        default: return "Unknown";
        }
    }

    const char* BindingKindToString(ProgramBindingKind kind)
    {
        switch (kind)
        {
        case ProgramBindingKind::ConstantBuffer: return "CBV";
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

    std::string GetBindingDebugName(const ProgramBinding& binding)
    {
        if (binding.Kind == ProgramBindingKind::ConstantBuffer ||
            binding.Kind == ProgramBindingKind::RootConstants)
        {
            return binding.Name + ".$uniforms";
        }
        return binding.Name;
    }

    uint32_t GetBindingNamespace(ProgramBindingKind kind)
    {
        switch (kind)
        {
        case ProgramBindingKind::ConstantBuffer:
        case ProgramBindingKind::RootConstants:
            return 0;
        case ProgramBindingKind::SRV:
        case ProgramBindingKind::RootBufferSRV:
            return 1;
        case ProgramBindingKind::UAV:
        case ProgramBindingKind::RootBufferUAV:
            return 2;
        case ProgramBindingKind::Sampler:
        case ProgramBindingKind::StaticSampler:
            return 3;
        default:
            return UINT_MAX;
        }
    }

    bool ValidateBindingRanges(
        const std::vector<ProgramBinding>& bindings,
        std::string& buildLog)
    {
        for (size_t firstIndex = 0; firstIndex < bindings.size(); ++firstIndex)
        {
            const ProgramBinding& first = bindings[firstIndex];
            const uint32_t firstNamespace = GetBindingNamespace(first.Kind);
            if (firstNamespace == UINT_MAX || first.Count == 0)
            {
                AppendLine(buildLog, "[ProgramReflection] Invalid physical binding range: '" + first.Name + "'.");
                return false;
            }

            const uint64_t firstEnd = static_cast<uint64_t>(first.Register) + first.Count;
            if (firstEnd > static_cast<uint64_t>(UINT_MAX) + 1)
            {
                AppendLine(buildLog, "[ProgramReflection] Binding range overflows: '" + first.Name + "'.");
                return false;
            }

            for (size_t secondIndex = firstIndex + 1; secondIndex < bindings.size(); ++secondIndex)
            {
                const ProgramBinding& second = bindings[secondIndex];
                if (firstNamespace != GetBindingNamespace(second.Kind) || first.Space != second.Space)
                    continue;

                const uint64_t secondEnd = static_cast<uint64_t>(second.Register) + second.Count;
                if (first.Register < secondEnd && second.Register < firstEnd)
                {
                    AppendLine(
                        buildLog,
                        "[ProgramReflection] Binding ranges overlap: '" + first.Name +
                        "' (register " + std::to_string(first.Register) +
                        ", space " + std::to_string(first.Space) +
                        ") and '" + second.Name + "' (register " +
                        std::to_string(second.Register) + ", space " +
                        std::to_string(second.Space) + ").");
                    return false;
                }
            }
        }
        return true;
    }

#ifdef _DEBUG
    void AppendParameterTreeNode(
        const ProgramReflection& reflection,
        uint32_t parameterIndex,
        uint32_t depth,
        std::string& buildLog)
    {
        const ProgramParameter* parameter = reflection.GetParameter(parameterIndex);
        if (parameter == nullptr)
            return;

        std::string line(depth * 2, ' ');
        if (parameter->Kind == ProgramParameterKind::Root)
            line += "$Root";
        else if (parameter->Name.empty())
            line += "[]";
        else
            line += parameter->Name;

        line += " : ";
        line += ParameterKindToString(parameter->Kind);
        if (parameter->RelativeUniformOffset != 0)
            line += " relativeUniform=" + std::to_string(parameter->RelativeUniformOffset);

        if (parameter->Kind == ProgramParameterKind::Array)
        {
            line += " count=" + std::to_string(parameter->ArrayCount);
            line += " uniformStride=" + std::to_string(parameter->UniformStride);
        }

        if (parameter->Kind == ProgramParameterKind::Uniform)
        {
            line += " size=" + std::to_string(parameter->UniformSize);
            if (parameter->UniformTypeStride > parameter->UniformSize)
                line += " typeStride=" + std::to_string(parameter->UniformTypeStride);
            line += " scalarType=" + std::to_string(parameter->ScalarType);
            line += " shape=" + std::to_string(parameter->RowCount) + "x" +
                std::to_string(parameter->ColumnCount);
            if (parameter->MatrixLayout != ProgramMatrixLayout::None)
            {
                line += parameter->MatrixLayout == ProgramMatrixLayout::RowMajor
                    ? " rowMajor"
                    : " columnMajor";
                line += " matrixStride=" + std::to_string(parameter->MatrixVectorStride);
            }
            if (parameter->UniformBindingIndex < reflection.GetBindings().size())
            {
                line += " owner='";
                line += reflection.GetBindings()[parameter->UniformBindingIndex].Name;
                line += "'";
            }
        }
        else if (parameter->BindingIndex < reflection.GetBindings().size())
        {
            line += " binding='";
            line += reflection.GetBindings()[parameter->BindingIndex].Name;
            line += "'";
        }

        AppendLine(buildLog, line);
        for (uint32_t childIndex : parameter->Children)
            AppendParameterTreeNode(reflection, childIndex, depth + 1, buildLog);
    }

    void AppendReflectionDebugLog(
        const ProgramReflection& reflection,
        std::string& buildLog)
    {
        AppendLine(buildLog, "[ProgramReflection] Parameter tree:");
        AppendParameterTreeNode(
            reflection,
            ProgramReflection::RootParameterIndex,
            0,
            buildLog);

        AppendLine(buildLog, "[ProgramReflection] Root signature layout:");
        for (const ProgramBinding& binding : reflection.GetBindings())
        {
            std::string line = "  '" + GetBindingDebugName(binding) + "' ";
            line += BindingKindToString(binding.Kind);
            line += " register=";
            switch (binding.Kind)
            {
            case ProgramBindingKind::ConstantBuffer:
            case ProgramBindingKind::RootConstants: line += "b"; break;
            case ProgramBindingKind::SRV:
            case ProgramBindingKind::RootBufferSRV: line += "t"; break;
            case ProgramBindingKind::UAV:
            case ProgramBindingKind::RootBufferUAV: line += "u"; break;
            case ProgramBindingKind::Sampler:
            case ProgramBindingKind::StaticSampler: line += "s"; break;
            default: break;
            }
            line += std::to_string(binding.Register);
            line += " space=" + std::to_string(binding.Space);
            line += " count=" + std::to_string(binding.Count);

            if (binding.Kind == ProgramBindingKind::StaticSampler)
                line += " static";
            else
            {
                line += " root=" + std::to_string(binding.RootIndex);
                if (binding.IsDescriptorTableBinding())
                    line += " tableOffset=" + std::to_string(binding.TableOffset);
            }

            if (binding.Kind == ProgramBindingKind::ConstantBuffer ||
                binding.Kind == ProgramBindingKind::RootConstants)
            {
                line += " bytes=" + std::to_string(binding.SizeInBytes);
            }
            AppendLine(buildLog, line);
        }
    }
#endif

    bool BuildRootSignatureFromReflection(
        const ProgramDesc& desc,
        std::vector<ProgramBinding>& bindings,
        bool usesDescriptorHeapIndexing,
        RootSignature& rootSignature,
        std::string& buildLog)
    {
        (void)usesDescriptorHeapIndexing;
#if defined(_DEBUG) && NYX_DUMP_PROGRAM_REFLECTION
        AppendLine(buildLog, "[ProgramReflection] Reflected physical bindings:");
        if (usesDescriptorHeapIndexing)
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Direct descriptor heap root signature flags requested.");
        }
        for (const ProgramBinding& binding : bindings)
        {
            AppendLine(
                buildLog,
                "  '" + GetBindingDebugName(binding) + "' " +
                BindingKindToString(binding.Kind) +
                " register=" + std::to_string(binding.Register) +
                " space=" + std::to_string(binding.Space) +
                " count=" + std::to_string(binding.Count));
        }
#endif

        if (!ValidateBindingRanges(bindings, buildLog))
            return false;

        std::vector<size_t> cbvBindings;
        std::vector<size_t> rootConstantBindings;
        std::vector<size_t> rootBufferSRVBindings;
        std::vector<size_t> rootBufferUAVBindings;
        std::vector<size_t> srvBindings;
        std::vector<size_t> uavBindings;
        std::vector<size_t> samplerBindings;
        std::vector<size_t> staticSamplerBindings;

        for (size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex)
        {
            switch (bindings[bindingIndex].Kind)
            {
            case ProgramBindingKind::ConstantBuffer: cbvBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::RootConstants: rootConstantBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::RootBufferSRV: rootBufferSRVBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::RootBufferUAV: rootBufferUAVBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::SRV: srvBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::UAV: uavBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::Sampler: samplerBindings.push_back(bindingIndex); break;
            case ProgramBindingKind::StaticSampler: staticSamplerBindings.push_back(bindingIndex); break;
            default: break;
            }
        }

        const uint32_t rootParamCount =
            static_cast<uint32_t>(cbvBindings.size()) +
            static_cast<uint32_t>(rootConstantBindings.size()) +
            static_cast<uint32_t>(rootBufferSRVBindings.size()) +
            static_cast<uint32_t>(rootBufferUAVBindings.size()) +
            (srvBindings.empty() ? 0u : 1u) +
            (uavBindings.empty() ? 0u : 1u) +
            (samplerBindings.empty() ? 0u : 1u);
        if (rootParamCount > 16)
        {
            AppendLine(buildLog, "[ProgramReflection] Auto root signature exceeds 16 root parameters.");
            return false;
        }

        uint64_t rootSignatureDwordCount = static_cast<uint64_t>(cbvBindings.size()) * 2;
        for (size_t bindingIndex : rootConstantBindings)
            rootSignatureDwordCount += bindings[bindingIndex].Num32BitValues;
        rootSignatureDwordCount += static_cast<uint64_t>(rootBufferSRVBindings.size()) * 2;
        rootSignatureDwordCount += static_cast<uint64_t>(rootBufferUAVBindings.size()) * 2;
        rootSignatureDwordCount += srvBindings.empty() ? 0u : 1u;
        rootSignatureDwordCount += uavBindings.empty() ? 0u : 1u;
        rootSignatureDwordCount += samplerBindings.empty() ? 0u : 1u;
        if (rootSignatureDwordCount > 64)
        {
            AppendLine(
                buildLog,
                "[ProgramReflection] Auto root signature costs " +
                std::to_string(rootSignatureDwordCount) +
                " DWORDs; D3D12 allows at most 64.");
            return false;
        }

        auto validateDescriptorTable = [&bindings, &buildLog](
            const std::vector<size_t>& bindingIndices,
            const char* tableName)
        {
            uint64_t descriptorCount = 0;
            for (size_t bindingIndex : bindingIndices)
                descriptorCount += bindings[bindingIndex].Count;

            // DynamicDescriptorHeap currently tracks assigned table slots with
            // a 32-bit bitmap. Reject larger reflected tables instead of
            // allowing undefined shifts and incomplete descriptor copies.
            if (descriptorCount > 32)
            {
                AppendLine(
                    buildLog,
                    std::string("[ProgramReflection] ") + tableName +
                    " descriptor table requires " + std::to_string(descriptorCount) +
                    " slots; the current dynamic descriptor heap supports at most 32 per table.");
                return false;
            }
            return true;
        };
        if (!validateDescriptorTable(srvBindings, "SRV") ||
            !validateDescriptorTable(uavBindings, "UAV") ||
            !validateDescriptorTable(samplerBindings, "sampler"))
        {
            return false;
        }

        rootSignature.Reset(
            rootParamCount,
            static_cast<uint32_t>(staticSamplerBindings.size()));

        uint32_t rootIndex = 0;
        for (size_t bindingIndex : cbvBindings)
        {
            ProgramBinding& binding = bindings[bindingIndex];
            binding.RootIndex = rootIndex;
            binding.TableOffset = 0;
            rootSignature[rootIndex].InitAsConstantBuffer(
                binding.Register,
                D3D12_SHADER_VISIBILITY_ALL,
                binding.Space);
            ++rootIndex;
        }

        for (size_t bindingIndex : rootConstantBindings)
        {
            ProgramBinding& binding = bindings[bindingIndex];
            binding.RootIndex = rootIndex;
            binding.TableOffset = 0;
            rootSignature[rootIndex].InitAsConstants(
                binding.Register,
                binding.Num32BitValues,
                D3D12_SHADER_VISIBILITY_ALL,
                binding.Space);
            ++rootIndex;
        }

        for (size_t bindingIndex : rootBufferSRVBindings)
        {
            ProgramBinding& binding = bindings[bindingIndex];
            binding.RootIndex = rootIndex;
            binding.TableOffset = 0;
            rootSignature[rootIndex].InitAsBufferSRV(
                binding.Register,
                D3D12_SHADER_VISIBILITY_ALL,
                binding.Space);
            ++rootIndex;
        }

        for (size_t bindingIndex : rootBufferUAVBindings)
        {
            ProgramBinding& binding = bindings[bindingIndex];
            binding.RootIndex = rootIndex;
            binding.TableOffset = 0;
            rootSignature[rootIndex].InitAsBufferUAV(
                binding.Register,
                D3D12_SHADER_VISIBILITY_ALL,
                binding.Space);
            ++rootIndex;
        }

        auto initDescriptorTable = [&rootSignature, &bindings, &rootIndex](
            const std::vector<size_t>& bindingIndices,
            D3D12_DESCRIPTOR_RANGE_TYPE rangeType)
        {
            if (bindingIndices.empty())
                return;

            const uint32_t tableRootIndex = rootIndex++;
            rootSignature[tableRootIndex].InitAsDescriptorTable(
                static_cast<UINT>(bindingIndices.size()));

            uint32_t tableOffset = 0;
            for (UINT rangeIndex = 0; rangeIndex < bindingIndices.size(); ++rangeIndex)
            {
                ProgramBinding& binding = bindings[bindingIndices[rangeIndex]];
                binding.RootIndex = tableRootIndex;
                binding.TableOffset = tableOffset;
                rootSignature[tableRootIndex].SetTableRange(
                    rangeIndex,
                    rangeType,
                    binding.Register,
                    binding.Count,
                    binding.Space);
                tableOffset += binding.Count;
            }
        };

        initDescriptorTable(srvBindings, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
        initDescriptorTable(uavBindings, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
        initDescriptorTable(samplerBindings, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);

        for (size_t bindingIndex : staticSamplerBindings)
        {
            ProgramBinding& binding = bindings[bindingIndex];
            const ProgramDesc::StaticSampler* sampler = desc.FindStaticSampler(binding.Name);
            ASSERT(sampler != nullptr);
            if (sampler == nullptr)
                return false;

            binding.RootIndex = UINT_MAX;
            binding.TableOffset = 0;
            rootSignature.InitStaticSampler(
                binding.Register,
                sampler->Desc,
                sampler->Visibility,
                binding.Space);
        }

        const std::string rootSignatureName =
            "Program RS: " + Utility::RemoveBasePath(desc.GetSourceFile());
        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = desc.GetRootSignatureFlags();

#if defined(_DEBUG) && NYX_DUMP_PROGRAM_REFLECTION
        Utility::Printf("[ProgramReflection] Creating root signature '%s':\n", rootSignatureName.c_str());
        if (rootSignatureFlags != D3D12_ROOT_SIGNATURE_FLAG_NONE)
        {
            Utility::Printf(
                "  RootFlags=%d\n",
                rootSignatureFlags);
        }
        for (const ProgramBinding& binding : bindings)
        {
            const std::string bindingDebugName = GetBindingDebugName(binding);
            Utility::Printf(
                "  %s '%s': register=%u space=%u count=%u root=%u tableOffset=%u bytes=%u\n",
                BindingKindToString(binding.Kind),
                bindingDebugName.c_str(),
                binding.Register,
                binding.Space,
                binding.Count,
                binding.RootIndex,
                binding.TableOffset,
                binding.SizeInBytes);
        }
#endif
        rootSignature.Finalize(Utility::UTF8ToWideString(rootSignatureName), rootSignatureFlags);
        return true;
    }
}

ProgramManager& ProgramManager::Get()
{
    static ProgramManager s_Manager;
    return s_Manager;
}

std::shared_ptr<Program> ProgramManager::GetProgram(const ProgramDesc& desc, std::string* outBuildLog)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    const std::string key = desc.GetCacheKey();
    auto iter = m_Cache.find(key);
    if (iter != m_Cache.end())
    {
        if (outBuildLog != nullptr)
            *outBuildLog = iter->second->GetBuildLog();
        return iter->second;
    }

    std::string buildLog;
    std::shared_ptr<Program> program = BuildProgram(desc, buildLog);
    if (outBuildLog != nullptr)
        *outBuildLog = buildLog;

    if (program)
        m_Cache[key] = program;
    return program;
}

void ProgramManager::ClearCache()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Cache.clear();
}

bool ProgramManager::EnsureGlobalSession(std::string& buildLog)
{
    if (m_SessionState && m_SessionState->GlobalSession)
        return true;

    m_SessionState.reset(new SessionState());
    const SlangResult result = slang::createGlobalSession(m_SessionState->GlobalSession.writeRef());
    if (SLANG_FAILED(result))
    {
        AppendLine(buildLog, "Failed to create shader compilation global session.");
        m_SessionState.reset();
        return false;
    }
    return true;
}

std::shared_ptr<Program> ProgramManager::BuildProgram(const ProgramDesc& desc, std::string& buildLog)
{
    buildLog.clear();
    if (!desc.IsValid())
    {
        AppendLine(buildLog, "Invalid ProgramDesc. Source file and at least one entry point are required.");
        return nullptr;
    }
    if (!EnsureGlobalSession(buildLog))
        return nullptr;

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_DXIL;
    targetDesc.profile = m_SessionState->GlobalSession->findProfile(desc.GetTargetProfile().c_str());
    if (targetDesc.profile == SLANG_PROFILE_UNKNOWN)
    {
        AppendLine(buildLog, "Unknown shader target profile: " + desc.GetTargetProfile());
        return nullptr;
    }

    std::vector<slang::CompilerOptionEntry> compilerOptions;
    if (desc.GetGenerateDebugInfo())
    {
        compilerOptions.push_back(MakeIntOption(
            slang::CompilerOptionName::DebugInformation,
            SLANG_DEBUG_INFO_LEVEL_MAXIMAL));
        compilerOptions.push_back(MakeIntOption(
            slang::CompilerOptionName::DebugInformationFormat,
            SLANG_DEBUG_INFO_FORMAT_C7));
        compilerOptions.push_back(MakeIntOption(
            slang::CompilerOptionName::Optimization,
            SLANG_OPTIMIZATION_LEVEL_NONE));
        targetDesc.compilerOptionEntries = compilerOptions.data();
        targetDesc.compilerOptionEntryCount = static_cast<uint32_t>(compilerOptions.size());
    }

    std::vector<std::string> searchPaths;
    const std::string basePath = Utility::GetBasePath(desc.GetSourceFile());
    if (!basePath.empty())
        searchPaths.push_back(basePath);
    for (const std::string& includeDirectory : desc.GetIncludeDirectories())
        searchPaths.push_back(includeDirectory);

    std::vector<const char*> searchPathPtrs;
    searchPathPtrs.reserve(searchPaths.size());
    for (const std::string& path : searchPaths)
        searchPathPtrs.push_back(path.c_str());

    std::vector<slang::PreprocessorMacroDesc> macros;
    macros.reserve(desc.GetDefines().size());
    for (const ProgramDesc::Define& define : desc.GetDefines())
    {
        slang::PreprocessorMacroDesc macro = {};
        macro.name = define.Name.c_str();
        macro.value = define.Value.c_str();
        macros.push_back(macro);
    }

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    sessionDesc.searchPaths = searchPathPtrs.empty() ? nullptr : searchPathPtrs.data();
    sessionDesc.searchPathCount = static_cast<SlangInt>(searchPathPtrs.size());
    sessionDesc.preprocessorMacros = macros.empty() ? nullptr : macros.data();
    sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());

    ComPtr<slang::ISession> session;
    SlangResult result = m_SessionState->GlobalSession->createSession(sessionDesc, session.writeRef());
    if (SLANG_FAILED(result) || !session)
    {
        AppendLine(buildLog, "Failed to create shader compilation session.");
        return nullptr;
    }

    ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModule(desc.GetSourceFile().c_str(), diagnostics.writeRef());
    AppendDiagnostics(diagnostics, buildLog);
    if (!module)
    {
        AppendLine(buildLog, "Failed to load shader module: " + desc.GetSourceFile());
        return nullptr;
    }

    std::vector<ComPtr<slang::IEntryPoint>> entryPoints;
    std::vector<slang::IComponentType*> componentTypes;
    componentTypes.reserve(desc.GetEntryPoints().size() + 1);
    componentTypes.push_back(module);

    for (const ProgramDesc::EntryPoint& entryPointDesc : desc.GetEntryPoints())
    {
        ComPtr<slang::IEntryPoint> entryPoint;
        result = module->findEntryPointByName(entryPointDesc.Name.c_str(), entryPoint.writeRef());
        if (SLANG_FAILED(result) || !entryPoint)
        {
            AppendLine(buildLog, "Failed to find shader entry point: " + entryPointDesc.Name);
            return nullptr;
        }
        componentTypes.push_back(entryPoint);
        entryPoints.push_back(entryPoint);
    }

    ComPtr<slang::IComponentType> composedProgram;
    diagnostics = nullptr;
    result = session->createCompositeComponentType(
        componentTypes.data(),
        componentTypes.size(),
        composedProgram.writeRef(),
        diagnostics.writeRef());
    AppendDiagnostics(diagnostics, buildLog);
    if (SLANG_FAILED(result) || !composedProgram)
    {
        AppendLine(buildLog, "Failed to compose shader program.");
        return nullptr;
    }

    ComPtr<slang::IComponentType> linkedProgram;
    diagnostics = nullptr;
    result = composedProgram->link(linkedProgram.writeRef(), diagnostics.writeRef());
    AppendDiagnostics(diagnostics, buildLog);
    if (SLANG_FAILED(result) || !linkedProgram)
    {
        AppendLine(buildLog, "Failed to link shader program.");
        return nullptr;
    }

    std::shared_ptr<Program> program(new Program(desc, m_NextVersionId++, buildLog));
    const int targetIndex = 0;

    ComPtr<slang::IBlob> layoutDiagnostics;
    slang::ProgramLayout* layout = linkedProgram->getLayout(targetIndex, layoutDiagnostics.writeRef());
    AppendDiagnostics(layoutDiagnostics, buildLog);
    if (layout == nullptr)
    {
        AppendLine(buildLog, "Failed to reflect shader program layout.");
        return nullptr;
    }

    ProgramReflection& reflection = program->GetMutableReflection();
    if (!ReflectProgramLayout(desc, layout, reflection, buildLog))
        return nullptr;
    if (!BuildRootSignatureFromReflection(
        desc,
        reflection.GetMutableBindings(),
        reflection.UsesDescriptorHeapIndexing(),
        program->GetMutableRootSignature(),
        buildLog))
    {
        return nullptr;
    }

    for (size_t entryPointIndex = 0; entryPointIndex < desc.GetEntryPoints().size(); ++entryPointIndex)
    {
        ComPtr<slang::IBlob> code;
        diagnostics = nullptr;
        result = linkedProgram->getEntryPointCode(
            static_cast<SlangInt>(entryPointIndex),
            targetIndex,
            code.writeRef(),
            diagnostics.writeRef());
        AppendDiagnostics(diagnostics, buildLog);
        if (SLANG_FAILED(result) || !code)
        {
            AppendLine(
                buildLog,
                "Failed to emit shader bytecode for entry point: " +
                desc.GetEntryPoints()[entryPointIndex].Name);
            return nullptr;
        }

        program->SetBytecode(
            desc.GetEntryPoints()[entryPointIndex].Stage,
            code->getBufferPointer(),
            code->getBufferSize());
    }

#if defined(_DEBUG) && NYX_DUMP_PROGRAM_REFLECTION
    AppendReflectionDebugLog(reflection, buildLog);
#endif
    program->SetBuildLog(buildLog);
    return program;
}

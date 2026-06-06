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
#include <algorithm>
#include <cctype>

enum class ShaderStage : uint8_t
{
    Vertex = 0,
    Pixel,
    Geometry,
    Hull,
    Domain,
    Compute,
    Mesh,
    Amplification,
    Count
};

class ProgramDesc
{
public:
    struct EntryPoint
    {
        ShaderStage Stage = ShaderStage::Compute;
        std::string Name;
    };

    struct Define
    {
        std::string Name;
        std::string Value;
    };

    struct StaticSampler
    {
        std::string ParameterName;
        D3D12_SAMPLER_DESC Desc = {};
        D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL;
    };

    ProgramDesc() = default;
    explicit ProgramDesc(const std::string& sourceFile) : m_SourceFile(sourceFile) {}

    ProgramDesc& SetSourceFile(const std::string& sourceFile)
    {
        m_SourceFile = sourceFile;
        return *this;
    }

    ProgramDesc& SetTargetProfile(const std::string& profile)
    {
        m_TargetProfile = profile;
        return *this;
    }

    ProgramDesc& SetGenerateDebugInfo(bool enable)
    {
        m_GenerateDebugInfo = enable;
        return *this;
    }

    ProgramDesc& AddEntryPoint(ShaderStage stage, const std::string& name)
    {
        ASSERT(!name.empty());
        for (EntryPoint& entryPoint : m_EntryPoints)
        {
            if (entryPoint.Stage == stage)
            {
                entryPoint.Name = name;
                return *this;
            }
        }

        EntryPoint entryPoint;
        entryPoint.Stage = stage;
        entryPoint.Name = name;
        m_EntryPoints.push_back(entryPoint);
        return *this;
    }

    ProgramDesc& AddDefine(const std::string& name, const std::string& value = "1")
    {
        ASSERT(!name.empty());
        for (Define& define : m_Defines)
        {
            if (define.Name == name)
            {
                define.Value = value;
                return *this;
            }
        }

        Define define;
        define.Name = name;
        define.Value = value;
        m_Defines.push_back(define);
        return *this;
    }

    ProgramDesc& AddIncludeDirectory(const std::string& directory)
    {
        m_IncludeDirectories.push_back(directory);
        return *this;
    }

    ProgramDesc& AddRootSignatureFlags(D3D12_ROOT_SIGNATURE_FLAGS flags)
    {
        m_RootSignatureFlags |= flags;
        return *this;
    }

    ProgramDesc& AddRootConstants(const std::string& parameterName)
    {
        ASSERT(!parameterName.empty());
        if (!UsesRootConstants(parameterName))
            m_RootConstantParameters.push_back(parameterName);
        return *this;
    }

    ProgramDesc& AddRootBufferSRV(const std::string& parameterName)
    {
        ASSERT(!parameterName.empty());
        ASSERT(!UsesRootBufferUAV(parameterName));
        if (!UsesRootBufferSRV(parameterName))
            m_RootBufferSRVParameters.push_back(parameterName);
        return *this;
    }

    ProgramDesc& AddRootBufferUAV(const std::string& parameterName)
    {
        ASSERT(!parameterName.empty());
        ASSERT(!UsesRootBufferSRV(parameterName));
        if (!UsesRootBufferUAV(parameterName))
            m_RootBufferUAVParameters.push_back(parameterName);
        return *this;
    }

    ProgramDesc& AddStaticSampler(
        const std::string& parameterName,
        const D3D12_SAMPLER_DESC& samplerDesc,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
    {
        ASSERT(!parameterName.empty());

        for (StaticSampler& sampler : m_StaticSamplers)
        {
            if (sampler.ParameterName == parameterName)
            {
                sampler.Desc = samplerDesc;
                sampler.Visibility = visibility;
                return *this;
            }
        }

        StaticSampler sampler;
        sampler.ParameterName = parameterName;
        sampler.Desc = samplerDesc;
        sampler.Visibility = visibility;
        m_StaticSamplers.push_back(sampler);
        return *this;
    }

    const std::string& GetSourceFile() const { return m_SourceFile; }
    const std::string& GetTargetProfile() const { return m_TargetProfile; }
    bool GetGenerateDebugInfo() const { return m_GenerateDebugInfo; }
    const std::vector<EntryPoint>& GetEntryPoints() const { return m_EntryPoints; }
    const std::vector<Define>& GetDefines() const { return m_Defines; }
    const std::vector<std::string>& GetIncludeDirectories() const { return m_IncludeDirectories; }
    const std::vector<std::string>& GetRootConstantParameters() const { return m_RootConstantParameters; }
    const std::vector<std::string>& GetRootBufferSRVParameters() const { return m_RootBufferSRVParameters; }
    const std::vector<std::string>& GetRootBufferUAVParameters() const { return m_RootBufferUAVParameters; }
    const std::vector<StaticSampler>& GetStaticSamplers() const { return m_StaticSamplers; }
    D3D12_ROOT_SIGNATURE_FLAGS GetRootSignatureFlags() const { return m_RootSignatureFlags; }

    bool UsesRootConstants(const std::string& parameterName) const
    {
        for (const std::string& name : m_RootConstantParameters)
        {
            if (name == parameterName)
                return true;
        }
        return false;
    }

    bool UsesRootBufferSRV(const std::string& parameterName) const
    {
        for (const std::string& name : m_RootBufferSRVParameters)
        {
            if (name == parameterName)
                return true;
        }
        return false;
    }

    bool UsesRootBufferUAV(const std::string& parameterName) const
    {
        for (const std::string& name : m_RootBufferUAVParameters)
        {
            if (name == parameterName)
                return true;
        }
        return false;
    }

    const StaticSampler* FindStaticSampler(const std::string& parameterName) const
    {
        for (const StaticSampler& sampler : m_StaticSamplers)
        {
            if (sampler.ParameterName == parameterName)
                return &sampler;
        }
        return nullptr;
    }

    bool IsValid() const
    {
        return !m_SourceFile.empty() && !m_TargetProfile.empty() && !m_EntryPoints.empty();
    }

    std::string GetCacheKey() const
    {
        std::string key;
        AppendCacheField(key, "format", "ProgramDescKey:v3");
        AppendCacheField(key, "source", NormalizePathForCache(m_SourceFile));
        AppendCacheField(key, "profile", m_TargetProfile);
        AppendCacheField(key, "debug", m_GenerateDebugInfo ? "1" : "0");
        AppendCacheField(
            key,
            "rootSignatureFlags",
            std::to_string(static_cast<uint32_t>(m_RootSignatureFlags)));

        std::vector<const EntryPoint*> sortedEntryPoints;
        sortedEntryPoints.reserve(m_EntryPoints.size());
        for (const EntryPoint& entryPoint : m_EntryPoints)
            sortedEntryPoints.push_back(&entryPoint);
        std::sort(
            sortedEntryPoints.begin(),
            sortedEntryPoints.end(),
            [](const EntryPoint* lhs, const EntryPoint* rhs)
            {
                if (lhs->Stage != rhs->Stage)
                    return static_cast<uint32_t>(lhs->Stage) < static_cast<uint32_t>(rhs->Stage);
                return lhs->Name < rhs->Name;
            });

        AppendCacheField(key, "entry.count", std::to_string(sortedEntryPoints.size()));
        for (const EntryPoint* entryPoint : sortedEntryPoints)
        {
            AppendCacheField(
                key,
                "entry.stage",
                std::to_string(static_cast<uint32_t>(entryPoint->Stage)));
            AppendCacheField(key, "entry.name", entryPoint->Name);
        }

        std::vector<const Define*> sortedDefines;
        sortedDefines.reserve(m_Defines.size());
        for (const Define& define : m_Defines)
            sortedDefines.push_back(&define);
        std::sort(
            sortedDefines.begin(),
            sortedDefines.end(),
            [](const Define* lhs, const Define* rhs)
            {
                return lhs->Name < rhs->Name;
            });

        AppendCacheField(key, "define.count", std::to_string(sortedDefines.size()));
        for (const Define* define : sortedDefines)
        {
            AppendCacheField(key, "define.name", define->Name);
            AppendCacheField(key, "define.value", define->Value);
        }

        AppendCacheField(key, "include.count", std::to_string(m_IncludeDirectories.size()));
        for (const std::string& includeDirectory : m_IncludeDirectories)
            AppendCacheField(key, "include", NormalizePathForCache(includeDirectory));

        std::vector<std::string> sortedRootConstantParameters = m_RootConstantParameters;
        std::sort(sortedRootConstantParameters.begin(), sortedRootConstantParameters.end());

        AppendCacheField(
            key,
            "rootConstants.count",
            std::to_string(sortedRootConstantParameters.size()));
        for (const std::string& parameterName : sortedRootConstantParameters)
            AppendCacheField(key, "rootConstants", parameterName);

        std::vector<std::string> sortedRootBufferSRVParameters = m_RootBufferSRVParameters;
        std::sort(sortedRootBufferSRVParameters.begin(), sortedRootBufferSRVParameters.end());

        AppendCacheField(
            key,
            "rootBufferSRV.count",
            std::to_string(sortedRootBufferSRVParameters.size()));
        for (const std::string& parameterName : sortedRootBufferSRVParameters)
            AppendCacheField(key, "rootBufferSRV", parameterName);

        std::vector<std::string> sortedRootBufferUAVParameters = m_RootBufferUAVParameters;
        std::sort(sortedRootBufferUAVParameters.begin(), sortedRootBufferUAVParameters.end());

        AppendCacheField(
            key,
            "rootBufferUAV.count",
            std::to_string(sortedRootBufferUAVParameters.size()));
        for (const std::string& parameterName : sortedRootBufferUAVParameters)
            AppendCacheField(key, "rootBufferUAV", parameterName);

        std::vector<const StaticSampler*> sortedStaticSamplers;
        sortedStaticSamplers.reserve(m_StaticSamplers.size());
        for (const StaticSampler& sampler : m_StaticSamplers)
            sortedStaticSamplers.push_back(&sampler);
        std::sort(
            sortedStaticSamplers.begin(),
            sortedStaticSamplers.end(),
            [](const StaticSampler* lhs, const StaticSampler* rhs)
            {
                return lhs->ParameterName < rhs->ParameterName;
            });

        AppendCacheField(key, "staticSampler.count", std::to_string(sortedStaticSamplers.size()));
        for (const StaticSampler* sampler : sortedStaticSamplers)
        {
            AppendCacheField(key, "staticSampler.name", sampler->ParameterName);
            AppendSamplerCacheKey(key, sampler->Desc, sampler->Visibility);
        }

        return key;
    }

private:
    static void AppendCacheField(std::string& key, const char* tag, const std::string& value)
    {
        key += tag;
        key += ":";
        key += std::to_string(value.size());
        key += ":";
        key += value;
        key += "\n";
    }

    static std::string NormalizePathForCache(const std::string& path)
    {
        if (path.empty())
            return path;

        std::string normalizedPath;
        DWORD requiredLength = GetFullPathNameA(path.c_str(), 0, nullptr, nullptr);
        if (requiredLength > 0)
        {
            std::string fullPath(requiredLength, '\0');
            DWORD actualLength =
                GetFullPathNameA(path.c_str(), requiredLength, &fullPath[0], nullptr);
            if (actualLength > 0 && actualLength < requiredLength)
            {
                fullPath.resize(actualLength);
                normalizedPath = fullPath;
            }
        }

        if (normalizedPath.empty())
            normalizedPath = path;

        for (char& ch : normalizedPath)
        {
            if (ch == '\\')
                ch = '/';
            else
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return normalizedPath;
    }

    static void AppendSamplerCacheKey(
        std::string& key,
        const D3D12_SAMPLER_DESC& desc,
        D3D12_SHADER_VISIBILITY visibility)
    {
        auto appendUint = [&key](const char* tag, uint32_t value)
        {
            AppendCacheField(key, tag, std::to_string(value));
        };
        auto appendFloat = [&appendUint](const char* tag, float value)
        {
            uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value), "Unexpected float size");
            memcpy(&bits, &value, sizeof(bits));
            appendUint(tag, bits);
        };

        appendUint("staticSampler.filter", static_cast<uint32_t>(desc.Filter));
        appendUint("staticSampler.addressU", static_cast<uint32_t>(desc.AddressU));
        appendUint("staticSampler.addressV", static_cast<uint32_t>(desc.AddressV));
        appendUint("staticSampler.addressW", static_cast<uint32_t>(desc.AddressW));
        appendFloat("staticSampler.mipLODBias", desc.MipLODBias);
        appendUint("staticSampler.maxAnisotropy", desc.MaxAnisotropy);
        appendUint("staticSampler.comparisonFunc", static_cast<uint32_t>(desc.ComparisonFunc));
        for (float borderComponent : desc.BorderColor)
            appendFloat("staticSampler.borderColor", borderComponent);
        appendFloat("staticSampler.minLOD", desc.MinLOD);
        appendFloat("staticSampler.maxLOD", desc.MaxLOD);
        appendUint("staticSampler.visibility", static_cast<uint32_t>(visibility));
    }

    std::string m_SourceFile;
    std::string m_TargetProfile = "sm_6_6";
    bool m_GenerateDebugInfo =
#ifdef RELEASE
        false;
#else
        true;
#endif
    std::vector<EntryPoint> m_EntryPoints;
    std::vector<Define> m_Defines;
    std::vector<std::string> m_IncludeDirectories;
    std::vector<std::string> m_RootConstantParameters;
    std::vector<std::string> m_RootBufferSRVParameters;
    std::vector<std::string> m_RootBufferUAVParameters;
    std::vector<StaticSampler> m_StaticSamplers;
    D3D12_ROOT_SIGNATURE_FLAGS m_RootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
};

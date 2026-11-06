#include "pch.h"
#include "DlssBackend.h"
#include "EngineVersion.h"
#include "GraphicsCore.h"
#include "CommandContext.h"
#include "CommandListManager.h"
#include "ColorBuffer.h"
#include "DepthBuffer.h"
#include <filesystem>
#include <cmath>
#include <shlobj.h>

#if NYX_ENABLE_DLSS
#include "DlssResolution.h"
#endif

namespace DlssBackend
{
    using TemporalEffects::Mode;
    using namespace Graphics;

#if NYX_ENABLE_DLSS
    namespace
    {
        // Stable project identity: never regenerate per build, machine or session.
        constexpr char ProjectId[] = "d52dddeb-09c1-4c63-a180-fc5502fe715a";
        constexpr char EngineVersion[] = NYX_ENGINE_VERSION;

        NVSDK_NGX_PerfQuality_Value Quality(Mode mode)
        {
            switch (mode)
            {
            case Mode::DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
            case Mode::DLSSBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
            case Mode::DLSSPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case Mode::DLSSUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            }
        }

        std::string Error(const char* operation, NVSDK_NGX_Result result)
        {
            char code[32];
            sprintf_s(code, " (NGX 0x%08X)", static_cast<unsigned int>(result));
            return std::string(operation) + code;
        }
    }
#endif

    struct Context::Impl
    {
        Status status;
        bool attempted = false;
        bool initialized = false;
        bool reset = true;
        Mode mode = Mode::Off;
        FrameResolution resolution = {};
        uint64_t lastFence = 0;
#if NYX_ENABLE_DLSS
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* capabilities = nullptr;
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        std::wstring runtimeDirectory;
        std::wstring dataDirectory;
        const wchar_t* searchPaths[1] = {};
        NVSDK_NGX_FeatureCommonInfo common = {};
#endif
    };

    Context::Context() : m_Impl(std::make_unique<Impl>()) {}
    Context::~Context() { Shutdown(); }
    const Status& Context::GetStatus() const { return m_Impl->status; }

    bool Context::Initialize()
    {
        auto& s = *m_Impl;
        if (s.attempted)
            return s.status.available;
        s.attempted = true;
#if !NYX_ENABLE_DLSS
        s.status.reason = "DLSS support is disabled in this build.";
        return false;
#else
        s.device = g_Device;
        if (!s.device)
        {
            s.status.reason = "No D3D12 device is available.";
            return false;
        }
        // Check the active render device, including on systems with multiple adapters.
        // NVIDIA hardware still needs the NGX capability and driver checks below.
        if (!IsDeviceNvidia(s.device))
        {
            s.status.reason = "DLSS requires an NVIDIA GPU; the active render device is not NVIDIA.";
            return false;
        }
        wchar_t executable[32768];
        const DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
        if (!length || length == _countof(executable))
        {
            s.status.reason = "Cannot locate the executable directory.";
            return false;
        }
        const auto directory = std::filesystem::path(executable).parent_path();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(directory / L"nvngx_dlss.dll", ec))
        {
            s.status.reason = "nvngx_dlss.dll is missing from the executable directory.";
            return false;
        }
        s.runtimeDirectory = directory.wstring();
        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)))
        {
            s.status.reason = "Cannot locate the user-writable NGX cache directory.";
            return false;
        }
        const auto dataPath = std::filesystem::path(localAppData) / L"Nyx" / L"NGX";
        CoTaskMemFree(localAppData);
        std::filesystem::create_directories(dataPath, ec);
        if (ec)
        {
            s.status.reason = "Cannot create the user-writable NGX cache directory.";
            return false;
        }
        s.dataDirectory = dataPath.wstring();
        s.searchPaths[0] = s.runtimeDirectory.c_str();
        s.common.PathListInfo = {s.searchPaths, 1};
        s.common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            EngineVersion, s.dataDirectory.c_str(), s.device, &s.common);
        if (NVSDK_NGX_FAILED(result))
        {
            s.status.reason = Error("NGX initialization failed", result);
            return false;
        }
        s.initialized = true;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.capabilities);
        if (NVSDK_NGX_FAILED(result) || !s.capabilities)
        {
            s.status.reason = Error("NGX capability query failed", result);
            return false;
        }
        int needsDriver = 0, available = 0;
        s.capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        s.capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &s.status.minimumDriverMajor);
        s.capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &s.status.minimumDriverMinor);
        if (needsDriver)
        {
            s.status.reason = "DLSS requires NVIDIA driver " + std::to_string(s.status.minimumDriverMajor) +
                "." + std::to_string(s.status.minimumDriverMinor) + " or newer.";
            return false;
        }
        result = s.capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (NVSDK_NGX_FAILED(result) || !available)
        {
            int initResult = 0;
            s.capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            s.status.reason = Error("DLSS is unavailable on this device/driver",
                static_cast<NVSDK_NGX_Result>(initResult));
            return false;
        }
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(s.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        {
            s.status.reason = "Cannot identify the active D3D12 adapter for NGX.";
            return false;
        }
        NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
        discovery.SDKVersion = NVSDK_NGX_Version_API;
        discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
        discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion};
        discovery.ApplicationDataPath = s.dataDirectory.c_str();
        discovery.FeatureInfo = &s.common;
        NVSDK_NGX_FeatureRequirement requirements = {};
        result = NVSDK_NGX_D3D12_GetFeatureRequirements(adapter.Get(), &discovery, &requirements);
        if (NVSDK_NGX_FAILED(result) || requirements.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported)
        {
            s.status.reason = Error("DLSS device requirements query failed", result) +
                "; support flags " + std::to_string(static_cast<unsigned int>(requirements.FeatureSupported));
            return false;
        }
        s.status.available = true;
        s.status.reason.clear();
        return true;
#endif
    }

    PixelSize Context::QueryRenderSize(Mode mode, PixelSize output)
    {
        if (mode < Mode::DLAA || mode > Mode::DLSSUltraPerformance ||
            output.width < 32 || output.height < 32 || !Initialize())
            return {};
#if NYX_ENABLE_DLSS
        const PixelSize size = QueryOptimalRenderSize(m_Impl->capabilities, Quality(mode), output);
        return !size.IsValid() || (mode == Mode::DLAA && size != output) ? PixelSize{} : size;
#else
        return {};
#endif
    }

    void Context::ReleaseFeature()
    {
        auto& s = *m_Impl;
#if NYX_ENABLE_DLSS
        // Only lifetime transitions wait. Steady-state Evaluate never waits on the CPU.
        if (s.lastFence)
            g_CommandManager.WaitForFence(s.lastFence);
        if (s.feature)
            NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
        if (s.parameters)
            NVSDK_NGX_D3D12_DestroyParameters(s.parameters);
        s.feature = nullptr;
        s.parameters = nullptr;
#endif
        s.lastFence = 0;
        s.reset = true;
        s.mode = Mode::Off;
        s.resolution = {};
    }

    bool Context::Configure(Mode mode, const FrameResolution& resolution)
    {
        auto& s = *m_Impl;
        if (!Initialize())
            return false;
#if NYX_ENABLE_DLSS
        if (s.feature && s.mode == mode && s.resolution.renderSize == resolution.renderSize &&
            s.resolution.outputSize == resolution.outputSize)
            return true;
        ReleaseFeature();
        if (mode < Mode::DLAA || !resolution.renderSize.IsValid() ||
            QueryRenderSize(mode, resolution.outputSize) != resolution.renderSize)
        {
            s.status.reason = "The requested DLSS mode or render size is unsupported.";
            return false;
        }
        auto result = NVSDK_NGX_D3D12_AllocateParameters(&s.parameters);
        if (NVSDK_NGX_FAILED(result))
        {
            s.status.reason = Error("NGX parameter allocation failed", result);
            return false;
        }
        // NGX otherwise caches allocations after ReleaseFeature, including across
        // quality/preset changes. Return that memory when replacing our sole feature.
        s.parameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        NVSDK_NGX_DLSS_Create_Params create = {};
        create.Feature.InWidth = resolution.renderSize.width;
        create.Feature.InHeight = resolution.renderSize.height;
        create.Feature.InTargetWidth = resolution.outputSize.width;
        create.Feature.InTargetHeight = resolution.outputSize.height;
        create.Feature.InPerfQualityValue = Quality(mode);
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        auto& commands = GraphicsContext::Begin(L"DLSS Create");
        result = NGX_D3D12_CREATE_DLSS_EXT(commands.GetCommandList(), 1, 1, &s.feature, s.parameters, &create);
        s.lastFence = commands.Finish();
        if (NVSDK_NGX_FAILED(result) || !s.feature)
        {
            s.status.reason = Error("DLSS feature creation failed", result);
            ReleaseFeature();
            return false;
        }
        s.mode = mode;
        s.resolution = resolution;
        s.reset = true;
        ++s.status.featureGeneration;
        s.status.reason.clear();
        return true;
#else
        (void)mode; (void)resolution; (void)s;
        return false;
#endif
    }

    bool Context::Evaluate(ColorBuffer& color, DepthBuffer& depth, ColorBuffer& motion,
        ColorBuffer& exposure, ColorBuffer& output,
        const TemporalEffects::TemporalFrameState& frame, float deltaSeconds)
    {
        auto& s = *m_Impl;
#if NYX_ENABLE_DLSS
        const auto matches = [](const PixelBuffer& buffer, PixelSize size)
        {
            return buffer.GetResource() && buffer.GetWidth() == size.width && buffer.GetHeight() == size.height;
        };
        if (!s.feature || frame.mode != s.mode ||
            frame.resolution.renderSize != s.resolution.renderSize || frame.resolution.outputSize != s.resolution.outputSize ||
            !matches(color, s.resolution.renderSize) || !matches(depth, s.resolution.renderSize) ||
            !matches(motion, s.resolution.renderSize) || !matches(output, s.resolution.outputSize) ||
            !matches(exposure, {1, 1}) || color.GetResource() == output.GetResource() ||
            color.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT || motion.GetFormat() != DXGI_FORMAT_R16G16_FLOAT ||
            depth.GetFormat() != DXGI_FORMAT_D32_FLOAT ||
            exposure.GetFormat() != DXGI_FORMAT_R32_FLOAT || output.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            !std::isfinite(frame.jitter.x) || !std::isfinite(frame.jitter.y))
        {
            s.status.reason = "DLSS inputs do not match the configured feature.";
            s.reset = true;
            return false;
        }
        NVSDK_NGX_D3D12_DLSS_Eval_Params evaluate = {};
        evaluate.Feature.pInColor = color.GetResource();
        evaluate.Feature.pInOutput = output.GetResource();
        evaluate.pInDepth = depth.GetResource();
        evaluate.pInMotionVectors = motion.GetResource();
        evaluate.pInExposureTexture = exposure.GetResource();
        evaluate.InJitterOffsetX = frame.jitter.x;
        evaluate.InJitterOffsetY = frame.jitter.y;
        evaluate.InRenderSubrectDimensions = {s.resolution.renderSize.width, s.resolution.renderSize.height};
        evaluate.InReset = s.reset || !frame.historyValid;
        evaluate.InMVScaleX = evaluate.InMVScaleY = 1.0f;
        evaluate.InPreExposure = evaluate.InExposureScale = 1.0f;
        evaluate.InFrameTimeDeltaInMsec = std::isfinite(deltaSeconds) ? std::max(0.0f, deltaSeconds * 1000.0f) : 0.0f;
        auto& commands = GraphicsContext::Begin(L"DLSS Evaluate");
        commands.TransitionResource(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands.TransitionResource(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands.TransitionResource(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands.TransitionResource(exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands.TransitionResource(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands.FlushResourceBarriers();
        const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(commands.GetCommandList(), s.feature, s.parameters, &evaluate);
        // NGX changes descriptor heaps/root state. Never reuse this recording for engine work.
        s.lastFence = commands.Finish();
        if (NVSDK_NGX_FAILED(result))
        {
            s.status.reason = Error("DLSS evaluation failed", result);
            s.reset = true;
            return false;
        }
        s.reset = false;
        ++s.status.evaluationCount;
        return true;
#else
        (void)color; (void)depth; (void)motion; (void)exposure; (void)output; (void)frame; (void)deltaSeconds;
        s.status.reason = "DLSS support is disabled in this build.";
        return false;
#endif
    }

    void Context::Shutdown()
    {
        auto& s = *m_Impl;
        ReleaseFeature();
#if NYX_ENABLE_DLSS
        if (s.capabilities)
            NVSDK_NGX_D3D12_DestroyParameters(s.capabilities);
        if (s.initialized)
            NVSDK_NGX_D3D12_Shutdown1(s.device);
#endif
        s = Impl{};
    }
}

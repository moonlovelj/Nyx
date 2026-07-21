#include "pch.h"
#include "RenderGraph.h"
#include "RenderGraphPrivate.h"

#include <atomic>
#include <utility>

namespace RenderGraph
{
    namespace
    {
        std::atomic<uint64_t> g_NextGraphEpoch{1};

        uint64_t AcquireGraphEpoch()
        {
            uint64_t epoch = g_NextGraphEpoch.fetch_add(1, std::memory_order_relaxed);
            if (epoch == 0)
                epoch = g_NextGraphEpoch.fetch_add(1, std::memory_order_relaxed);
            return epoch;
        }

        const char* ResourceTypeName(ResourceKind kind, bool imported)
        {
            if (kind == ResourceKind::Texture)
                return imported ? "Imported texture" : "Texture";
            return imported ? "Imported buffer" : "Buffer";
        }

        template <typename ImplType, typename Desc>
        std::optional<ResourceId> RegisterResource(
            ImplType& graph,
            const char* operation,
            std::string_view name,
            const Desc& desc,
            bool imported,
            void* externalResource,
            Detail::ExternalResourceType externalType,
            Usage initialUsage,
            ShaderStage initialStages)
        {
            if (graph.RejectMutationDuringExecution(operation))
                return std::nullopt;

            return graph.RegisterResource(
                name,
                Detail::MakeDescriptor(desc),
                imported,
                externalResource,
                externalType,
                initialUsage,
                initialStages);
        }
    } // namespace

    Graph::Impl::Impl(std::string graphName)
        : Name(std::move(graphName)), Epoch(AcquireGraphEpoch())
    {
    }

    void Graph::Impl::InvalidateCompiledPlan()
    {
        Compilation.Invalidate();
        RuntimeDiagnostics.clear();
    }

    bool Graph::Impl::RejectMutationDuringExecution(const char* operation)
    {
        if (!Executing)
            return false;

        Detail::AddDiagnostic(
            RuntimeDiagnostics,
            DiagnosticCode::MutationDuringExecution,
            std::string(operation) + " is not allowed while the render graph is executing.");
        return true;
    }

    Detail::PassDef* Graph::Impl::GetOpenPass(
        PassId pass,
        uint64_t builderEpoch,
        const char* operation)
    {
        if (RejectMutationDuringExecution(operation))
            return nullptr;

        if (builderEpoch != Epoch)
        {
            Detail::AddDiagnostic(
                BuildDiagnostics,
                DiagnosticCode::InvalidBuilderEpoch,
                "A pass builder belongs to an expired graph epoch.",
                pass);
            return nullptr;
        }

        if (pass >= Definition.Passes.size() || !Definition.Passes[pass].BuilderOpen)
        {
            Detail::AddDiagnostic(
                BuildDiagnostics,
                DiagnosticCode::BuilderClosed,
                "The pass builder is already closed.",
                pass);
            return nullptr;
        }

        return &Definition.Passes[pass];
    }

    std::optional<ResourceId> Graph::Impl::RegisterResource(
        std::string_view name,
        const Detail::ResourceDescriptor& descriptor,
        bool imported,
        void* externalResource,
        Detail::ExternalResourceType externalType,
        Usage initialUsage,
        ShaderStage initialStages)
    {
        InvalidateCompiledPlan();

        const char* typeName = ResourceTypeName(descriptor.Kind, imported);
        if (!Detail::IsValidDescriptor(descriptor))
        {
            Detail::AddDiagnostic(
                BuildDiagnostics,
                DiagnosticCode::InvalidResourceDesc,
                std::string(typeName) + " '" + std::string(name) +
                    "' has an invalid descriptor.");
            return std::nullopt;
        }

        if (imported &&
            (initialUsage == Usage::Undefined ||
             !Detail::IsUsageValidForKind(descriptor, initialUsage)))
        {
            Detail::AddDiagnostic(
                BuildDiagnostics,
                DiagnosticCode::InvalidUsage,
                std::string(typeName) + " '" + std::string(name) +
                    "' has an invalid initial usage.");
            return std::nullopt;
        }
        if (imported &&
            ((!Detail::IsShaderUsage(initialUsage) && initialStages != ShaderStage::None) ||
             (static_cast<uint32_t>(initialStages) &
              ~static_cast<uint32_t>(ShaderStage::All)) != 0))
        {
            Detail::AddDiagnostic(
                BuildDiagnostics,
                DiagnosticCode::InvalidUsage,
                std::string(typeName) + " '" + std::string(name) +
                    "' has invalid initial shader stages.");
            return std::nullopt;
        }

        Detail::ResourceDef resource;
        resource.Name = std::string(name);
        resource.Desc = descriptor;
        resource.Imported = imported;
        resource.ExternalResource = externalResource;
        resource.ExternalType = externalType;
        if (imported)
            resource.InitialState =
                Detail::MakeCanonicalResourceState(initialUsage, initialStages);

        resource.Versions.push_back({});

        const ResourceId id = static_cast<ResourceId>(Definition.Resources.size());
        Definition.Resources.push_back(std::move(resource));
        return id;
    }

    void Graph::Impl::ResetFrame()
    {
        Definition.Clear();
        Compilation.ResetForNewDefinition();
        BuildDiagnostics.clear();
        RuntimeDiagnostics.clear();
        BuilderToken = std::make_shared<uint8_t>();
        OpenBuilderCount = 0;
        Epoch = AcquireGraphEpoch();
    }

    Graph::Graph(std::string name)
        : m_Impl(std::make_unique<Impl>(std::move(name)))
    {
    }

    Graph::~Graph() = default;

    TextureHandle Graph::CreateTexture(std::string_view name, const TextureDesc& desc)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "CreateTexture",
            name,
            desc,
            false,
            nullptr,
            Detail::ExternalResourceType::None,
            Usage::Undefined,
            ShaderStage::None);
        return id ? TextureHandle(*id, 0, m_Impl->Epoch) : TextureHandle{};
    }

    BufferHandle Graph::CreateBuffer(std::string_view name, const BufferDesc& desc)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "CreateBuffer",
            name,
            desc,
            false,
            nullptr,
            Detail::ExternalResourceType::None,
            Usage::Undefined,
            ShaderStage::None);
        return id ? BufferHandle(*id, 0, m_Impl->Epoch) : BufferHandle{};
    }

    TextureHandle Graph::ImportTexture(
        std::string_view name,
        const TextureDesc& desc,
        void* externalResource,
        Usage initialUsage,
        ShaderStage initialStages)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "ImportTexture",
            name,
            desc,
            true,
            externalResource,
            Detail::ExternalResourceType::Opaque,
            initialUsage,
            initialStages);
        return id ? TextureHandle(*id, 0, m_Impl->Epoch) : TextureHandle{};
    }

    TextureHandle Graph::ImportTexture(
        std::string_view name,
        const TextureDesc& desc,
        GpuResource& externalResource,
        Usage initialUsage,
        ShaderStage initialStages)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "ImportTexture",
            name,
            desc,
            true,
            &externalResource,
            Detail::ExternalResourceType::D3D12GpuResource,
            initialUsage,
            initialStages);
        return id ? TextureHandle(*id, 0, m_Impl->Epoch) : TextureHandle{};
    }

    BufferHandle Graph::ImportBuffer(
        std::string_view name,
        const BufferDesc& desc,
        void* externalResource,
        Usage initialUsage,
        ShaderStage initialStages)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "ImportBuffer",
            name,
            desc,
            true,
            externalResource,
            Detail::ExternalResourceType::Opaque,
            initialUsage,
            initialStages);
        return id ? BufferHandle(*id, 0, m_Impl->Epoch) : BufferHandle{};
    }

    BufferHandle Graph::ImportBuffer(
        std::string_view name,
        const BufferDesc& desc,
        GpuResource& externalResource,
        Usage initialUsage,
        ShaderStage initialStages)
    {
        const auto id = RegisterResource(
            *m_Impl,
            "ImportBuffer",
            name,
            desc,
            true,
            &externalResource,
            Detail::ExternalResourceType::D3D12GpuResource,
            initialUsage,
            initialStages);
        return id ? BufferHandle(*id, 0, m_Impl->Epoch) : BufferHandle{};
    }

    PassBuilder Graph::BeginPass(std::string_view name, PassFlags flags)
    {
        if (m_Impl->RejectMutationDuringExecution("AddPass"))
            return {};

        m_Impl->InvalidateCompiledPlan();
        const PassId id = static_cast<PassId>(m_Impl->Definition.Passes.size());
        Detail::PassDef pass;
        pass.Name = std::string(name);
        pass.Flags = flags;
        m_Impl->Definition.Passes.push_back(std::move(pass));
        ++m_Impl->OpenBuilderCount;
        return PassBuilder(*this, id);
    }

    Graph::RawHandle Graph::AccessInternal(
        PassId passId,
        uint64_t builderEpoch,
        RawHandle handle,
        AccessMode mode,
        Usage usage,
        ShaderStage stages)
    {
        Detail::PassDef* pass = m_Impl->GetOpenPass(
            passId,
            builderEpoch,
            "PassBuilder::Access");
        if (pass == nullptr)
            return {};

        m_Impl->InvalidateCompiledPlan();
        auto fail = [&](DiagnosticCode code, std::string message, ResourceId resource = kInvalidIndex)
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                code,
                std::move(message),
                passId,
                resource);
            return RawHandle{};
        };

        if (handle.Resource == kInvalidIndex || handle.Epoch == 0)
            return fail(DiagnosticCode::InvalidHandle, "A pass used an invalid resource handle.");
        if (handle.Epoch != m_Impl->Epoch)
        {
            return fail(
                DiagnosticCode::InvalidHandleEpoch,
                "A pass used a handle from another graph epoch.");
        }
        if (handle.Resource >= m_Impl->Definition.Resources.size())
            return fail(DiagnosticCode::InvalidHandle, "A pass used an out-of-range resource handle.");

        Detail::ResourceDef& resource = m_Impl->Definition.Resources[handle.Resource];
        if (resource.Desc.Kind != handle.Kind)
        {
            return fail(
                DiagnosticCode::ResourceKindMismatch,
                "A typed resource handle does not match the underlying resource kind.",
                handle.Resource);
        }
        if (handle.Version >= resource.Versions.size())
        {
            return fail(
                DiagnosticCode::InvalidHandle,
                "A pass used an out-of-range resource version.",
                handle.Resource);
        }
        if (handle.Version != resource.GetLatestVersion())
        {
            return fail(
                DiagnosticCode::StaleVersion,
                "A pass used a stale resource version.",
                handle.Resource);
        }
        if (!Detail::IsAccessUsageValid(resource.Desc, mode, usage))
        {
            return fail(
                DiagnosticCode::InvalidUsage,
                "Resource '" + resource.Name + "' was declared with an incompatible usage.",
                handle.Resource);
        }
        if (!Detail::IsShaderUsage(usage) && stages != ShaderStage::None)
        {
            return fail(
                DiagnosticCode::InvalidUsage,
                "Shader stages are only valid for shader-resource and unordered-access usages.",
                handle.Resource);
        }
        if (Detail::IsShaderUsage(usage) &&
            (static_cast<uint32_t>(stages) &
             ~static_cast<uint32_t>(ShaderStage::All)) != 0)
        {
            return fail(
                DiagnosticCode::InvalidUsage,
                "A shader resource declaration contains an unknown shader stage.",
                handle.Resource);
        }

        const ResourceState state = Detail::MakeCanonicalResourceState(usage, stages);
        auto existing = std::find_if(
            pass->Accesses.begin(),
            pass->Accesses.end(),
            [&](const Detail::AccessRecord& access)
            {
                return access.Resource == handle.Resource;
            });
        if (existing != pass->Accesses.end())
        {
            if (existing->Mode == AccessMode::Read && mode == AccessMode::Read &&
                existing->State.UsageType == state.UsageType)
            {
                existing->State.Stages = existing->State.Stages | state.Stages;
                return handle;
            }

            return fail(
                DiagnosticCode::ConflictingUsage,
                "Resource '" + resource.Name +
                    "' has multiple conflicting declarations in one pass.",
                handle.Resource);
        }

        Detail::VersionDef& inputVersion = resource.Versions[handle.Version];
        if (Detail::Reads(mode) && !resource.IsInitialized(handle.Version))
        {
            return fail(
                DiagnosticCode::UninitializedRead,
                "Transient resource '" + resource.Name +
                    "' was read before its first write.",
                handle.Resource);
        }

        Detail::AccessRecord access;
        access.Resource = handle.Resource;
        access.InputVersion = handle.Version;
        access.OutputVersion = handle.Version;
        access.Mode = mode;
        access.State = state;

        if (Detail::Reads(mode) &&
            std::find(inputVersion.Readers.begin(), inputVersion.Readers.end(), passId) ==
                inputVersion.Readers.end())
        {
            inputVersion.Readers.push_back(passId);
        }

        if (Detail::Writes(mode))
        {
            access.OutputVersion = static_cast<uint32_t>(resource.Versions.size());
            Detail::VersionDef outputVersion;
            outputVersion.Producer = passId;
            resource.Versions.push_back(std::move(outputVersion));
        }

        pass->Accesses.push_back(access);
        return {handle.Kind, handle.Resource, access.OutputVersion, m_Impl->Epoch};
    }

    void Graph::MarkSideEffectInternal(PassId passId, uint64_t builderEpoch)
    {
        Detail::PassDef* pass = m_Impl->GetOpenPass(
            passId,
            builderEpoch,
            "PassBuilder::MarkSideEffect");
        if (pass == nullptr)
            return;

        m_Impl->InvalidateCompiledPlan();
        pass->Flags = pass->Flags | PassFlags::SideEffect;
    }

    void Graph::SetExecuteInternal(
        PassId passId,
        uint64_t builderEpoch,
        PassBuilder::ExecuteCallback callback)
    {
        Detail::PassDef* pass = m_Impl->GetOpenPass(
            passId,
            builderEpoch,
            "PassBuilder::SetExecute");
        if (pass == nullptr)
            return;

        m_Impl->InvalidateCompiledPlan();
        if (pass->Execute)
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::DuplicateExecuteCallback,
                "A pass cannot have more than one execute callback.",
                passId);
            return;
        }

        pass->Execute = std::move(callback);
    }

    void Graph::CloseBuilderInternal(PassId passId, uint64_t builderEpoch)
    {
        if (builderEpoch != m_Impl->Epoch || passId >= m_Impl->Definition.Passes.size())
            return;

        Detail::PassDef& pass = m_Impl->Definition.Passes[passId];
        if (!pass.BuilderOpen)
            return;

        pass.BuilderOpen = false;
        if (m_Impl->OpenBuilderCount > 0)
            --m_Impl->OpenBuilderCount;
    }

    void Graph::ExportInternal(
        ResourceKind kind,
        ResourceId resourceId,
        uint32_t version,
        uint64_t epoch,
        Usage finalUsage)
    {
        if (m_Impl->RejectMutationDuringExecution("Export"))
            return;

        m_Impl->InvalidateCompiledPlan();
        if (resourceId == kInvalidIndex || epoch == 0)
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::InvalidHandle,
                "Export used an invalid handle.");
            return;
        }
        if (epoch != m_Impl->Epoch)
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::InvalidHandleEpoch,
                "Export used a handle from another graph epoch.");
            return;
        }
        if (resourceId >= m_Impl->Definition.Resources.size())
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::InvalidHandle,
                "Export used an out-of-range handle.");
            return;
        }

        Detail::ResourceDef& resource = m_Impl->Definition.Resources[resourceId];
        if (resource.Desc.Kind != kind || version >= resource.Versions.size())
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::InvalidExport,
                "Export used an invalid resource version.",
                kInvalidIndex,
                resourceId);
            return;
        }
        if (finalUsage != Usage::Undefined &&
            !Detail::IsUsageValidForKind(resource.Desc, finalUsage))
        {
            Detail::AddDiagnostic(
                m_Impl->BuildDiagnostics,
                DiagnosticCode::InvalidUsage,
                "Export requested an invalid final usage for resource '" + resource.Name + "'.",
                kInvalidIndex,
                resourceId);
            return;
        }

        auto existing = std::find_if(
            m_Impl->Definition.Exports.begin(),
            m_Impl->Definition.Exports.end(),
            [&](const Detail::ExportDef& record)
            {
                return record.Resource == resourceId && record.Version == version;
            });
        if (existing != m_Impl->Definition.Exports.end())
        {
            if (existing->FinalUsage != finalUsage)
            {
                Detail::AddDiagnostic(
                    m_Impl->BuildDiagnostics,
                    DiagnosticCode::ConflictingFinalUsage,
                    "Resource '" + resource.Name +
                        "' was exported with conflicting final usages.",
                    kInvalidIndex,
                    resourceId);
            }
            return;
        }

        m_Impl->Definition.Exports.push_back({resourceId, version, finalUsage});
    }

    CompileResult Graph::Compile(const CompileOptions& options)
    {
        if (m_Impl->RejectMutationDuringExecution("Compile"))
            return {};

        m_Impl->RuntimeDiagnostics.clear();
        Detail::CompileGraph(
            m_Impl->Definition,
            m_Impl->OpenBuilderCount,
            m_Impl->BuildDiagnostics,
            options,
            m_Impl->Compilation);
        return m_Impl->Compilation.Result;
    }

    void Graph::Reset()
    {
        if (m_Impl->RejectMutationDuringExecution("Reset"))
            return;
        m_Impl->ResetFrame();
    }

    bool Graph::IsCompiled() const
    {
        return m_Impl->Compilation.Result.Succeeded;
    }

    uint64_t Graph::GetEpoch() const
    {
        return m_Impl->Epoch;
    }

    std::string_view Graph::GetName() const
    {
        return m_Impl->Name;
    }

    const CompileResult& Graph::GetCompileResult() const
    {
        return m_Impl->Compilation.Result;
    }

    const std::vector<PassId>& Graph::GetExecutionOrder() const
    {
        return m_Impl->Compilation.Plan.ExecutionOrder;
    }

    const std::vector<Barrier>& Graph::GetBarriers() const
    {
        return m_Impl->Compilation.Plan.Barriers;
    }

    std::vector<Diagnostic> Graph::CollectDiagnostics() const
    {
        std::vector<Diagnostic> diagnostics;
        diagnostics.reserve(
            m_Impl->BuildDiagnostics.size() + m_Impl->Compilation.Diagnostics.size() +
            m_Impl->RuntimeDiagnostics.size());
        diagnostics.insert(
            diagnostics.end(),
            m_Impl->BuildDiagnostics.begin(),
            m_Impl->BuildDiagnostics.end());
        diagnostics.insert(
            diagnostics.end(),
            m_Impl->Compilation.Diagnostics.begin(),
            m_Impl->Compilation.Diagnostics.end());
        diagnostics.insert(
            diagnostics.end(),
            m_Impl->RuntimeDiagnostics.begin(),
            m_Impl->RuntimeDiagnostics.end());
        return diagnostics;
    }

    std::string_view Graph::GetPassName(PassId pass) const
    {
        return pass < m_Impl->Definition.Passes.size()
                   ? std::string_view(m_Impl->Definition.Passes[pass].Name)
                   : std::string_view();
    }

    bool Graph::IsPassLive(PassId pass) const
    {
        return pass < m_Impl->Compilation.Plan.Passes.size() &&
               m_Impl->Compilation.Plan.Passes[pass].Live;
    }

    bool Graph::IsDeclaredInternal(PassId passId, RawHandle handle) const
    {
        if (handle.Epoch != m_Impl->Epoch || passId >= m_Impl->Definition.Passes.size() ||
            handle.Resource >= m_Impl->Definition.Resources.size() ||
            m_Impl->Definition.Resources[handle.Resource].Desc.Kind != handle.Kind)
        {
            return false;
        }

        const Detail::PassDef& pass = m_Impl->Definition.Passes[passId];
        return std::any_of(
            pass.Accesses.begin(),
            pass.Accesses.end(),
            [&](const Detail::AccessRecord& access)
            {
                if (access.Resource != handle.Resource)
                    return false;
                if (access.Mode == AccessMode::Read)
                    return handle.Version == access.InputVersion;
                if (access.Mode == AccessMode::Write)
                    return handle.Version == access.OutputVersion;
                return handle.Version == access.InputVersion ||
                       handle.Version == access.OutputVersion;
            });
    }

    void* Graph::GetResourceInternal(PassId pass, RawHandle handle)
    {
        if (!IsDeclaredInternal(pass, handle))
        {
            Detail::AddDiagnostic(
                m_Impl->RuntimeDiagnostics,
                DiagnosticCode::UndeclaredResourceAccess,
                "A pass requested an external resource that it did not declare.",
                pass,
                handle.Resource < m_Impl->Definition.Resources.size()
                    ? handle.Resource
                    : kInvalidIndex);
            return nullptr;
        }

        const Detail::ResourceDef& resource = m_Impl->Definition.Resources[handle.Resource];
        if (m_Impl->Executing && handle.Resource < m_Impl->RuntimeResources.size())
            return m_Impl->RuntimeResources[handle.Resource];
        return resource.Imported ? resource.ExternalResource : nullptr;
    }

} // namespace RenderGraph

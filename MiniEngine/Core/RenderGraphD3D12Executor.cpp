#include "pch.h"
#include "CommandContext.h"
#include "GpuResource.h"
#include "RenderGraphD3D12.h"
#include "RenderGraphD3D12Allocator.h"
#include "RenderGraphInternal.h"
#include "RenderGraphPrivate.h"

namespace RenderGraph
{
    namespace
    {
        D3D12_RESOURCE_FLAGS GetRequiredD3D12Flags(ResourceFlags flags)
        {
            uint32_t nativeFlags = D3D12_RESOURCE_FLAG_NONE;
            if (HasAny(flags, ResourceFlags::AllowRenderTarget))
                nativeFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (HasAny(flags, ResourceFlags::AllowDepthStencil))
                nativeFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (HasAny(flags, ResourceFlags::AllowUnorderedAccess))
                nativeFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return static_cast<D3D12_RESOURCE_FLAGS>(nativeFlags);
        }

        bool MatchesDescriptor(
            const Detail::ResourceDescriptor& graphDesc,
            uint32_t requiredUsageMask,
            const D3D12_RESOURCE_DESC& nativeDesc,
            std::string& message)
        {
            auto uses = [&](Usage usage)
            {
                return (requiredUsageMask & (1u << static_cast<uint32_t>(usage))) != 0;
            };

            const ResourceFlags graphFlags = graphDesc.Kind == ResourceKind::Texture
                                                 ? graphDesc.Texture.Flags
                                                 : graphDesc.Buffer.Flags;
            uint32_t requiredFlags = static_cast<uint32_t>(GetRequiredD3D12Flags(graphFlags));
            if (uses(Usage::RenderTarget))
                requiredFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (uses(Usage::DepthRead) || uses(Usage::DepthWrite))
                requiredFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (uses(Usage::UnorderedAccess))
                requiredFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if ((static_cast<uint32_t>(nativeDesc.Flags) & requiredFlags) != requiredFlags)
            {
                message = "The native resource is missing flags required by its RenderGraph descriptor.";
                return false;
            }
            if (uses(Usage::ShaderResource) &&
                (nativeDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
            {
                message = "The graph uses a ShaderResource view, but the native resource denies shader-resource access.";
                return false;
            }

            if (graphDesc.Kind == ResourceKind::Buffer)
            {
                if (nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
                {
                    message = "A logical buffer was imported from a native texture resource.";
                    return false;
                }
                if (nativeDesc.Width < graphDesc.Buffer.SizeInBytes)
                {
                    message = "The native buffer is smaller than its RenderGraph descriptor.";
                    return false;
                }
                return true;
            }

            if (nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            {
                message = "RenderGraph v1 logical textures require a native Texture2D or Texture2D array.";
                return false;
            }

            const TextureDesc& texture = graphDesc.Texture;
            if (nativeDesc.Width != texture.Width ||
                nativeDesc.Height != texture.Height ||
                nativeDesc.DepthOrArraySize != texture.DepthOrArraySize ||
                nativeDesc.MipLevels != texture.MipLevels ||
                nativeDesc.SampleDesc.Count != texture.SampleCount)
            {
                message = "The native texture dimensions, mip count, or sample count do not match its RenderGraph descriptor.";
                return false;
            }

            // Format is intentionally not compared: a typeless native texture can
            // legitimately be imported with a typed view format in the graph.
            return true;
        }

        class D3D12ExecutionBackend final : public Detail::ExecutionBackend
        {
          public:
            D3D12ExecutionBackend(
                CommandContext& commandContext,
                const Detail::GraphDefinition& definition,
                const Detail::CompiledPlan& plan)
                : m_CommandContext(commandContext),
                  m_Allocator(Detail::GetD3D12TransientAllocator())
            {
                if (m_CommandContext.GetCommandListType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    m_ValidationCode = DiagnosticCode::UnsupportedCommandContext;
                    m_ValidationMessage = "Render Graph execution requires a direct CommandContext; async compute and copy queues are not modeled yet.";
                    return;
                }

                if (!m_Allocator.Prepare(definition, plan, m_ValidationMessage))
                {
                    m_ValidationCode = DiagnosticCode::UnresolvedRuntimeResource;
                    return;
                }
                m_Prepared = true;
            }

            ~D3D12ExecutionBackend() override
            {
                if (m_Prepared)
                    m_Allocator.Retire();
            }

            DiagnosticCode ValidateExecution(std::string& message) const override
            {
                message = m_ValidationMessage;
                return m_ValidationCode;
            }

            DiagnosticCode ResolveResource(
                const Detail::ResourceResolveRequest& request,
                Detail::ResourceResolveResult& result,
                std::string& message) override
            {
                result = {};
                if (!request.Imported)
                {
                    GpuResource* resource = m_Allocator.GetResource(request.LogicalResourceId);
                    if (resource == nullptr || resource->GetResource() == nullptr)
                    {
                        message = "Memory slot " + std::to_string(request.MemorySlot) +
                                  " did not resolve to a native resource.";
                        return DiagnosticCode::UnresolvedRuntimeResource;
                    }
                    if (!MatchesDescriptor(
                            request.Descriptor,
                            request.RequiredUsageMask,
                            resource->GetResource()->GetDesc(),
                            message))
                    {
                        return DiagnosticCode::RuntimeResourceDescMismatch;
                    }
                    result.RuntimeResource = resource;
                    result.NativeResourceIdentity = resource->GetResource();
                    return DiagnosticCode::None;
                }
                if (request.ExternalResource == nullptr)
                {
                    message = "The imported resource pointer is null.";
                    return DiagnosticCode::UnresolvedRuntimeResource;
                }
                if (request.ExternalType != Detail::ExternalResourceType::D3D12GpuResource)
                {
                    message = "Render Graph execution requires the strongly typed GpuResource import overload.";
                    return DiagnosticCode::UnresolvedRuntimeResource;
                }
                auto* resource = static_cast<GpuResource*>(request.ExternalResource);
                ID3D12Resource* nativeResource = resource->GetResource();
                if (nativeResource == nullptr)
                {
                    message = "The imported GpuResource has no native ID3D12Resource.";
                    return DiagnosticCode::UnresolvedRuntimeResource;
                }
                if (!MatchesDescriptor(
                        request.Descriptor,
                        request.RequiredUsageMask,
                        nativeResource->GetDesc(),
                        message))
                    return DiagnosticCode::RuntimeResourceDescMismatch;
                if (resource->HasPendingTransition())
                {
                    message = "The imported GpuResource has an unfinished split transition.";
                    return DiagnosticCode::RuntimeResourceStateMismatch;
                }

                const auto declared = TryGetD3D12ResourceState(request.DeclaredInitialState);
                if (!declared)
                {
                    message = "The imported resource has no valid D3D12 initial state.";
                    return DiagnosticCode::RuntimeResourceStateMismatch;
                }
                if (!IsD3D12ResourceStateCompatible(resource->GetUsageState(), *declared))
                {
                    message = "The imported GpuResource state does not match its declared initial usage (actual=" +
                              std::to_string(static_cast<uint32_t>(resource->GetUsageState())) +
                              ", declared=" + std::to_string(static_cast<uint32_t>(*declared)) + ").";
                    return DiagnosticCode::RuntimeResourceStateMismatch;
                }

                result.RuntimeResource = resource;
                result.NativeResourceIdentity = nativeResource;
                return DiagnosticCode::None;
            }

            DiagnosticCode RecordBarrier(
                const Barrier& barrier,
                void* beforeResource,
                void* afterResource,
                std::string& message) override
            {
                auto* resource = static_cast<GpuResource*>(afterResource);
                if (resource == nullptr)
                {
                    message = "A barrier target did not resolve to a native resource.";
                    return DiagnosticCode::BarrierRecordingFailed;
                }
                switch (barrier.Kind)
                {
                case BarrierKind::Aliasing:
                    m_CommandContext.InsertAliasBarrier(
                        static_cast<GpuResource*>(beforeResource),
                        *resource);
                    return DiagnosticCode::None;
                case BarrierKind::Transition:
                case BarrierKind::FinalTransition:
                {
                    const auto state = TryGetD3D12ResourceState(barrier.After);
                    if (!state)
                    {
                        message = "A transition barrier has no valid D3D12 destination state.";
                        return DiagnosticCode::BarrierRecordingFailed;
                    }
                    m_CommandContext.TransitionResource(*resource, *state);
                    return DiagnosticCode::None;
                }
                case BarrierKind::UnorderedAccess:
                    // TransitionResource emits a UAV barrier when the tracked state
                    // already is UAV and retains CommandContext's 16-entry auto-flush.
                    // It also safely restores UAV if pass code changed the state.
                    m_CommandContext.TransitionResource(
                        *resource,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    return DiagnosticCode::None;
                }

                message = "The compiled plan contains an unknown barrier kind.";
                return DiagnosticCode::BarrierRecordingFailed;
            }

            void FlushBarrierBatch() override
            {
                m_CommandContext.FlushResourceBarriers();
            }

          private:
            CommandContext& m_CommandContext;
            Detail::D3D12TransientAllocator& m_Allocator;
            DiagnosticCode m_ValidationCode = DiagnosticCode::None;
            std::string m_ValidationMessage;
            bool m_Prepared = false;
        };
    } // namespace

    bool Graph::Execute(CommandContext& commandContext, void* userContext)
    {
        if (!IsCompiled() || m_Impl->Executing)
            return ExecuteInternal(userContext, &commandContext, nullptr);

        D3D12ExecutionBackend backend(
            commandContext,
            m_Impl->Definition,
            m_Impl->Compilation.Plan);
        return ExecuteInternal(userContext, &commandContext, &backend);
    }
} // namespace RenderGraph

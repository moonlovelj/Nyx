#include "pch.h"
#include "RenderGraphPrivate.h"

#include <unordered_map>

namespace RenderGraph
{
    namespace
    {
        class ExecutionGuard
        {
          public:
            ExecutionGuard(bool& executing, std::vector<void*>& runtimeResources)
                : m_Executing(executing), m_RuntimeResources(runtimeResources)
            {
                m_Executing = true;
            }

            ~ExecutionGuard()
            {
                m_Executing = false;
                m_RuntimeResources.clear();
            }

          private:
            bool& m_Executing;
            std::vector<void*>& m_RuntimeResources;
        };
    } // namespace

    bool Graph::ExecuteInternal(
        void* userContext,
        CommandContext* commandContext,
        Detail::ExecutionBackend* backend)
    {
        if (m_Impl->Executing)
        {
            Detail::AddDiagnostic(
                m_Impl->RuntimeDiagnostics,
                DiagnosticCode::MutationDuringExecution,
                "Recursive render graph execution is not allowed.");
            return false;
        }

        m_Impl->RuntimeDiagnostics.clear();
        if (!IsCompiled())
        {
            Detail::AddDiagnostic(
                m_Impl->RuntimeDiagnostics,
                DiagnosticCode::ExecuteBeforeCompile,
                "Render graph execution requires a successful compile first.");
            return false;
        }

        const Detail::GraphDefinition& definition = m_Impl->Definition;
        const Detail::CompiledPlan& plan = m_Impl->Compilation.Plan;
        std::vector<void*> resolvedResources;

        if (backend != nullptr)
        {
            resolvedResources.resize(definition.Resources.size(), nullptr);
            std::string executionMessage;
            const DiagnosticCode executionValidation =
                backend->ValidateExecution(executionMessage);
            if (executionValidation != DiagnosticCode::None)
            {
                Detail::AddDiagnostic(
                    m_Impl->RuntimeDiagnostics,
                    executionValidation,
                    std::move(executionMessage));
                return false;
            }

            std::vector<bool> requiredResources(definition.Resources.size(), false);
            std::vector<uint32_t> requiredUsageMasks(definition.Resources.size(), 0);
            auto requireUsage = [&](ResourceId resource, Usage usage)
            {
                requiredResources[resource] = true;
                if (usage != Usage::Undefined)
                    requiredUsageMasks[resource] |= 1u << static_cast<uint32_t>(usage);
            };

            for (PassId passId : plan.ExecutionOrder)
            {
                for (const Detail::AccessRecord& access : definition.Passes[passId].Accesses)
                    requireUsage(access.Resource, access.State.UsageType);
            }
            for (const Barrier& barrier : plan.Barriers)
            {
                requireUsage(barrier.Resource, barrier.After.UsageType);
                if (barrier.AliasedResourceBefore != kInvalidIndex)
                    requiredResources[barrier.AliasedResourceBefore] = true;
            }

            std::unordered_map<const void*, ResourceId> importedOwners;
            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                if (!requiredResources[resourceId])
                    continue;

                const Detail::ResourceDef& resource = definition.Resources[resourceId];
                if (resource.Imported && resource.InitialState.UsageType != Usage::Undefined)
                {
                    requiredUsageMasks[resourceId] |=
                        1u << static_cast<uint32_t>(resource.InitialState.UsageType);
                }

                const Detail::ResourceResolveRequest request{
                    resourceId,
                    plan.Resources[resourceId].MemorySlot,
                    resource.Desc,
                    requiredUsageMasks[resourceId],
                    resource.Imported,
                    resource.ExternalResource,
                    resource.ExternalType,
                    resource.InitialState};

                Detail::ResourceResolveResult resolved;
                std::string message;
                const DiagnosticCode resolution =
                    backend->ResolveResource(request, resolved, message);
                if (resolution != DiagnosticCode::None)
                {
                    Detail::AddDiagnostic(
                        m_Impl->RuntimeDiagnostics,
                        resolution,
                        "Resource '" + resource.Name + "': " + message,
                        kInvalidIndex,
                        resourceId);
                    continue;
                }
                if (resolved.RuntimeResource == nullptr)
                {
                    Detail::AddDiagnostic(
                        m_Impl->RuntimeDiagnostics,
                        DiagnosticCode::UnresolvedRuntimeResource,
                        "Resource '" + resource.Name +
                            "': the runtime resolver returned a null resource.",
                        kInvalidIndex,
                        resourceId);
                    continue;
                }

                resolvedResources[resourceId] = resolved.RuntimeResource;
                if (!resource.Imported)
                    continue;

                const void* identity = resolved.NativeResourceIdentity != nullptr
                                           ? resolved.NativeResourceIdentity
                                           : resolved.RuntimeResource;
                const auto [owner, inserted] = importedOwners.emplace(identity, resourceId);
                if (!inserted && owner->second != resourceId)
                {
                    Detail::AddDiagnostic(
                        m_Impl->RuntimeDiagnostics,
                        DiagnosticCode::DuplicateRuntimeResource,
                        "Imported resource '" + resource.Name +
                            "' aliases another logical resource. Whole-resource state tracking requires one ResourceId per native resource.",
                        kInvalidIndex,
                        resourceId);
                }
            }

            if (Detail::HasErrors(m_Impl->RuntimeDiagnostics))
                return false;
        }

        if (backend != nullptr)
            m_Impl->RuntimeResources = resolvedResources;
        ExecutionGuard guard(m_Impl->Executing, m_Impl->RuntimeResources);
        auto recordAndFlushBarrierBatch =
            [&](const std::vector<uint32_t>& barrierIndices) -> bool
        {
            if (backend == nullptr || barrierIndices.empty())
                return true;

            bool hasRecordedBarriers = false;
            for (uint32_t barrierIndex : barrierIndices)
            {
                const Barrier& barrier = plan.Barriers[barrierIndex];
                std::string message;
                const DiagnosticCode result = backend->RecordBarrier(
                    barrier,
                    barrier.AliasedResourceBefore != kInvalidIndex
                        ? resolvedResources[barrier.AliasedResourceBefore]
                        : nullptr,
                    resolvedResources[barrier.Resource],
                    message);
                if (result != DiagnosticCode::None)
                {
                    if (hasRecordedBarriers)
                        backend->FlushBarrierBatch();
                    Detail::AddDiagnostic(
                        m_Impl->RuntimeDiagnostics,
                        result,
                        std::move(message),
                        barrier.Pass,
                        barrier.Resource);
                    return false;
                }
                hasRecordedBarriers = true;
            }
            backend->FlushBarrierBatch();
            return true;
        };

        for (uint32_t executionIndex = 0;
             executionIndex < plan.ExecutionOrder.size();
             ++executionIndex)
        {
            const PassId passId = plan.ExecutionOrder[executionIndex];
            if (!recordAndFlushBarrierBatch(plan.BarrierBatches.BeforePass[passId]))
                return false;

            PassContext context(*this, passId, executionIndex, userContext, commandContext);
            definition.Passes[passId].Execute(context);
        }

        if (!recordAndFlushBarrierBatch(plan.BarrierBatches.Epilogue))
            return false;

        return !Detail::HasErrors(m_Impl->RuntimeDiagnostics);
    }
} // namespace RenderGraph

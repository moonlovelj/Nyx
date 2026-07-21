#include "pch.h"
#include "RenderGraphInternal.h"

#include <functional>
#include <queue>
#include <set>
#include <tuple>

namespace RenderGraph::Detail
{
    namespace
    {
        using EdgeKey = std::tuple<PassId, PassId, EdgeKind, ResourceId, uint32_t>;

        EdgeKey MakeEdgeKey(const EdgeRecord& edge)
        {
            return {edge.From, edge.To, edge.Kind, edge.Resource, edge.Version};
        }

        void PreparePlan(const GraphDefinition& definition, CompiledPlan& plan)
        {
            plan.Passes.resize(definition.Passes.size());
            plan.Resources.resize(definition.Resources.size());
            plan.BarrierBatches.BeforePass.resize(definition.Passes.size());

            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                plan.Resources[resourceId].VersionLifetimes.resize(
                    definition.Resources[resourceId].Versions.size());
            }
        }

        void ValidateDefinition(
            const GraphDefinition& definition,
            uint32_t openBuilderCount,
            std::vector<Diagnostic>& diagnostics)
        {
            if (openBuilderCount != 0)
            {
                AddDiagnostic(
                    diagnostics,
                    DiagnosticCode::BuilderStillOpen,
                    "Compile was called while one or more pass builders were still open.");
            }

            for (PassId passId = 0; passId < definition.Passes.size(); ++passId)
            {
                const PassDef& pass = definition.Passes[passId];
                if (!pass.Execute)
                {
                    AddDiagnostic(
                        diagnostics,
                        DiagnosticCode::MissingExecuteCallback,
                        "Pass '" + pass.Name + "' has no execute callback.",
                        passId);
                }
            }

            for (const ExportDef& exportRecord : definition.Exports)
            {
                const ResourceDef& resource = definition.Resources[exportRecord.Resource];
                if (exportRecord.Version != resource.GetLatestVersion() ||
                    !resource.IsInitialized(exportRecord.Version))
                {
                    AddDiagnostic(
                        diagnostics,
                        DiagnosticCode::InvalidExport,
                        "Only the initialized latest version of resource '" + resource.Name +
                            "' can be exported.",
                        kInvalidIndex,
                        exportRecord.Resource);
                }
            }
        }

        template <typename KeepPass>
        std::vector<EdgeRecord> BuildTimelineEdgesFor(
            const GraphDefinition& definition,
            KeepPass&& keepPass)
        {
            std::vector<EdgeRecord> edges;
            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                const ResourceDef& resource = definition.Resources[resourceId];
                PassId lastWriter = kInvalidIndex;
                uint32_t lastWriterVersion = 0;
                std::vector<std::pair<PassId, uint32_t>> pendingReaders;

                for (uint32_t versionIndex = 0; versionIndex < resource.Versions.size(); ++versionIndex)
                {
                    const VersionDef& version = resource.Versions[versionIndex];
                    const PassId writer = version.Producer;
                    const bool keepWriter = writer != kInvalidIndex && keepPass(writer);

                    if (keepWriter)
                    {
                        if (lastWriter != kInvalidIndex && lastWriter != writer)
                        {
                            edges.push_back({lastWriter,
                                             writer,
                                             EdgeKind::WAW,
                                             resourceId,
                                             lastWriterVersion});
                        }
                        for (const auto& [reader, readerVersion] : pendingReaders)
                        {
                            if (reader != writer)
                            {
                                edges.push_back({reader,
                                                 writer,
                                                 EdgeKind::WAR,
                                                 resourceId,
                                                 readerVersion});
                            }
                        }

                        lastWriter = writer;
                        lastWriterVersion = versionIndex;
                        pendingReaders.clear();
                    }

                    for (PassId reader : version.Readers)
                    {
                        if (!keepPass(reader))
                            continue;
                        if (keepWriter && reader != writer)
                        {
                            edges.push_back({writer,
                                             reader,
                                             EdgeKind::Data,
                                             resourceId,
                                             versionIndex});
                        }

                        const std::pair<PassId, uint32_t> pending{reader, versionIndex};
                        if (std::find(pendingReaders.begin(), pendingReaders.end(), pending) ==
                            pendingReaders.end())
                        {
                            pendingReaders.push_back(pending);
                        }
                    }
                }
            }
            return edges;
        }

        std::vector<EdgeRecord> BuildDebugEdges(const GraphDefinition& definition)
        {
            return BuildTimelineEdgesFor(
                definition,
                [](PassId)
                { return true; });
        }

        std::vector<EdgeRecord> BuildLiveSchedulingEdges(
            const GraphDefinition& definition,
            const CompiledPlan& plan)
        {
            // Rebuilding after liveness analysis contracts hazards across culled
            // discard writers, preserving the ordering required by live passes.
            return BuildTimelineEdgesFor(
                definition,
                [&](PassId pass)
                { return plan.Passes[pass].Live; });
        }

        void AppendUniqueEdges(
            std::vector<EdgeRecord>& destination,
            const std::vector<EdgeRecord>& source)
        {
            std::set<EdgeKey> knownEdges;
            for (const EdgeRecord& edge : destination)
                knownEdges.insert(MakeEdgeKey(edge));
            for (const EdgeRecord& edge : source)
            {
                if (knownEdges.insert(MakeEdgeKey(edge)).second)
                    destination.push_back(edge);
            }
        }

        void AnalyzeLiveness(
            const GraphDefinition& definition,
            const CompileOptions& options,
            CompiledPlan& plan,
            CompileResult& result)
        {
            std::queue<PassId> liveQueue;
            auto markLive = [&](PassId pass, bool root)
            {
                if (pass == kInvalidIndex || pass >= definition.Passes.size())
                    return;
                if (root)
                    plan.Passes[pass].Root = true;
                if (!plan.Passes[pass].Live)
                {
                    plan.Passes[pass].Live = true;
                    liveQueue.push(pass);
                }
            };

            for (PassId passId = 0; passId < definition.Passes.size(); ++passId)
            {
                const PassDef& pass = definition.Passes[passId];
                if (HasAny(pass.Flags, PassFlags::SideEffect) ||
                    HasAny(pass.Flags, PassFlags::NeverCull))
                {
                    markLive(passId, true);
                }
            }

            for (const ExportDef& exportRecord : definition.Exports)
            {
                markLive(
                    definition.Resources[exportRecord.Resource]
                        .Versions[exportRecord.Version]
                        .Producer,
                    true);
            }

            if (!options.EnablePassCulling)
            {
                for (PassId passId = 0; passId < definition.Passes.size(); ++passId)
                    markLive(passId, false);
            }
            else
            {
                while (!liveQueue.empty())
                {
                    const PassId passId = liveQueue.front();
                    liveQueue.pop();
                    for (const AccessRecord& access : definition.Passes[passId].Accesses)
                    {
                        if (!Reads(access.Mode))
                            continue;
                        markLive(
                            definition.Resources[access.Resource]
                                .Versions[access.InputVersion]
                                .Producer,
                            false);
                    }
                }
            }

            uint32_t liveCount = 0;
            for (const PassPlan& pass : plan.Passes)
                liveCount += pass.Live ? 1u : 0u;

            result.LivePassCount = liveCount;
            result.CulledPassCount = static_cast<uint32_t>(definition.Passes.size()) - liveCount;
        }

        bool StableTopologicalSort(
            const GraphDefinition& definition,
            CompiledPlan& plan,
            std::vector<Diagnostic>& diagnostics,
            uint32_t liveCount)
        {
            std::vector<std::vector<PassId>> adjacency(definition.Passes.size());
            std::vector<uint32_t> indegree(definition.Passes.size(), 0);
            std::set<std::pair<PassId, PassId>> schedulingEdges;

            for (const EdgeRecord& edge : plan.Edges)
            {
                if (edge.From == edge.To || !plan.Passes[edge.From].Live ||
                    !plan.Passes[edge.To].Live)
                {
                    continue;
                }

                if (schedulingEdges.emplace(edge.From, edge.To).second)
                {
                    adjacency[edge.From].push_back(edge.To);
                    ++indegree[edge.To];
                }
            }

            std::priority_queue<PassId, std::vector<PassId>, std::greater<PassId>> ready;
            for (PassId passId = 0; passId < definition.Passes.size(); ++passId)
            {
                if (plan.Passes[passId].Live && indegree[passId] == 0)
                    ready.push(passId);
            }

            while (!ready.empty())
            {
                const PassId pass = ready.top();
                ready.pop();
                plan.Passes[pass].ExecutionIndex =
                    static_cast<uint32_t>(plan.ExecutionOrder.size());
                plan.ExecutionOrder.push_back(pass);

                for (PassId next : adjacency[pass])
                {
                    if (--indegree[next] == 0)
                        ready.push(next);
                }
            }

            if (plan.ExecutionOrder.size() == liveCount)
                return true;

            AddDiagnostic(
                diagnostics,
                DiagnosticCode::CycleDetected,
                "The live render graph contains a dependency cycle.");
            return false;
        }

        void AnalyzeLifetimes(
            const GraphDefinition& definition,
            const CompileOptions& options,
            CompiledPlan& plan)
        {
            for (uint32_t executionIndex = 0;
                 executionIndex < plan.ExecutionOrder.size();
                 ++executionIndex)
            {
                const PassDef& pass = definition.Passes[plan.ExecutionOrder[executionIndex]];
                for (const AccessRecord& access : pass.Accesses)
                {
                    ResourcePlan& resource = plan.Resources[access.Resource];
                    resource.LifetimeRange.Include(executionIndex);
                    if (Reads(access.Mode))
                        resource.VersionLifetimes[access.InputVersion].Include(executionIndex);
                    if (Writes(access.Mode))
                        resource.VersionLifetimes[access.OutputVersion].Include(executionIndex);
                }
            }

            const uint32_t epilogueIndex = static_cast<uint32_t>(plan.ExecutionOrder.size());
            for (const ExportDef& exportRecord : definition.Exports)
            {
                ResourcePlan& resource = plan.Resources[exportRecord.Resource];
                resource.VersionLifetimes[exportRecord.Version].ExtendTo(epilogueIndex);
                resource.LifetimeRange.ExtendTo(epilogueIndex);
            }

            if (!options.ExtendTransientLifetimes)
                return;

            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                if (definition.Resources[resourceId].Imported ||
                    plan.Resources[resourceId].LifetimeRange.IsEmpty())
                {
                    continue;
                }

                ResourcePlan& resource = plan.Resources[resourceId];
                resource.LifetimeRange.First = 0;
                resource.LifetimeRange.Last = epilogueIndex;
                for (Lifetime& version : resource.VersionLifetimes)
                {
                    if (!version.IsEmpty())
                    {
                        version.First = 0;
                        version.Last = epilogueIndex;
                    }
                }
            }
        }

        enum class AliasClass : uint8_t
        {
            Buffer,
            Texture,
            RenderTargetOrDepthStencil,
        };

        AliasClass GetAliasClass(const ResourceDef& resource)
        {
            if (resource.Desc.Kind == ResourceKind::Buffer)
                return AliasClass::Buffer;

            const ResourceFlags flags = resource.Desc.Texture.Flags;
            return HasAny(
                       flags,
                       ResourceFlags::AllowRenderTarget |
                           ResourceFlags::AllowDepthStencil)
                       ? AliasClass::RenderTargetOrDepthStencil
                       : AliasClass::Texture;
        }

        bool CanShareMemorySlot(const ResourceDef& lhs, const ResourceDef& rhs)
        {
            return GetAliasClass(lhs) == GetAliasClass(rhs);
        }

        void AssignMemorySlots(
            const GraphDefinition& definition,
            const CompileOptions& options,
            CompiledPlan& plan,
            CompileResult& result)
        {
            std::vector<ResourceId> transientResources;
            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                if (!definition.Resources[resourceId].Imported &&
                    !plan.Resources[resourceId].LifetimeRange.IsEmpty())
                {
                    transientResources.push_back(resourceId);
                }
            }

            std::stable_sort(
                transientResources.begin(),
                transientResources.end(),
                [&](ResourceId lhs, ResourceId rhs)
                {
                    const Lifetime& left = plan.Resources[lhs].LifetimeRange;
                    const Lifetime& right = plan.Resources[rhs].LifetimeRange;
                    if (left.First != right.First)
                        return left.First < right.First;
                    return lhs < rhs;
                });

            for (ResourceId resourceId : transientResources)
            {
                const ResourceDef& resource = definition.Resources[resourceId];
                const Lifetime life = plan.Resources[resourceId].LifetimeRange;
                MemorySlotId selected = kInvalidIndex;

                if (options.EnableMemoryAliasing)
                {
                    for (MemorySlotId slotId = 0;
                         slotId < plan.MemorySlots.size();
                         ++slotId)
                    {
                        const MemorySlotPlan& slot = plan.MemorySlots[slotId];
                        const ResourceDef& firstOwner =
                            definition.Resources[slot.Uses.front().Resource];
                        if (CanShareMemorySlot(resource, firstOwner) &&
                            slot.LastUse < life.First)
                        {
                            selected = slotId;
                            break;
                        }
                    }
                }

                if (selected == kInvalidIndex)
                {
                    selected = static_cast<MemorySlotId>(plan.MemorySlots.size());
                    MemorySlotPlan slot;
                    slot.LastUse = life.Last;
                    slot.Uses.push_back({resourceId, life});
                    plan.MemorySlots.push_back(std::move(slot));
                }
                else
                {
                    plan.MemorySlots[selected].LastUse = life.Last;
                    plan.MemorySlots[selected].Uses.push_back({resourceId, life});
                }

                plan.Resources[resourceId].MemorySlot = selected;
            }

            result.MemorySlotCount = static_cast<uint32_t>(plan.MemorySlots.size());
        }

        void PlanBarriers(
            const GraphDefinition& definition,
            CompiledPlan& plan,
            CompileResult& result)
        {
            struct StateTracker
            {
                ResourceState State;
                AccessMode LastMode = AccessMode::Read;
                bool HasAccess = false;
                bool ExternalUavWritePending = false;
            };

            std::vector<StateTracker> states(definition.Resources.size());
            for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
            {
                if (definition.Resources[resourceId].Imported)
                {
                    states[resourceId].State = definition.Resources[resourceId].InitialState;
                    // InitialUsage cannot describe whether work outside the graph
                    // wrote an imported UAV. Be conservative at the first same-UAV
                    // access so callers never need a graph-boundary UAV barrier.
                    states[resourceId].ExternalUavWritePending =
                        definition.Resources[resourceId].InitialState.UsageType ==
                        Usage::UnorderedAccess;
                }
            }

            auto appendBeforePass = [&](PassId pass, Barrier barrier)
            {
                const uint32_t index = static_cast<uint32_t>(plan.Barriers.size());
                plan.Barriers.push_back(std::move(barrier));
                plan.BarrierBatches.BeforePass[pass].push_back(index);
            };

            auto appendEpilogue = [&](Barrier barrier)
            {
                const uint32_t index = static_cast<uint32_t>(plan.Barriers.size());
                plan.Barriers.push_back(std::move(barrier));
                plan.BarrierBatches.Epilogue.push_back(index);
            };

            std::vector<std::vector<Barrier>> aliasBarriers(definition.Passes.size());
            for (const MemorySlotPlan& slot : plan.MemorySlots)
            {
                if (slot.Uses.size() < 2)
                    continue;

                ResourceId previous = kInvalidIndex;
                for (const MemoryUse& use : slot.Uses)
                {
                    const PassId firstPass = plan.ExecutionOrder[use.LifetimeRange.First];
                    Barrier barrier;
                    barrier.Kind = BarrierKind::Aliasing;
                    barrier.Resource = use.Resource;
                    barrier.Pass = firstPass;
                    barrier.AliasedResourceBefore = previous;
                    aliasBarriers[firstPass].push_back(barrier);
                    previous = use.Resource;
                }
            }

            for (PassId passId : plan.ExecutionOrder)
            {
                for (const Barrier& barrier : aliasBarriers[passId])
                    appendBeforePass(passId, barrier);

                for (const AccessRecord& access : definition.Passes[passId].Accesses)
                {
                    StateTracker& tracker = states[access.Resource];
                    const uint32_t version = Reads(access.Mode) && !Writes(access.Mode)
                                                 ? access.InputVersion
                                                 : access.OutputVersion;

                    if (RequiresTransition(tracker.State, access.State))
                    {
                        appendBeforePass(passId, {BarrierKind::Transition,
                                                  access.Resource,
                                                  version,
                                                  passId,
                                                  false,
                                                  tracker.State,
                                                  access.State});
                    }
                    else if (access.State.UsageType == Usage::UnorderedAccess &&
                             (tracker.ExternalUavWritePending ||
                              (tracker.HasAccess &&
                               (Writes(tracker.LastMode) || Writes(access.Mode)))))
                    {
                        appendBeforePass(passId, {BarrierKind::UnorderedAccess,
                                                  access.Resource,
                                                  version,
                                                  passId,
                                                  false,
                                                  tracker.State,
                                                  access.State});
                    }

                    tracker.State = access.State;
                    tracker.LastMode = access.Mode;
                    tracker.HasAccess = true;
                    tracker.ExternalUavWritePending = false;
                    plan.Resources[access.Resource].LastAccessPass = passId;
                }
            }

            for (const ExportDef& exportRecord : definition.Exports)
            {
                if (exportRecord.FinalUsage == Usage::Undefined)
                    continue;

                StateTracker& tracker = states[exportRecord.Resource];
                const ResourceState finalState =
                    MakeCanonicalResourceState(exportRecord.FinalUsage);
                const PassId lastAccess = plan.Resources[exportRecord.Resource].LastAccessPass;

                if (RequiresTransition(tracker.State, finalState))
                {
                    appendEpilogue({BarrierKind::FinalTransition,
                                    exportRecord.Resource,
                                    exportRecord.Version,
                                    lastAccess,
                                    true,
                                    tracker.State,
                                    finalState});
                    tracker.State = finalState;
                }
                else if (finalState.UsageType == Usage::UnorderedAccess &&
                         tracker.HasAccess)
                {
                    appendEpilogue({BarrierKind::UnorderedAccess,
                                    exportRecord.Resource,
                                    exportRecord.Version,
                                    lastAccess,
                                    true,
                                    tracker.State,
                                    finalState});
                }
            }

            result.BarrierCount = static_cast<uint32_t>(plan.Barriers.size());
        }
    } // namespace

    void CompileGraph(
        const GraphDefinition& definition,
        uint32_t openBuilderCount,
        const std::vector<Diagnostic>& buildDiagnostics,
        const CompileOptions& options,
        CompileArtifact& artifact)
    {
        artifact.Invalidate();
        artifact.Attempted = true;
        artifact.Options = options;
        PreparePlan(definition, artifact.Plan);

        ValidateDefinition(definition, openBuilderCount, artifact.Diagnostics);
        if (HasErrors(buildDiagnostics) || HasErrors(artifact.Diagnostics))
            return;

        artifact.Plan.Edges = BuildDebugEdges(definition);
        AnalyzeLiveness(definition, options, artifact.Plan, artifact.Result);
        AppendUniqueEdges(
            artifact.Plan.Edges,
            BuildLiveSchedulingEdges(definition, artifact.Plan));

        if (!StableTopologicalSort(
                definition,
                artifact.Plan,
                artifact.Diagnostics,
                artifact.Result.LivePassCount))
        {
            return;
        }

        AnalyzeLifetimes(definition, options, artifact.Plan);
        AssignMemorySlots(definition, options, artifact.Plan, artifact.Result);
        PlanBarriers(definition, artifact.Plan, artifact.Result);

        artifact.Result.Succeeded = true;
    }
} // namespace RenderGraph::Detail

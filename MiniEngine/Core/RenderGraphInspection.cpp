#include "pch.h"
#include "RenderGraphDebug.h"
#include "RenderGraphPrivate.h"

namespace RenderGraph
{
    DebugSnapshot Graph::CaptureDebugSnapshot() const
    {
        const Detail::GraphDefinition& definition = m_Impl->Definition;
        const Detail::CompileArtifact& compilation = m_Impl->Compilation;

        DebugSnapshot snapshot;
        snapshot.GraphName = m_Impl->Name;
        snapshot.Epoch = m_Impl->Epoch;
        snapshot.State = compilation.Result.Succeeded
                             ? CompileState::Succeeded
                             : (compilation.Attempted ? CompileState::Failed : CompileState::Uncompiled);
        snapshot.Options = compilation.Options;
        snapshot.Result = compilation.Result;
        snapshot.Barriers = compilation.Plan.Barriers;
        snapshot.Diagnostics = CollectDiagnostics();

        snapshot.Passes.reserve(definition.Passes.size());
        for (PassId passId = 0; passId < definition.Passes.size(); ++passId)
        {
            const Detail::PassDef& pass = definition.Passes[passId];
            const Detail::PassPlan passPlan = passId < compilation.Plan.Passes.size()
                                                  ? compilation.Plan.Passes[passId]
                                                  : Detail::PassPlan{};

            DebugPass debugPass;
            debugPass.Id = passId;
            debugPass.Name = pass.Name;
            debugPass.InsertionIndex = passId;
            debugPass.ExecutionIndex = passPlan.ExecutionIndex;
            debugPass.Flags = pass.Flags;
            debugPass.Live = passPlan.Live;
            debugPass.Root = passPlan.Root;
            debugPass.Accesses.reserve(pass.Accesses.size());
            for (const Detail::AccessRecord& access : pass.Accesses)
            {
                debugPass.Accesses.push_back({access.Resource,
                                              access.InputVersion,
                                              access.OutputVersion,
                                              access.Mode,
                                              access.State});
            }
            snapshot.Passes.push_back(std::move(debugPass));
        }

        for (ResourceId resourceId = 0; resourceId < definition.Resources.size(); ++resourceId)
        {
            const Detail::ResourceDef& resource = definition.Resources[resourceId];
            const Detail::ResourcePlan* resourcePlan =
                resourceId < compilation.Plan.Resources.size()
                    ? &compilation.Plan.Resources[resourceId]
                    : nullptr;

            for (uint32_t versionIndex = 0; versionIndex < resource.Versions.size(); ++versionIndex)
            {
                const Detail::VersionDef& version = resource.Versions[versionIndex];
                const Detail::Lifetime life = resourcePlan != nullptr &&
                                                      versionIndex < resourcePlan->VersionLifetimes.size()
                                                  ? resourcePlan->VersionLifetimes[versionIndex]
                                                  : Detail::Lifetime{};

                DebugResourceVersion debugResource;
                debugResource.Resource = resourceId;
                debugResource.Version = versionIndex;
                debugResource.Name = resource.Name;
                debugResource.Kind = resource.Desc.Kind;
                debugResource.Imported = resource.Imported;
                debugResource.Exported = std::any_of(
                    definition.Exports.begin(),
                    definition.Exports.end(),
                    [&](const Detail::ExportDef& exportRecord)
                    {
                        return exportRecord.Resource == resourceId &&
                               exportRecord.Version == versionIndex;
                    });
                debugResource.Initialized = resource.IsInitialized(versionIndex);
                debugResource.Producer = version.Producer;
                debugResource.Readers = version.Readers;
                debugResource.FirstUse = life.First;
                debugResource.LastUse = life.Last;
                debugResource.MemorySlot = resourcePlan != nullptr
                                               ? resourcePlan->MemorySlot
                                               : kInvalidIndex;
                snapshot.ResourceVersions.push_back(std::move(debugResource));
            }
        }

        snapshot.Edges.reserve(compilation.Plan.Edges.size());
        for (const Detail::EdgeRecord& edge : compilation.Plan.Edges)
        {
            snapshot.Edges.push_back({edge.From,
                                      edge.To,
                                      edge.Kind,
                                      edge.Resource,
                                      edge.Version});
        }
        return snapshot;
    }
} // namespace RenderGraph

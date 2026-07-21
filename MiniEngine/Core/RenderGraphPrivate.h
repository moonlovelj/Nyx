#pragma once

#include "RenderGraphInternal.h"

#include <optional>

namespace RenderGraph
{
    // Private graph storage shared only by the authoring, execution, pass, and
    // inspection translation units. Keeping it out of RenderGraph.h preserves the
    // public PImpl boundary while allowing each responsibility to live in a small
    // source file.
    struct Graph::Impl
    {
        explicit Impl(std::string graphName);

        void InvalidateCompiledPlan();
        bool RejectMutationDuringExecution(const char* operation);

        Detail::PassDef* GetOpenPass(
            PassId pass,
            uint64_t builderEpoch,
            const char* operation);

        std::optional<ResourceId> RegisterResource(
            std::string_view name,
            const Detail::ResourceDescriptor& descriptor,
            bool imported,
            void* externalResource,
            Detail::ExternalResourceType externalType,
            Usage initialUsage,
            ShaderStage initialStages);

        void ResetFrame();

        std::string Name;
        uint64_t Epoch = 0;
        Detail::GraphDefinition Definition;
        Detail::CompileArtifact Compilation;
        std::vector<Diagnostic> BuildDiagnostics;
        std::vector<Diagnostic> RuntimeDiagnostics;
        // Populated only for the duration of Execute so PassContext resolves both
        // imported and transient resources through the same ResourceId path.
        std::vector<void*> RuntimeResources;
        std::shared_ptr<void> BuilderToken = std::make_shared<uint8_t>();
        uint32_t OpenBuilderCount = 0;
        bool Executing = false;
    };
} // namespace RenderGraph

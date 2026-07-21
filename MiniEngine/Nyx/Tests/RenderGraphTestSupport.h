#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CppUnitTest.h"

#include "RenderGraph.h"
#include "RenderGraphD3D12.h"
#include "RenderGraphDebug.h"
#include "RenderGraphInternal.h"
#include "RenderGraphTestAccess.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NyxRenderGraphTests
{
    template <typename GraphType>
    concept HasLowLevelAddPass = requires(GraphType& graph) {
        graph.AddPass("Pass");
    };

    template <typename GraphType>
    concept HasCallbackOnlyExecute = requires(GraphType& graph) {
        graph.Execute();
    };

    template <typename BuilderType>
    concept HasManualBuilderLifecycle = requires(
        BuilderType& builder,
        typename BuilderType::ExecuteCallback callback) {
        builder.IsOpen();
        builder.SetExecute(std::move(callback));
        builder.Close();
    };

    static_assert(!HasLowLevelAddPass<RenderGraph::Graph>);
    static_assert(!HasCallbackOnlyExecute<RenderGraph::Graph>);
    static_assert(!HasManualBuilderLifecycle<RenderGraph::PassBuilder>);
    static_assert(requires(RenderGraph::Graph& graph, CommandContext& commandContext) {
        { graph.Execute(commandContext) } -> std::same_as<bool>;
    });
    static_assert(!std::is_copy_constructible_v<RenderGraph::PassContext>);
    static_assert(!std::is_move_constructible_v<RenderGraph::PassContext>);

    template <typename Handle>
    struct SemanticAccessSupport
    {
        static constexpr bool Generic = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.ReadSRV(handle) } -> std::same_as<Handle>;
            { builder.ReadUAV(handle) } -> std::same_as<Handle>;
            { builder.WriteUAV(handle) } -> std::same_as<Handle>;
            { builder.ReadWriteUAV(handle) } -> std::same_as<Handle>;
            { builder.CopySrc(handle) } -> std::same_as<Handle>;
            { builder.CopyDst(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool WriteRTV = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.WriteRTV(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool ReadWriteRTV = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.ReadWriteRTV(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool ReadDepth = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.ReadDepth(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool WriteDepth = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.WriteDepth(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool ReadWriteDepth = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.ReadWriteDepth(handle) } -> std::same_as<Handle>;
        };

        static constexpr bool ReadIndirectArgument = requires(RenderGraph::PassBuilder& builder, Handle handle) {
            { builder.ReadIndirectArgument(handle) } -> std::same_as<Handle>;
        };
    };

    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::Generic);
    static_assert(SemanticAccessSupport<RenderGraph::BufferHandle>::Generic);
    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::WriteRTV);
    static_assert(!SemanticAccessSupport<RenderGraph::BufferHandle>::WriteRTV);
    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::ReadWriteRTV);
    static_assert(!SemanticAccessSupport<RenderGraph::BufferHandle>::ReadWriteRTV);
    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::ReadDepth);
    static_assert(!SemanticAccessSupport<RenderGraph::BufferHandle>::ReadDepth);
    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::WriteDepth);
    static_assert(!SemanticAccessSupport<RenderGraph::BufferHandle>::WriteDepth);
    static_assert(SemanticAccessSupport<RenderGraph::TextureHandle>::ReadWriteDepth);
    static_assert(!SemanticAccessSupport<RenderGraph::BufferHandle>::ReadWriteDepth);
    static_assert(!SemanticAccessSupport<RenderGraph::TextureHandle>::ReadIndirectArgument);
    static_assert(SemanticAccessSupport<RenderGraph::BufferHandle>::ReadIndirectArgument);

    namespace
    {
        RenderGraph::TextureDesc TextureDesc(
            RenderGraph::ResourceFlags flags = RenderGraph::ResourceFlags::None,
            uint32_t width = 64,
            uint32_t height = 64)
        {
            RenderGraph::TextureDesc desc;
            desc.Width = width;
            desc.Height = height;
            desc.Format = 28;
            desc.Flags = flags;
            return desc;
        }

        RenderGraph::BufferDesc BufferDesc(
            RenderGraph::ResourceFlags flags = RenderGraph::ResourceFlags::None,
            uint64_t size = 4096)
        {
            RenderGraph::BufferDesc desc;
            desc.SizeInBytes = size;
            desc.StrideInBytes = 16;
            desc.Flags = flags;
            return desc;
        }

        RenderGraph::PassBuilder BeginPass(
            RenderGraph::Graph& graph,
            std::string_view name,
            RenderGraph::PassFlags flags = RenderGraph::PassFlags::None)
        {
            return RenderGraph::Detail::GraphTestAccess::BeginPass(graph, name, flags);
        }

        void SetExecute(
            RenderGraph::PassBuilder& pass,
            RenderGraph::PassBuilder::ExecuteCallback callback)
        {
            RenderGraph::Detail::GraphTestAccess::SetExecute(pass, std::move(callback));
        }

        void Close(RenderGraph::PassBuilder& pass)
        {
            RenderGraph::Detail::GraphTestAccess::Close(pass);
        }

        bool IsOpen(const RenderGraph::PassBuilder& pass)
        {
            return RenderGraph::Detail::GraphTestAccess::IsOpen(pass);
        }

        bool ExecuteCallbacks(RenderGraph::Graph& graph, void* userContext = nullptr)
        {
            return RenderGraph::Detail::GraphTestAccess::ExecuteCallbacks(graph, userContext);
        }

        void SetNoop(RenderGraph::PassBuilder& pass)
        {
            SetExecute(pass, [](RenderGraph::PassContext&) {});
        }

        bool HasDiagnostic(const RenderGraph::Graph& graph, RenderGraph::DiagnosticCode code)
        {
            const std::vector<RenderGraph::Diagnostic> diagnostics = graph.CollectDiagnostics();
            return std::any_of(
                diagnostics.begin(),
                diagnostics.end(),
                [code](const RenderGraph::Diagnostic& diagnostic)
                { return diagnostic.Code == code; });
        }

        size_t CountBarriers(const RenderGraph::Graph& graph, RenderGraph::BarrierKind kind)
        {
            return static_cast<size_t>(std::count_if(
                graph.GetBarriers().begin(),
                graph.GetBarriers().end(),
                [kind](const RenderGraph::Barrier& barrier)
                { return barrier.Kind == kind; }));
        }

        RenderGraph::MemorySlotId GetMemorySlot(
            const RenderGraph::Graph& graph,
            RenderGraph::ResourceId resource)
        {
            const auto snapshot = graph.CaptureDebugSnapshot();
            const auto found = std::find_if(
                snapshot.ResourceVersions.begin(),
                snapshot.ResourceVersions.end(),
                [resource](const RenderGraph::DebugResourceVersion& version)
                {
                    return version.Resource == resource;
                });
            return found != snapshot.ResourceVersions.end()
                       ? found->MemorySlot
                       : RenderGraph::kInvalidIndex;
        }

        enum class RuntimeEventKind
        {
            Aliasing,
            Transition,
            UnorderedAccess,
            BarrierBatchFlush,
            Pass,
        };

        struct RuntimeEvent
        {
            RuntimeEventKind Kind = RuntimeEventKind::Pass;
            RenderGraph::BarrierKind BarrierKind = RenderGraph::BarrierKind::Transition;
            RenderGraph::PassId Pass = RenderGraph::kInvalidIndex;
            RenderGraph::ResourceId Resource = RenderGraph::kInvalidIndex;
            RenderGraph::Usage After = RenderGraph::Usage::Undefined;
            void* BeforeRuntimeResource = nullptr;
            void* AfterRuntimeResource = nullptr;
        };

        class RecordingExecutionBackend final : public RenderGraph::Detail::ExecutionBackend
        {
          public:
            explicit RecordingExecutionBackend(
                std::vector<RuntimeEvent>& events,
                bool resolveTransients = false)
                : m_Events(events), m_ResolveTransients(resolveTransients)
            {
            }

            RenderGraph::DiagnosticCode ValidateExecution(std::string&) const override
            {
                return RenderGraph::DiagnosticCode::None;
            }

            RenderGraph::DiagnosticCode ResolveResource(
                const RenderGraph::Detail::ResourceResolveRequest& request,
                RenderGraph::Detail::ResourceResolveResult& result,
                std::string& message) override
            {
                result = {};
                if (!request.Imported)
                {
                    if (m_ResolveTransients)
                    {
                        result.RuntimeResource = reinterpret_cast<void*>(
                            static_cast<uintptr_t>(request.LogicalResourceId) + 1u);
                        result.NativeResourceIdentity = result.RuntimeResource;
                        return RenderGraph::DiagnosticCode::None;
                    }
                    message = "No transient test allocation.";
                    return RenderGraph::DiagnosticCode::UnresolvedRuntimeResource;
                }
                result.RuntimeResource = request.ExternalResource;
                result.NativeResourceIdentity = request.ExternalResource;
                return RenderGraph::DiagnosticCode::None;
            }

            RenderGraph::DiagnosticCode RecordBarrier(
                const RenderGraph::Barrier& barrier,
                void* beforeResource,
                void* afterResource,
                std::string&) override
            {
                const RuntimeEventKind kind = barrier.Kind == RenderGraph::BarrierKind::Aliasing
                                                  ? RuntimeEventKind::Aliasing
                                              : barrier.Kind == RenderGraph::BarrierKind::UnorderedAccess
                                                  ? RuntimeEventKind::UnorderedAccess
                                                  : RuntimeEventKind::Transition;
                m_Events.push_back(
                    {kind,
                     barrier.Kind,
                     barrier.Pass,
                     barrier.Resource,
                     barrier.After.UsageType,
                     beforeResource,
                     afterResource});
                return RenderGraph::DiagnosticCode::None;
            }

            void FlushBarrierBatch() override
            {
                m_Events.push_back({RuntimeEventKind::BarrierBatchFlush});
            }

          private:
            std::vector<RuntimeEvent>& m_Events;
            bool m_ResolveTransients = false;
        };

        void AssertNativeState(
            RenderGraph::ResourceState state,
            D3D12_RESOURCE_STATES expected)
        {
            const auto actual = RenderGraph::TryGetD3D12ResourceState(state);
            Assert::IsTrue(actual.has_value());
            Assert::AreEqual(
                static_cast<uint32_t>(expected),
                static_cast<uint32_t>(*actual));
        }

        void AssertAccess(
            const RenderGraph::DebugAccess& access,
            RenderGraph::ResourceId resource,
            RenderGraph::AccessMode mode,
            RenderGraph::Usage usage,
            RenderGraph::ShaderStage stages,
            uint32_t inputVersion,
            uint32_t outputVersion)
        {
            Assert::AreEqual(resource, access.Resource);
            Assert::IsTrue(access.Mode == mode);
            Assert::IsTrue(access.State.UsageType == usage);
            Assert::IsTrue(access.State.Stages == stages);
            Assert::AreEqual(inputVersion, access.InputVersion);
            Assert::AreEqual(outputVersion, access.OutputVersion);
        }

        bool HasEdge(
            const RenderGraph::DebugSnapshot& snapshot,
            RenderGraph::PassId from,
            RenderGraph::PassId to,
            RenderGraph::EdgeKind kind)
        {
            return std::any_of(
                snapshot.Edges.begin(),
                snapshot.Edges.end(),
                [&](const RenderGraph::DebugEdge& edge)
                {
                    return edge.From == from && edge.To == to && edge.Kind == kind;
                });
        }

        bool HasAnyEdge(
            const RenderGraph::DebugSnapshot& snapshot,
            RenderGraph::PassId first,
            RenderGraph::PassId second)
        {
            return std::any_of(
                snapshot.Edges.begin(),
                snapshot.Edges.end(),
                [&](const RenderGraph::DebugEdge& edge)
                {
                    return (edge.From == first && edge.To == second) ||
                           (edge.From == second && edge.To == first);
                });
        }

        uintmax_t FileSize(const std::filesystem::path& path)
        {
            return std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
        }

        std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::string(
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>());
        }

        std::filesystem::path UniqueArtifactPath(std::string_view label, uint64_t epoch)
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            return std::filesystem::temp_directory_path() /
                   (std::string(label) + std::to_string(epoch) + "_" + std::to_string(nonce));
        }
    } // namespace
} // namespace NyxRenderGraphTests

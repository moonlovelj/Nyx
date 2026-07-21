#pragma once

#include "RenderGraphInternal.h"

namespace RenderGraph::Detail
{
    // White-box adapter for compiler/runtime tests. Production code only keeps the
    // friend declaration; the implementation belongs to the test target.
    struct GraphTestAccess
    {
        static PassBuilder BeginPass(
            Graph& graph,
            std::string_view name,
            PassFlags flags = PassFlags::None)
        {
            return graph.BeginPass(name, flags);
        }

        static bool IsOpen(const PassBuilder& builder)
        {
            return builder.IsOpen();
        }

        static void SetExecute(PassBuilder& builder, PassBuilder::ExecuteCallback callback)
        {
            builder.SetExecute(std::move(callback));
        }

        static void Close(PassBuilder& builder)
        {
            builder.Close();
        }

        static bool ExecuteCallbacks(Graph& graph, void* userContext = nullptr)
        {
            return graph.ExecuteInternal(userContext, nullptr, nullptr);
        }

        static bool Execute(
            Graph& graph,
            ExecutionBackend& backend,
            void* userContext = nullptr)
        {
            return graph.ExecuteInternal(userContext, nullptr, &backend);
        }
    };
} // namespace RenderGraph::Detail

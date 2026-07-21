#include "pch.h"
#include "RenderGraphPrivate.h"

#include <utility>

namespace RenderGraph
{
    PassBuilder::PassBuilder(Graph& graph, PassId pass)
        : m_Graph(&graph),
          m_Pass(pass),
          m_Epoch(graph.GetEpoch()),
          m_OwnerToken(graph.m_Impl->BuilderToken)
    {
    }

    PassBuilder::PassBuilder(PassBuilder&& other) noexcept
        : m_Graph(std::exchange(other.m_Graph, nullptr)),
          m_Pass(std::exchange(other.m_Pass, kInvalidIndex)),
          m_Epoch(std::exchange(other.m_Epoch, 0)),
          m_OwnerToken(std::move(other.m_OwnerToken))
    {
    }

    PassBuilder& PassBuilder::operator=(PassBuilder&& other) noexcept
    {
        if (this == &other)
            return *this;

        Close();
        m_Graph = std::exchange(other.m_Graph, nullptr);
        m_Pass = std::exchange(other.m_Pass, kInvalidIndex);
        m_Epoch = std::exchange(other.m_Epoch, 0);
        m_OwnerToken = std::move(other.m_OwnerToken);
        return *this;
    }

    PassBuilder::~PassBuilder()
    {
        Close();
    }

    Graph* PassBuilder::TryGetGraph()
    {
        if (m_Graph == nullptr)
            return nullptr;
        if (!m_OwnerToken.expired())
            return m_Graph;

        Abandon();
        return nullptr;
    }

    void PassBuilder::Abandon()
    {
        m_Graph = nullptr;
        m_Pass = kInvalidIndex;
        m_Epoch = 0;
        m_OwnerToken.reset();
    }

    void PassBuilder::MarkSideEffect()
    {
        if (Graph* graph = TryGetGraph())
            graph->MarkSideEffectInternal(m_Pass, m_Epoch);
    }

    TextureHandle PassBuilder::WriteRTV(TextureHandle handle)
    {
        return Write(handle, Usage::RenderTarget);
    }

    TextureHandle PassBuilder::ReadWriteRTV(TextureHandle handle)
    {
        return ReadWrite(handle, Usage::RenderTarget);
    }

    TextureHandle PassBuilder::ReadDepth(TextureHandle handle)
    {
        return Read(handle, Usage::DepthRead);
    }

    TextureHandle PassBuilder::WriteDepth(TextureHandle handle)
    {
        return Write(handle, Usage::DepthWrite);
    }

    TextureHandle PassBuilder::ReadWriteDepth(TextureHandle handle)
    {
        return ReadWrite(handle, Usage::DepthWrite);
    }

    BufferHandle PassBuilder::ReadIndirectArgument(BufferHandle handle)
    {
        return Read(handle, Usage::IndirectArgument);
    }

    void PassBuilder::SetExecute(ExecuteCallback callback)
    {
        Graph* graph = TryGetGraph();
        if (graph == nullptr)
            return;

        graph->SetExecuteInternal(m_Pass, m_Epoch, std::move(callback));
        Close();
    }

    void PassBuilder::Close()
    {
        if (Graph* graph = TryGetGraph())
            graph->CloseBuilderInternal(m_Pass, m_Epoch);
        Abandon();
    }

    PassContext::PassContext(
        Graph& graph,
        PassId pass,
        uint32_t executionIndex,
        void* userContext,
        CommandContext* commandContext)
        : m_Graph(&graph),
          m_Pass(pass),
          m_ExecutionIndex(executionIndex),
          m_UserContext(userContext),
          m_CommandContext(commandContext)
    {
    }

    PassId PassContext::GetPassId() const
    {
        return m_Pass;
    }

    uint32_t PassContext::GetExecutionIndex() const
    {
        return m_ExecutionIndex;
    }

    std::string_view PassContext::GetPassName() const
    {
        return m_Graph != nullptr ? m_Graph->GetPassName(m_Pass) : std::string_view();
    }

    CommandContext* PassContext::GetCommandContext() const noexcept
    {
        return m_CommandContext;
    }

    bool PassContext::IsDeclaredInternal(
        ResourceKind kind,
        ResourceId resource,
        uint32_t version,
        uint64_t epoch) const
    {
        return m_Graph != nullptr &&
               m_Graph->IsDeclaredInternal(m_Pass, {kind, resource, version, epoch});
    }

    void* PassContext::GetResourceInternal(
        ResourceKind kind,
        ResourceId resource,
        uint32_t version,
        uint64_t epoch) const
    {
        return m_Graph != nullptr
                   ? m_Graph->GetResourceInternal(m_Pass, {kind, resource, version, epoch})
                   : nullptr;
    }

} // namespace RenderGraph

#pragma once

namespace RenderGraph
{
    template <ResourceKind Kind>
    constexpr Handle<Kind>::Handle() = default;

    template <ResourceKind Kind>
    constexpr Handle<Kind>::Handle(
        ResourceId resource,
        uint32_t version,
        uint64_t epoch)
        : m_Resource(resource), m_Version(version), m_Epoch(epoch)
    {
    }

    template <ResourceKind Kind>
    constexpr bool Handle<Kind>::IsValid() const
    {
        return m_Resource != kInvalidIndex && m_Epoch != 0;
    }

    template <ResourceKind Kind>
    constexpr Handle<Kind>::operator bool() const
    {
        return IsValid();
    }

    template <ResourceKind Kind>
    constexpr ResourceId Handle<Kind>::GetResourceIndex() const
    {
        return m_Resource;
    }

    template <ResourceKind Kind>
    constexpr uint32_t Handle<Kind>::GetVersion() const
    {
        return m_Version;
    }

    template <ResourceKind Kind>
    constexpr uint64_t Handle<Kind>::GetEpoch() const
    {
        return m_Epoch;
    }

    template <typename T>
    T* PassContext::GetUserContext() const
    {
        return static_cast<T*>(m_UserContext);
    }

    template <ResourceKind Kind>
    bool PassContext::IsDeclared(Handle<Kind> handle) const
    {
        return IsDeclaredInternal(
            Kind,
            handle.m_Resource,
            handle.m_Version,
            handle.m_Epoch);
    }

    template <ResourceKind Kind, typename T>
    T* PassContext::GetResource(Handle<Kind> handle) const
    {
        return static_cast<T*>(GetResourceInternal(
            Kind,
            handle.m_Resource,
            handle.m_Version,
            handle.m_Epoch));
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::Read(
        Handle<Kind> handle,
        Usage usage,
        ShaderStage stages)
    {
        return Access(handle, AccessMode::Read, usage, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::Write(
        Handle<Kind> handle,
        Usage usage,
        ShaderStage stages)
    {
        return Access(handle, AccessMode::Write, usage, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::ReadWrite(
        Handle<Kind> handle,
        Usage usage,
        ShaderStage stages)
    {
        return Access(handle, AccessMode::ReadWrite, usage, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::ReadSRV(Handle<Kind> handle, ShaderStage stages)
    {
        return Read(handle, Usage::ShaderResource, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::ReadUAV(Handle<Kind> handle, ShaderStage stages)
    {
        return Read(handle, Usage::UnorderedAccess, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::WriteUAV(Handle<Kind> handle, ShaderStage stages)
    {
        return Write(handle, Usage::UnorderedAccess, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::ReadWriteUAV(Handle<Kind> handle, ShaderStage stages)
    {
        return ReadWrite(handle, Usage::UnorderedAccess, stages);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::CopySrc(Handle<Kind> handle)
    {
        return Read(handle, Usage::CopySource);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::CopyDst(Handle<Kind> handle)
    {
        return Write(handle, Usage::CopyDestination);
    }

    template <ResourceKind Kind>
    Handle<Kind> PassBuilder::Access(
        Handle<Kind> handle,
        AccessMode mode,
        Usage usage,
        ShaderStage stages)
    {
        Graph* graph = TryGetGraph();
        if (graph == nullptr)
            return {};

        return Graph::FromRaw<Kind>(
            graph->AccessInternal(
                m_Pass,
                m_Epoch,
                Graph::ToRaw(handle),
                mode,
                usage,
                stages));
    }

    template <ResourceKind Kind>
    void Graph::Export(Handle<Kind> handle, Usage finalUsage)
    {
        ExportInternal(
            Kind,
            handle.m_Resource,
            handle.m_Version,
            handle.m_Epoch,
            finalUsage);
    }

    template <ResourceKind Kind>
    Graph::RawHandle Graph::ToRaw(Handle<Kind> handle)
    {
        return {Kind, handle.m_Resource, handle.m_Version, handle.m_Epoch};
    }

    template <ResourceKind Kind>
    Handle<Kind> Graph::FromRaw(RawHandle handle)
    {
        return Handle<Kind>(handle.Resource, handle.Version, handle.Epoch);
    }

    template <typename PassData, typename Setup, typename ExecuteCallback>
    PassData Graph::AddPass(
        std::string_view name,
        Setup&& setup,
        ExecuteCallback&& execute,
        PassFlags flags)
    {
        using ExecuteType = std::decay_t<ExecuteCallback>;

        static_assert(
            std::is_default_constructible_v<PassData>,
            "Render Graph PassData must be default constructible.");
        static_assert(
            std::is_copy_constructible_v<PassData> && std::is_move_constructible_v<PassData>,
            "Render Graph PassData must be copy and move constructible.");
        static_assert(
            Detail::VoidInvocable<Setup&&, PassBuilder&, PassData&>,
            "Render Graph setup must be callable as void(PassBuilder&, PassData&).");
        static_assert(
            std::is_copy_constructible_v<ExecuteType> && std::is_move_constructible_v<ExecuteType>,
            "Render Graph execute callbacks must be copy and move constructible.");
        static_assert(
            Detail::VoidInvocable<ExecuteType&, const PassData&, PassContext&>,
            "Render Graph execute must be callable as void(const PassData&, PassContext&).");

        PassData data{};
        ExecuteType executeCallback(std::forward<ExecuteCallback>(execute));
        PassBuilder builder = BeginPass(name, flags);
        if (!builder.IsOpen())
            return data;

        std::invoke(std::forward<Setup>(setup), builder, data);
        if (!builder.IsOpen())
            return data;

        PassData result = data;
        builder.SetExecute(
            [data = std::move(data), executeCallback = std::move(executeCallback)](
                PassContext& context) mutable
            {
                std::invoke(executeCallback, std::as_const(data), context);
            });
        return result;
    }
} // namespace RenderGraph

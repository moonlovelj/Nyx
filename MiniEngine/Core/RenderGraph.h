#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

class CommandContext;
class GpuResource;

namespace RenderGraph
{
    constexpr uint32_t kInvalidIndex = UINT32_MAX;

    using PassId = uint32_t;
    using ResourceId = uint32_t;
    // Identifies an aliasable memory range in the compiled physical plan.
    // Logical resources remain distinct native resource objects.
    using MemorySlotId = uint32_t;

    enum class ResourceKind : uint8_t
    {
        Texture,
        Buffer,
    };

    enum class ResourceFlags : uint32_t
    {
        None = 0,
        AllowRenderTarget = 1u << 0,
        AllowDepthStencil = 1u << 1,
        AllowUnorderedAccess = 1u << 2,
    };

    enum class Usage : uint8_t
    {
        Undefined,
        Common,
        ShaderResource,
        UnorderedAccess,
        RenderTarget,
        DepthRead,
        DepthWrite,
        CopySource,
        CopyDestination,
        IndirectArgument,
        Present,
    };

    enum class ShaderStage : uint32_t
    {
        None = 0,
        Vertex = 1u << 0,
        Pixel = 1u << 1,
        Compute = 1u << 2,
        AllGraphics = (1u << 0) | (1u << 1),
        All = (1u << 0) | (1u << 1) | (1u << 2),
    };

    enum class AccessMode : uint8_t
    {
        Read,
        Write,
        ReadWrite,
    };

    enum class PassFlags : uint8_t
    {
        None = 0,
        SideEffect = 1u << 0,
        NeverCull = 1u << 1,
    };

    enum class EdgeKind : uint8_t
    {
        Data,
        WAR,
        WAW,
    };

    enum class BarrierKind : uint8_t
    {
        Aliasing,
        Transition,
        UnorderedAccess,
        FinalTransition,
    };

    enum class DiagnosticSeverity : uint8_t
    {
        Warning,
        Error,
    };

    enum class DiagnosticCode : uint16_t
    {
        None,
        InvalidResourceDesc,
        InvalidHandle,
        InvalidHandleEpoch,
        ResourceKindMismatch,
        StaleVersion,
        UninitializedRead,
        InvalidUsage,
        ConflictingUsage,
        BuilderClosed,
        InvalidBuilderEpoch,
        BuilderStillOpen,
        MissingExecuteCallback,
        DuplicateExecuteCallback,
        InvalidExport,
        ConflictingFinalUsage,
        CycleDetected,
        ExecuteBeforeCompile,
        MutationDuringExecution,
        UndeclaredResourceAccess,
        UnresolvedRuntimeResource,
        DuplicateRuntimeResource,
        RuntimeResourceDescMismatch,
        RuntimeResourceStateMismatch,
        UnsupportedCommandContext,
        BarrierRecordingFailed,
    };

    constexpr ResourceFlags operator|(ResourceFlags lhs, ResourceFlags rhs)
    {
        return static_cast<ResourceFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    constexpr ResourceFlags operator&(ResourceFlags lhs, ResourceFlags rhs)
    {
        return static_cast<ResourceFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
    }

    constexpr ShaderStage operator|(ShaderStage lhs, ShaderStage rhs)
    {
        return static_cast<ShaderStage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    constexpr ShaderStage operator&(ShaderStage lhs, ShaderStage rhs)
    {
        return static_cast<ShaderStage>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
    }

    constexpr PassFlags operator|(PassFlags lhs, PassFlags rhs)
    {
        return static_cast<PassFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    }

    constexpr PassFlags operator&(PassFlags lhs, PassFlags rhs)
    {
        return static_cast<PassFlags>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
    }

    constexpr bool HasAny(ResourceFlags value, ResourceFlags flags)
    {
        return (value & flags) != ResourceFlags::None;
    }

    constexpr bool HasAny(ShaderStage value, ShaderStage stages)
    {
        return (value & stages) != ShaderStage::None;
    }

    constexpr bool HasAny(PassFlags value, PassFlags flags)
    {
        return (value & flags) != PassFlags::None;
    }

    struct TextureDesc
    {
        // v1 models Texture2D and Texture2D arrays only. DepthOrArraySize is the
        // array size; adding 1D/3D is deferred until a concrete pass needs it.
        uint32_t Width = 1;
        uint32_t Height = 1;
        uint16_t DepthOrArraySize = 1;
        uint16_t MipLevels = 1;
        uint16_t SampleCount = 1;
        uint16_t Reserved = 0;
        uint32_t Format = 0;
        ResourceFlags Flags = ResourceFlags::None;

        bool operator==(const TextureDesc&) const = default;
    };

    struct BufferDesc
    {
        uint64_t SizeInBytes = 0;
        uint32_t StrideInBytes = 0;
        ResourceFlags Flags = ResourceFlags::None;

        bool operator==(const BufferDesc&) const = default;
    };

    struct ResourceState
    {
        Usage UsageType = Usage::Undefined;
        ShaderStage Stages = ShaderStage::None;

        bool operator==(const ResourceState&) const = default;
    };

    struct Diagnostic
    {
        DiagnosticSeverity Severity = DiagnosticSeverity::Error;
        DiagnosticCode Code = DiagnosticCode::None;
        std::string Message;
        PassId Pass = kInvalidIndex;
        ResourceId Resource = kInvalidIndex;
    };

    struct Barrier
    {
        // Aliasing uses AliasedResourceBefore -> Resource. A missing before resource
        // maps to a native nullptr -> Resource barrier at a shared slot's first use.
        BarrierKind Kind = BarrierKind::Transition;
        ResourceId Resource = kInvalidIndex;
        uint32_t Version = 0;
        PassId Pass = kInvalidIndex;
        bool AfterPass = false;
        ResourceState Before;
        ResourceState After;
        ResourceId AliasedResourceBefore = kInvalidIndex;
    };

    struct CompileOptions
    {
        bool EnablePassCulling = true;
        bool EnableMemoryAliasing = true;
        bool ExtendTransientLifetimes = false;
    };

    struct CompileResult
    {
        bool Succeeded = false;
        uint32_t LivePassCount = 0;
        uint32_t CulledPassCount = 0;
        uint32_t BarrierCount = 0;
        uint32_t MemorySlotCount = 0;
    };

    class Graph;
    class PassBuilder;
    class PassContext;
    struct DebugSnapshot;

    namespace Detail
    {
        class ExecutionBackend;
        struct GraphTestAccess;

        template <typename Function, typename... Args>
        concept VoidInvocable = requires {
            typename std::invoke_result_t<Function, Args...>;
            requires std::same_as<std::invoke_result_t<Function, Args...>, void>;
        };
    } // namespace Detail

    template <ResourceKind Kind>
    class Handle
    {
      public:
        constexpr Handle();

        constexpr bool IsValid() const;
        constexpr explicit operator bool() const;
        constexpr ResourceId GetResourceIndex() const;
        constexpr uint32_t GetVersion() const;
        constexpr uint64_t GetEpoch() const;

        constexpr bool operator==(const Handle&) const = default;

      private:
        constexpr Handle(ResourceId resource, uint32_t version, uint64_t epoch);

        ResourceId m_Resource = kInvalidIndex;
        uint32_t m_Version = 0;
        uint64_t m_Epoch = 0;

        friend class Graph;
        friend class PassBuilder;
        friend class PassContext;
    };

    using TextureHandle = Handle<ResourceKind::Texture>;
    using BufferHandle = Handle<ResourceKind::Buffer>;

    class PassContext
    {
      public:
        PassContext(const PassContext&) = delete;
        PassContext& operator=(const PassContext&) = delete;
        PassContext(PassContext&&) = delete;
        PassContext& operator=(PassContext&&) = delete;

        PassId GetPassId() const;
        uint32_t GetExecutionIndex() const;
        std::string_view GetPassName() const;
        // Non-null only while Graph::Execute(CommandContext&) is invoking this pass.
        ::CommandContext* GetCommandContext() const noexcept;

        template <typename T>
        T* GetUserContext() const;

        template <ResourceKind Kind>
        bool IsDeclared(Handle<Kind> handle) const;

        template <ResourceKind Kind, typename T = void>
        T* GetResource(Handle<Kind> handle) const;

      private:
        PassContext(
            Graph& graph,
            PassId pass,
            uint32_t executionIndex,
            void* userContext,
            ::CommandContext* commandContext);

        bool IsDeclaredInternal(ResourceKind kind, ResourceId resource, uint32_t version, uint64_t epoch) const;
        void* GetResourceInternal(ResourceKind kind, ResourceId resource, uint32_t version, uint64_t epoch) const;
        Graph* m_Graph = nullptr;
        PassId m_Pass = kInvalidIndex;
        uint32_t m_ExecutionIndex = kInvalidIndex;
        void* m_UserContext = nullptr;
        ::CommandContext* m_CommandContext = nullptr;

        friend class Graph;
    };

    class PassBuilder
    {
      public:
        using ExecuteCallback = std::function<void(PassContext&)>;

        PassBuilder() = default;
        PassBuilder(const PassBuilder&) = delete;
        PassBuilder& operator=(const PassBuilder&) = delete;
        PassBuilder(PassBuilder&& other) noexcept;
        PassBuilder& operator=(PassBuilder&& other) noexcept;
        ~PassBuilder();

        PassId GetPassId() const
        {
            return m_Pass;
        }

        template <ResourceKind Kind>
        Handle<Kind> Read(
            Handle<Kind> handle,
            Usage usage = Usage::ShaderResource,
            ShaderStage stages = ShaderStage::None);

        template <ResourceKind Kind>
        Handle<Kind> Write(
            Handle<Kind> handle,
            Usage usage,
            ShaderStage stages = ShaderStage::None);

        template <ResourceKind Kind>
        Handle<Kind> ReadWrite(
            Handle<Kind> handle,
            Usage usage = Usage::UnorderedAccess,
            ShaderStage stages = ShaderStage::None);

        // Preferred semantic accessors.  They keep common pass setup concise while
        // resolving immediately to the same AccessMode + Usage + ShaderStage records
        // consumed by the compiler.  Shader accesses default to all stages so the
        // legacy D3D12 backend can use the combined pixel/non-pixel read state safely;
        // callers may still provide a narrower stage mask when it is useful.
        // Write* and CopyDst declare that previous contents are not required;
        // choose the corresponding ReadWrite* form when a pass loads and preserves
        // existing contents.  Partial copy-destination updates are not modeled yet.
        template <ResourceKind Kind>
        Handle<Kind> ReadSRV(
            Handle<Kind> handle,
            ShaderStage stages = ShaderStage::All);

        template <ResourceKind Kind>
        Handle<Kind> ReadUAV(
            Handle<Kind> handle,
            ShaderStage stages = ShaderStage::All);

        template <ResourceKind Kind>
        Handle<Kind> WriteUAV(
            Handle<Kind> handle,
            ShaderStage stages = ShaderStage::All);

        template <ResourceKind Kind>
        Handle<Kind> ReadWriteUAV(
            Handle<Kind> handle,
            ShaderStage stages = ShaderStage::All);

        TextureHandle WriteRTV(TextureHandle handle);

        TextureHandle ReadWriteRTV(TextureHandle handle);

        TextureHandle ReadDepth(TextureHandle handle);

        TextureHandle WriteDepth(TextureHandle handle);

        TextureHandle ReadWriteDepth(TextureHandle handle);

        template <ResourceKind Kind>
        Handle<Kind> CopySrc(Handle<Kind> handle);

        template <ResourceKind Kind>
        Handle<Kind> CopyDst(Handle<Kind> handle);

        BufferHandle ReadIndirectArgument(BufferHandle handle);

        void MarkSideEffect();

      private:
        PassBuilder(Graph& graph, PassId pass);

        bool IsOpen() const
        {
            return m_Graph != nullptr && !m_OwnerToken.expired();
        }
        void SetExecute(ExecuteCallback callback);
        void Close();
        Graph* TryGetGraph();
        void Abandon();

        template <ResourceKind Kind>
        Handle<Kind> Access(Handle<Kind> handle, AccessMode mode, Usage usage, ShaderStage stages);

        Graph* m_Graph = nullptr;
        PassId m_Pass = kInvalidIndex;
        uint64_t m_Epoch = 0;
        std::weak_ptr<void> m_OwnerToken;

        friend class Graph;
        friend struct Detail::GraphTestAccess;
    };

    class Graph
    {
      public:
        // Graph construction, compilation, and execution are intentionally
        // single-threaded. PassContext is valid only for the current callback.
        explicit Graph(std::string name = "Render Graph");
        ~Graph();

        Graph(const Graph&) = delete;
        Graph& operator=(const Graph&) = delete;
        Graph(Graph&&) = delete;
        Graph& operator=(Graph&&) = delete;

        TextureHandle CreateTexture(std::string_view name, const TextureDesc& desc);
        BufferHandle CreateBuffer(std::string_view name, const BufferDesc& desc);

        TextureHandle ImportTexture(
            std::string_view name,
            const TextureDesc& desc,
            void* externalResource,
            Usage initialUsage,
            ShaderStage initialStages = ShaderStage::None);

        // Strongly typed import required by GPU execution. The void* overload is
        // retained for backend-independent tools and tests.
        TextureHandle ImportTexture(
            std::string_view name,
            const TextureDesc& desc,
            ::GpuResource& externalResource,
            Usage initialUsage,
            ShaderStage initialStages = ShaderStage::None);

        BufferHandle ImportBuffer(
            std::string_view name,
            const BufferDesc& desc,
            void* externalResource,
            Usage initialUsage,
            ShaderStage initialStages = ShaderStage::None);

        BufferHandle ImportBuffer(
            std::string_view name,
            const BufferDesc& desc,
            ::GpuResource& externalResource,
            Usage initialUsage,
            ShaderStage initialStages = ShaderStage::None);

        // setup runs immediately and may only declare resources or call MarkSideEffect.
        // execute is stored and invoked later with an immutable snapshot of the
        // resulting data. Returning PassData exposes
        // output handles for declarations of later passes. Setup exceptions propagate;
        // Reset the graph before reuse because declaration rollback is not attempted.
        template <typename PassData, typename Setup, typename ExecuteCallback>
        PassData AddPass(
            std::string_view name,
            Setup&& setup,
            ExecuteCallback&& execute,
            PassFlags flags = PassFlags::None);

        template <ResourceKind Kind>
        void Export(Handle<Kind> handle, Usage finalUsage = Usage::Undefined);

        CompileResult Compile(const CompileOptions& options = {});
        // Replays every compiled alias/transition/UAV barrier through CommandContext,
        // flushes each pass batch before its callback, and flushes the epilogue.
        bool Execute(::CommandContext& commandContext, void* userContext = nullptr);
        void Reset();

        bool IsCompiled() const;
        uint64_t GetEpoch() const;
        std::string_view GetName() const;
        const CompileResult& GetCompileResult() const;
        const std::vector<PassId>& GetExecutionOrder() const;
        const std::vector<Barrier>& GetBarriers() const;
        std::vector<Diagnostic> CollectDiagnostics() const;
        std::string_view GetPassName(PassId pass) const;
        bool IsPassLive(PassId pass) const;
        DebugSnapshot CaptureDebugSnapshot() const;

      private:
        struct Impl;
        struct RawHandle
        {
            ResourceKind Kind = ResourceKind::Texture;
            ResourceId Resource = kInvalidIndex;
            uint32_t Version = 0;
            uint64_t Epoch = 0;
        };

        PassBuilder BeginPass(std::string_view name, PassFlags flags);
        RawHandle AccessInternal(
            PassId pass,
            uint64_t builderEpoch,
            RawHandle handle,
            AccessMode mode,
            Usage usage,
            ShaderStage stages);

        void MarkSideEffectInternal(PassId pass, uint64_t builderEpoch);
        void SetExecuteInternal(PassId pass, uint64_t builderEpoch, PassBuilder::ExecuteCallback callback);
        void CloseBuilderInternal(PassId pass, uint64_t builderEpoch);
        void ExportInternal(
            ResourceKind kind,
            ResourceId resource,
            uint32_t version,
            uint64_t epoch,
            Usage finalUsage);

        bool IsDeclaredInternal(PassId pass, RawHandle handle) const;
        void* GetResourceInternal(PassId pass, RawHandle handle);
        bool ExecuteInternal(
            void* userContext,
            ::CommandContext* commandContext,
            Detail::ExecutionBackend* backend);

        template <ResourceKind Kind>
        static RawHandle ToRaw(Handle<Kind> handle);

        template <ResourceKind Kind>
        static Handle<Kind> FromRaw(RawHandle handle);

        std::unique_ptr<Impl> m_Impl;

        friend class PassBuilder;
        friend class PassContext;
        friend struct Detail::GraphTestAccess;
    };

} // namespace RenderGraph

#include "RenderGraph.inl"

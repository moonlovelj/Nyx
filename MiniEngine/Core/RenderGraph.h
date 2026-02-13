#pragma once

#include "GpuResource.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class GraphicsContext;
class ColorBuffer;
class DepthBuffer;

namespace RenderGraph
{
    struct GraphTestAccess;

    struct ResourceHandle
    {
        uint32_t Index = 0xFFFFFFFFu;
        uint32_t Version = 0xFFFFFFFFu;

        bool IsValid() const { return Index != 0xFFFFFFFFu; }
    };

    enum class AccessType : uint8_t
    {
        ReadSrv,
        ReadDepth,
        WriteRenderTarget,
        WriteDepth,
        WriteUav,
        CopySrc,
        CopyDst,
        Present
    };

    struct TextureDesc
    {
        uint32_t Width = 1;
        uint32_t Height = 1;
        uint32_t NumMips = 1;
        DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uint32_t TemporalLayers = 1;
        bool IsDepth = false;
    };

    struct CompileOptions
    {
        bool EnablePassCulling = true;
        bool DumpGraph = true;
        std::string DumpPathPrefix = "RenderGraphDumps/nyx_m0";
    };

    struct ExecuteOptions
    {
        uint32_t FrameIndex = 0;
    };

    struct ResourceLifetimeInfo
    {
        int32_t FirstPass = -1;
        int32_t LastPass = -1;
        bool IsTransient = false;
        bool IsTemporal = false;
    };

    struct PassInfo
    {
        std::string Name;
        bool Culled = false;
        std::vector<ResourceHandle> Reads;
        std::vector<ResourceHandle> Writes;
    };

    class Graph;
    class PassBuilder;

    class PassExecutionContext
    {
    public:
        PassExecutionContext(Graph& graph, GraphicsContext& context, uint32_t frameIndex, uint32_t passIndex)
            : m_Graph(graph), m_Context(context), m_FrameIndex(frameIndex), m_PassIndex(passIndex)
        {
        }

        GraphicsContext& GetGraphicsContext() { return m_Context; }
        uint32_t GetFrameIndex() const { return m_FrameIndex; }
        uint32_t GetPassIndex() const { return m_PassIndex; }

        GpuResource& GetResource(ResourceHandle handle, int32_t temporalLayerOffset = 0);
        ColorBuffer& GetColor(ResourceHandle handle, int32_t temporalLayerOffset = 0);
        DepthBuffer& GetDepth(ResourceHandle handle, int32_t temporalLayerOffset = 0);

    private:
        Graph& m_Graph;
        GraphicsContext& m_Context;
        uint32_t m_FrameIndex;
        uint32_t m_PassIndex;
    };

    class Graph
    {
    public:
        using ExecuteCallback = std::function<void(PassExecutionContext&)>;

        Graph() = default;
        ~Graph();

        Graph(const Graph&) = delete;
        Graph& operator=(const Graph&) = delete;
        Graph(Graph&&) = default;
        Graph& operator=(Graph&&) = default;

        ResourceHandle Import(const std::string& name, GpuResource& resource);
        ResourceHandle CreateTexture(const std::string& name, const TextureDesc& desc);

        void Export(ResourceHandle handle);

        template<class SetupFn, class ExecuteFn>
        void AddPass(const std::string& name, SetupFn&& setup, ExecuteFn&& execute)
        {
            const uint32_t passIndex = BeginPass(name);
            PassBuilder builder(*this, passIndex);
            setup(builder);
            EndPass(passIndex, ExecuteCallback(std::forward<ExecuteFn>(execute)));
        }

        bool Compile(const CompileOptions& options = {});
        void Execute(GraphicsContext& context, const ExecuteOptions& options = {});
        void Reset();

        const std::string& GetLastCompileError() const { return m_LastCompileError; }
        const std::vector<PassInfo>& GetPassInfos() const { return m_PassInfos; }
        const std::vector<ResourceLifetimeInfo>& GetResourceLifetimes() const { return m_ResourceLifetimes; }

        struct BarrierOp
        {
            uint32_t ResourceIndex = 0;
            int32_t TemporalLayerOffset = 0;
            D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        };

        const std::vector<std::vector<BarrierOp>>& GetBarrierBatches() const { return m_BarrierBatches; }

    private:
        friend class PassBuilder;
        friend class PassExecutionContext;
        friend struct GraphTestAccess;

        struct ReadDecl
        {
            ResourceHandle Handle;
            AccessType Access = AccessType::ReadSrv;
            int32_t TemporalLayerOffset = 0;
        };

        struct WriteDecl
        {
            ResourceHandle SourceHandle;
            ResourceHandle TargetHandle;
            AccessType Access = AccessType::WriteUav;
            int32_t TemporalLayerOffset = 0;
        };

        struct PassNode
        {
            std::string Name;
            std::wstring WideName;
            bool SideEffect = false;
            std::vector<ReadDecl> Reads;
            std::vector<WriteDecl> Writes;
            ExecuteCallback Callback;
        };

        struct VersionNode
        {
            int32_t WriterPass = -1;
        };

        struct TransientSlice
        {
            std::unique_ptr<ColorBuffer> Color;
            std::unique_ptr<DepthBuffer> Depth;

            bool IsAllocated() const { return Color != nullptr || Depth != nullptr; }
            GpuResource* GetResource();
            void Destroy();
        };

        struct ResourceNode
        {
            std::string Name;
            bool Imported = false;
            bool IsDepth = false;
            TextureDesc Desc = {};
            GpuResource* ImportedResource = nullptr;
            uint32_t LatestVersion = 0;
            std::vector<VersionNode> Versions;
            std::vector<TransientSlice> TemporalSlices;
        };

        uint32_t BeginPass(const std::string& name);
        void EndPass(uint32_t passIndex, ExecuteCallback&& callback);

        ResourceHandle RegisterRead(uint32_t passIndex, ResourceHandle handle, AccessType access, int32_t temporalLayerOffset);
        ResourceHandle RegisterWrite(uint32_t passIndex, ResourceHandle handle, AccessType access, int32_t temporalLayerOffset);
        void MarkPassSideEffect(uint32_t passIndex);

        bool BuildDependencies(const CompileOptions& options);
        void BuildPassInfos();
        void BuildLifetimes();
        void BuildBarriers();
        void DumpGraph(const CompileOptions& options) const;

        bool ValidateHandle(ResourceHandle handle, uint32_t passIndex, const char* operation, bool requireLatest);
        bool ValidateTemporalOffset(const ResourceNode& resource, int32_t temporalLayerOffset, uint32_t passIndex, const char* operation);

        bool EnsureTransientSliceAllocated(uint32_t resourceIndex, uint32_t frameIndex, int32_t temporalLayerOffset);
        void ReleaseTransientIfNeeded(uint32_t resourceIndex, uint32_t activePassOrdinal, uint32_t frameIndex);
        GpuResource* ResolveResource(uint32_t resourceIndex, uint32_t frameIndex, int32_t temporalLayerOffset);

    private:
        std::vector<ResourceNode> m_Resources;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceHandle> m_ExportedHandles;

        std::vector<std::vector<uint32_t>> m_Edges;
        std::vector<std::vector<uint32_t>> m_ReverseEdges;
        std::vector<uint32_t> m_ActivePasses;
        std::vector<std::vector<BarrierOp>> m_BarrierBatches;
        std::vector<ResourceLifetimeInfo> m_ResourceLifetimes;
        std::vector<PassInfo> m_PassInfos;

        bool m_IsCompiled = false;
        bool m_HasBuilderError = false;
        std::string m_LastCompileError;
    };

    class PassBuilder
    {
    public:
        PassBuilder(Graph& graph, uint32_t passIndex) : m_Graph(graph), m_PassIndex(passIndex) {}

        ResourceHandle Read(ResourceHandle handle, AccessType access = AccessType::ReadSrv, int32_t temporalLayerOffset = 0)
        {
            return m_Graph.RegisterRead(m_PassIndex, handle, access, temporalLayerOffset);
        }

        ResourceHandle Write(ResourceHandle handle, AccessType access = AccessType::WriteUav, int32_t temporalLayerOffset = 0)
        {
            return m_Graph.RegisterWrite(m_PassIndex, handle, access, temporalLayerOffset);
        }

        ResourceHandle CreateTexture(const std::string& name, const TextureDesc& desc)
        {
            return m_Graph.CreateTexture(name, desc);
        }

        ResourceHandle Import(const std::string& name, GpuResource& resource)
        {
            return m_Graph.Import(name, resource);
        }

        void SetSideEffect(bool enabled = true)
        {
            if (enabled)
                m_Graph.MarkPassSideEffect(m_PassIndex);
        }

    private:
        Graph& m_Graph;
        uint32_t m_PassIndex;
    };
} // namespace RenderGraph

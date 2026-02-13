#include "pch.h"
#include "RenderGraph.h"

#include "ColorBuffer.h"
#include "DepthBuffer.h"
#include "CommandContext.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>

namespace RenderGraph
{
    namespace
    {
        bool IsDepthFormat(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                return true;
            default:
                return false;
            }
        }

        bool IsWriteAccess(AccessType access)
        {
            return access == AccessType::WriteRenderTarget
                || access == AccessType::WriteDepth
                || access == AccessType::WriteUav
                || access == AccessType::CopyDst;
        }

        D3D12_RESOURCE_STATES AccessToState(AccessType access)
        {
            switch (access)
            {
            case AccessType::ReadSrv:
                return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case AccessType::ReadDepth:
                return D3D12_RESOURCE_STATE_DEPTH_READ;
            case AccessType::WriteRenderTarget:
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case AccessType::WriteDepth:
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case AccessType::WriteUav:
                return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case AccessType::CopySrc:
                return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case AccessType::CopyDst:
                return D3D12_RESOURCE_STATE_COPY_DEST;
            case AccessType::Present:
                return D3D12_RESOURCE_STATE_PRESENT;
            default:
                return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        std::wstring ToWide(const std::string& str)
        {
            return std::wstring(str.begin(), str.end());
        }

        std::string EscapeJson(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size() + 8);
            for (char c : value)
            {
                switch (c)
                {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += c; break;
                }
            }
            return escaped;
        }

        std::string EscapeDotLabel(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size() + 8);
            for (char c : value)
            {
                switch (c)
                {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                default: escaped += c; break;
                }
            }
            return escaped;
        }

        const char* AccessTypeToString(AccessType access)
        {
            switch (access)
            {
            case AccessType::ReadSrv: return "ReadSrv";
            case AccessType::ReadDepth: return "ReadDepth";
            case AccessType::WriteRenderTarget: return "WriteRenderTarget";
            case AccessType::WriteDepth: return "WriteDepth";
            case AccessType::WriteUav: return "WriteUav";
            case AccessType::CopySrc: return "CopySrc";
            case AccessType::CopyDst: return "CopyDst";
            case AccessType::Present: return "Present";
            default: return "Unknown";
            }
        }
    } // namespace

    GpuResource* Graph::TransientSlice::GetResource()
    {
        if (Color)
            return Color.get();
        if (Depth)
            return Depth.get();
        return nullptr;
    }

    void Graph::TransientSlice::Destroy()
    {
        if (Color)
            Color->Destroy();
        if (Depth)
            Depth->Destroy();
        Color.reset();
        Depth.reset();
    }

    Graph::~Graph()
    {
        Reset();
    }

    ResourceHandle Graph::Import(const std::string& name, GpuResource& resource)
    {
        ResourceNode node;
        node.Name = name;
        node.Imported = true;
        node.ImportedResource = &resource;
        node.IsDepth = dynamic_cast<DepthBuffer*>(&resource) != nullptr;
        node.Desc.IsDepth = node.IsDepth;
        node.Desc.TemporalLayers = 1;
        node.LatestVersion = 0;
        node.Versions.push_back({});

        const uint32_t index = static_cast<uint32_t>(m_Resources.size());
        m_Resources.push_back(std::move(node));
        m_IsCompiled = false;
        return ResourceHandle{ index, 0u };
    }

    ResourceHandle Graph::CreateTexture(const std::string& name, const TextureDesc& desc)
    {
        ResourceNode node;
        node.Name = name;
        node.Imported = false;
        node.IsDepth = desc.IsDepth || IsDepthFormat(desc.Format);
        node.Desc = desc;
        node.Desc.IsDepth = node.IsDepth;
        node.Desc.NumMips = std::max(1u, node.Desc.NumMips);
        node.Desc.TemporalLayers = std::max(1u, node.Desc.TemporalLayers);
        node.LatestVersion = 0;
        node.Versions.push_back({});
        node.TemporalSlices.resize(node.Desc.TemporalLayers);

        const uint32_t index = static_cast<uint32_t>(m_Resources.size());
        m_Resources.push_back(std::move(node));
        m_IsCompiled = false;
        return ResourceHandle{ index, 0u };
    }

    void Graph::Export(ResourceHandle handle)
    {
        m_ExportedHandles.push_back(handle);
        m_IsCompiled = false;
    }

    uint32_t Graph::BeginPass(const std::string& name)
    {
        PassNode pass;
        pass.Name = name;
        pass.WideName = ToWide(name);
        pass.SideEffect = false;
        const uint32_t index = static_cast<uint32_t>(m_Passes.size());
        m_Passes.push_back(std::move(pass));
        m_IsCompiled = false;
        return index;
    }

    void Graph::EndPass(uint32_t passIndex, ExecuteCallback&& callback)
    {
        if (passIndex < m_Passes.size())
        {
            m_Passes[passIndex].Callback = std::move(callback);
        }
    }

    bool Graph::ValidateHandle(ResourceHandle handle, uint32_t passIndex, const char* operation, bool requireLatest)
    {
        if (!handle.IsValid())
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: invalid handle used by pass " + std::to_string(passIndex) + " in operation " + operation;
            return false;
        }

        if (handle.Index >= m_Resources.size())
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: out-of-range handle index in operation " + std::string(operation);
            return false;
        }

        const ResourceNode& resource = m_Resources[handle.Index];
        if (handle.Version >= resource.Versions.size())
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: out-of-range resource version in operation " + std::string(operation);
            return false;
        }

        if (requireLatest && handle.Version != resource.LatestVersion)
        {
            m_HasBuilderError = true;
            m_LastCompileError =
                "RenderGraph: stale handle detected in operation " + std::string(operation) +
                " for resource '" + resource.Name + "', requested version=" + std::to_string(handle.Version) +
                ", latest version=" + std::to_string(resource.LatestVersion);
            return false;
        }

        return true;
    }

    bool Graph::ValidateTemporalOffset(const ResourceNode& resource, int32_t temporalLayerOffset, uint32_t passIndex, const char* operation)
    {
        if (temporalLayerOffset != 0 && resource.Desc.TemporalLayers <= 1)
        {
            m_HasBuilderError = true;
            m_LastCompileError =
                "RenderGraph: temporal offset used on non-temporal resource '" + resource.Name +
                "' in pass " + std::to_string(passIndex) + " operation " + operation;
            return false;
        }
        return true;
    }

    ResourceHandle Graph::RegisterRead(uint32_t passIndex, ResourceHandle handle, AccessType access, int32_t temporalLayerOffset)
    {
        if (!ValidateHandle(handle, passIndex, "Read", true))
            return {};

        if (passIndex >= m_Passes.size())
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: read pass index out of range";
            return {};
        }

        const ResourceNode& resource = m_Resources[handle.Index];
        if (!ValidateTemporalOffset(resource, temporalLayerOffset, passIndex, "Read"))
            return {};

        m_Passes[passIndex].Reads.push_back(ReadDecl{ handle, access, temporalLayerOffset });
        return handle;
    }

    ResourceHandle Graph::RegisterWrite(uint32_t passIndex, ResourceHandle handle, AccessType access, int32_t temporalLayerOffset)
    {
        if (!ValidateHandle(handle, passIndex, "Write", true))
            return {};

        if (!IsWriteAccess(access))
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: write declared with non-write access type";
            return {};
        }

        if (passIndex >= m_Passes.size())
        {
            m_HasBuilderError = true;
            m_LastCompileError = "RenderGraph: write pass index out of range";
            return {};
        }

        ResourceNode& resource = m_Resources[handle.Index];
        if (!ValidateTemporalOffset(resource, temporalLayerOffset, passIndex, "Write"))
            return {};

        const uint32_t newVersion = resource.LatestVersion + 1;
        resource.LatestVersion = newVersion;
        resource.Versions.push_back(VersionNode{ static_cast<int32_t>(passIndex) });

        const ResourceHandle targetHandle{ handle.Index, newVersion };
        m_Passes[passIndex].Writes.push_back(WriteDecl{ handle, targetHandle, access, temporalLayerOffset });
        return targetHandle;
    }

    void Graph::MarkPassSideEffect(uint32_t passIndex)
    {
        if (passIndex < m_Passes.size())
            m_Passes[passIndex].SideEffect = true;
    }

    bool Graph::BuildDependencies(const CompileOptions& options)
    {
        m_Edges.assign(m_Passes.size(), {});
        m_ReverseEdges.assign(m_Passes.size(), {});
        m_ActivePasses.clear();

        std::unordered_set<uint64_t> seenEdges;
        auto tryAddEdge = [&](uint32_t from, uint32_t to)
        {
            if (from == to)
                return;
            const uint64_t key = (uint64_t(from) << 32ull) | uint64_t(to);
            if (!seenEdges.insert(key).second)
                return;
            m_Edges[from].push_back(to);
            m_ReverseEdges[to].push_back(from);
        };

        for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
        {
            const PassNode& pass = m_Passes[passIndex];

            for (const ReadDecl& read : pass.Reads)
            {
                const ResourceNode& resource = m_Resources[read.Handle.Index];
                const int32_t writer = resource.Versions[read.Handle.Version].WriterPass;
                if (writer >= 0)
                    tryAddEdge(static_cast<uint32_t>(writer), passIndex);
            }

            for (const WriteDecl& write : pass.Writes)
            {
                const ResourceNode& resource = m_Resources[write.SourceHandle.Index];
                const int32_t writer = resource.Versions[write.SourceHandle.Version].WriterPass;
                if (writer >= 0)
                    tryAddEdge(static_cast<uint32_t>(writer), passIndex);
            }
        }

        std::vector<bool> livePasses(m_Passes.size(), false);
        std::deque<uint32_t> roots;

        for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
        {
            if (m_Passes[passIndex].SideEffect)
                roots.push_back(passIndex);
        }

        for (ResourceHandle exportHandle : m_ExportedHandles)
        {
            if (!exportHandle.IsValid() || exportHandle.Index >= m_Resources.size())
                continue;
            const ResourceNode& resource = m_Resources[exportHandle.Index];
            if (exportHandle.Version >= resource.Versions.size())
                continue;

            const int32_t writer = resource.Versions[exportHandle.Version].WriterPass;
            if (writer >= 0)
                roots.push_back(static_cast<uint32_t>(writer));
        }

        if (roots.empty())
        {
            for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
                roots.push_back(passIndex);
        }

        while (!roots.empty())
        {
            const uint32_t passIndex = roots.front();
            roots.pop_front();
            if (livePasses[passIndex])
                continue;

            livePasses[passIndex] = true;
            for (uint32_t producer : m_ReverseEdges[passIndex])
                roots.push_back(producer);
        }

        if (!options.EnablePassCulling)
        {
            std::fill(livePasses.begin(), livePasses.end(), true);
        }

        std::vector<uint32_t> indegree(m_Passes.size(), 0);
        for (uint32_t from = 0; from < m_Passes.size(); ++from)
        {
            if (!livePasses[from])
                continue;
            for (uint32_t to : m_Edges[from])
            {
                if (livePasses[to])
                    ++indegree[to];
            }
        }

        std::deque<uint32_t> ready;
        for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
        {
            if (livePasses[passIndex] && indegree[passIndex] == 0)
                ready.push_back(passIndex);
        }

        while (!ready.empty())
        {
            const uint32_t passIndex = ready.front();
            ready.pop_front();
            m_ActivePasses.push_back(passIndex);

            for (uint32_t next : m_Edges[passIndex])
            {
                if (!livePasses[next])
                    continue;
                if (--indegree[next] == 0)
                    ready.push_back(next);
            }
        }

        size_t liveCount = 0;
        for (bool isLive : livePasses)
        {
            if (isLive)
                ++liveCount;
        }

        if (m_ActivePasses.size() != liveCount)
        {
            m_LastCompileError = "RenderGraph: cycle detected while compiling DAG";
            return false;
        }

        return true;
    }

    void Graph::BuildPassInfos()
    {
        std::vector<bool> isActive(m_Passes.size(), false);
        for (uint32_t passIndex : m_ActivePasses)
            isActive[passIndex] = true;

        m_PassInfos.assign(m_Passes.size(), {});
        for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
        {
            const PassNode& pass = m_Passes[passIndex];
            PassInfo info;
            info.Name = pass.Name;
            info.Culled = !isActive[passIndex];

            for (const ReadDecl& read : pass.Reads)
                info.Reads.push_back(read.Handle);
            for (const WriteDecl& write : pass.Writes)
                info.Writes.push_back(write.TargetHandle);

            m_PassInfos[passIndex] = std::move(info);
        }
    }

    void Graph::BuildLifetimes()
    {
        m_ResourceLifetimes.assign(m_Resources.size(), {});

        for (uint32_t ordinal = 0; ordinal < m_ActivePasses.size(); ++ordinal)
        {
            const PassNode& pass = m_Passes[m_ActivePasses[ordinal]];

            auto touch = [&](uint32_t resourceIndex)
            {
                ResourceLifetimeInfo& lifetime = m_ResourceLifetimes[resourceIndex];
                if (lifetime.FirstPass < 0)
                    lifetime.FirstPass = static_cast<int32_t>(ordinal);
                lifetime.LastPass = static_cast<int32_t>(ordinal);
            };

            for (const ReadDecl& read : pass.Reads)
                touch(read.Handle.Index);

            for (const WriteDecl& write : pass.Writes)
                touch(write.TargetHandle.Index);
        }

        for (uint32_t resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
        {
            const ResourceNode& resource = m_Resources[resourceIndex];
            ResourceLifetimeInfo& lifetime = m_ResourceLifetimes[resourceIndex];
            lifetime.IsTransient = !resource.Imported;
            lifetime.IsTemporal = resource.Desc.TemporalLayers > 1;
        }
    }

    void Graph::BuildBarriers()
    {
        m_BarrierBatches.clear();
        m_BarrierBatches.resize(m_ActivePasses.size());

        for (uint32_t ordinal = 0; ordinal < m_ActivePasses.size(); ++ordinal)
        {
            const PassNode& pass = m_Passes[m_ActivePasses[ordinal]];
            std::map<uint64_t, BarrierOp> deduped;

            auto addBarrier = [&](uint32_t resourceIndex, int32_t temporalLayerOffset, AccessType access)
            {
                const D3D12_RESOURCE_STATES state = AccessToState(access);
                const uint64_t key = (uint64_t(resourceIndex) << 32ull) | uint32_t(temporalLayerOffset);
                auto iter = deduped.find(key);
                if (iter == deduped.end())
                {
                    deduped.emplace(key, BarrierOp{ resourceIndex, temporalLayerOffset, state });
                    return;
                }

                if (IsWriteAccess(access))
                {
                    iter->second.State = state;
                }
                else
                {
                    iter->second.State |= state;
                }
            };

            for (const ReadDecl& read : pass.Reads)
                addBarrier(read.Handle.Index, read.TemporalLayerOffset, read.Access);
            for (const WriteDecl& write : pass.Writes)
                addBarrier(write.TargetHandle.Index, write.TemporalLayerOffset, write.Access);

            std::vector<BarrierOp>& batch = m_BarrierBatches[ordinal];
            batch.reserve(deduped.size());
            for (const auto& kv : deduped)
                batch.push_back(kv.second);
        }
    }

    bool Graph::Compile(const CompileOptions& options)
    {
        m_IsCompiled = false;

        if (m_HasBuilderError)
        {
            if (m_LastCompileError.empty())
                m_LastCompileError = "RenderGraph: builder validation failed";
            return false;
        }

        m_LastCompileError.clear();

        if (!BuildDependencies(options))
            return false;

        BuildPassInfos();
        BuildLifetimes();
        BuildBarriers();

        if (options.DumpGraph)
            DumpGraph(options);

        m_IsCompiled = true;
        return true;
    }

    bool Graph::EnsureTransientSliceAllocated(uint32_t resourceIndex, uint32_t frameIndex, int32_t temporalLayerOffset)
    {
        if (resourceIndex >= m_Resources.size())
            return false;

        ResourceNode& resource = m_Resources[resourceIndex];
        if (resource.Imported)
            return true;

        const uint32_t layerCount = std::max(1u, resource.Desc.TemporalLayers);
        if (resource.TemporalSlices.size() != layerCount)
            resource.TemporalSlices.resize(layerCount);

        auto createSlice = [&](uint32_t layerIndex) -> bool
        {
            TransientSlice& slice = resource.TemporalSlices[layerIndex];
            if (slice.IsAllocated())
                return true;

            const std::wstring resourceName = ToWide(resource.Name + "_L" + std::to_string(layerIndex));
            if (resource.IsDepth)
            {
                auto depth = std::make_unique<DepthBuffer>();
                depth->Create(resourceName,
                    resource.Desc.Width,
                    resource.Desc.Height,
                    resource.Desc.Format);
                slice.Depth = std::move(depth);
            }
            else
            {
                auto color = std::make_unique<ColorBuffer>();
                color->Create(resourceName,
                    resource.Desc.Width,
                    resource.Desc.Height,
                    resource.Desc.NumMips,
                    resource.Desc.Format);
                slice.Color = std::move(color);
            }
            return true;
        };

        if (layerCount > 1)
        {
            // Temporal resources keep all slices alive to preserve history.
            for (uint32_t layer = 0; layer < layerCount; ++layer)
            {
                if (!createSlice(layer))
                    return false;
            }
            return true;
        }

        const uint32_t targetLayer = static_cast<uint32_t>((int64_t(frameIndex) + temporalLayerOffset + layerCount * 1024ll) % layerCount);
        return createSlice(targetLayer);
    }

    GpuResource* Graph::ResolveResource(uint32_t resourceIndex, uint32_t frameIndex, int32_t temporalLayerOffset)
    {
        if (resourceIndex >= m_Resources.size())
            return nullptr;

        ResourceNode& resource = m_Resources[resourceIndex];
        if (resource.Imported)
            return resource.ImportedResource;

        const uint32_t layerCount = std::max(1u, resource.Desc.TemporalLayers);
        if (resource.TemporalSlices.empty())
            return nullptr;

        const uint32_t targetLayer = static_cast<uint32_t>((int64_t(frameIndex) + temporalLayerOffset + layerCount * 1024ll) % layerCount);
        return resource.TemporalSlices[targetLayer].GetResource();
    }

    void Graph::ReleaseTransientIfNeeded(uint32_t resourceIndex, uint32_t activePassOrdinal, uint32_t frameIndex)
    {
        (frameIndex);
        if (resourceIndex >= m_Resources.size() || resourceIndex >= m_ResourceLifetimes.size())
            return;

        ResourceNode& resource = m_Resources[resourceIndex];
        const ResourceLifetimeInfo& lifetime = m_ResourceLifetimes[resourceIndex];
        if (!lifetime.IsTransient || lifetime.LastPass != static_cast<int32_t>(activePassOrdinal))
            return;

        // Keep temporal slices alive across frames.
        if (resource.Desc.TemporalLayers > 1)
            return;

        if (!resource.TemporalSlices.empty())
            resource.TemporalSlices[0].Destroy();
    }

    void Graph::Execute(GraphicsContext& context, const ExecuteOptions& options)
    {
        if (!m_IsCompiled)
            return;

        for (uint32_t ordinal = 0; ordinal < m_ActivePasses.size(); ++ordinal)
        {
            const uint32_t passIndex = m_ActivePasses[ordinal];
            const PassNode& pass = m_Passes[passIndex];

            // First-use creation for transient resources.
            for (uint32_t resourceIndex = 0; resourceIndex < m_ResourceLifetimes.size(); ++resourceIndex)
            {
                const ResourceLifetimeInfo& lifetime = m_ResourceLifetimes[resourceIndex];
                if (!lifetime.IsTransient || lifetime.FirstPass != static_cast<int32_t>(ordinal))
                    continue;

                EnsureTransientSliceAllocated(resourceIndex, options.FrameIndex, 0);
            }

            for (const BarrierOp& barrier : m_BarrierBatches[ordinal])
            {
                EnsureTransientSliceAllocated(barrier.ResourceIndex, options.FrameIndex, barrier.TemporalLayerOffset);
                GpuResource* resource = ResolveResource(barrier.ResourceIndex, options.FrameIndex, barrier.TemporalLayerOffset);
                if (resource != nullptr)
                    context.TransitionResource(*resource, barrier.State);
            }
            context.FlushResourceBarriers();

            context.PIXBeginEvent(pass.WideName.c_str());
            if (pass.Callback)
            {
                PassExecutionContext passContext(*this, context, options.FrameIndex, passIndex);
                pass.Callback(passContext);
            }
            context.PIXEndEvent();

            // Last-use release for non-temporal transient resources.
            for (uint32_t resourceIndex = 0; resourceIndex < m_ResourceLifetimes.size(); ++resourceIndex)
                ReleaseTransientIfNeeded(resourceIndex, ordinal, options.FrameIndex);
        }
    }

    void Graph::Reset()
    {
        for (ResourceNode& resource : m_Resources)
        {
            for (TransientSlice& slice : resource.TemporalSlices)
                slice.Destroy();
        }

        m_Resources.clear();
        m_Passes.clear();
        m_ExportedHandles.clear();

        m_Edges.clear();
        m_ReverseEdges.clear();
        m_ActivePasses.clear();
        m_BarrierBatches.clear();
        m_ResourceLifetimes.clear();
        m_PassInfos.clear();

        m_IsCompiled = false;
        m_HasBuilderError = false;
        m_LastCompileError.clear();
    }

    void Graph::DumpGraph(const CompileOptions& options) const
    {
        if (options.DumpPathPrefix.empty())
            return;

        std::filesystem::path prefix(options.DumpPathPrefix);
        const std::filesystem::path parent = prefix.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        std::set<uint32_t> activeSet(m_ActivePasses.begin(), m_ActivePasses.end());

        const std::filesystem::path dotPath = prefix.string() + ".dot";
        std::ofstream dot(dotPath, std::ios::out | std::ios::trunc);
        if (dot.is_open())
        {
            dot << "digraph NyxRenderGraph {\n";
            dot << "  rankdir=LR;\n";
            dot << "  graph [fontname=\"Consolas\", fontsize=10, splines=polyline, overlap=false, ranksep=1.1, nodesep=0.35];\n";
            dot << "  node [fontname=\"Consolas\", fontsize=10, shape=box];\n";
            dot << "  edge [fontname=\"Consolas\", fontsize=9];\n";

            dot << "  subgraph cluster_passes {\n";
            dot << "    label=\"Pass Dependency Graph\";\n";
            dot << "    color=\"lightgrey\";\n";
            for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
            {
                const PassNode& pass = m_Passes[passIndex];
                const bool isActive = activeSet.find(passIndex) != activeSet.end();
                std::vector<std::string> readResourceNames;
                std::vector<std::string> writeResourceNames;
                std::unordered_set<std::string> readSeen;
                std::unordered_set<std::string> writeSeen;

                for (const ReadDecl& read : pass.Reads)
                {
                    if (read.Handle.Index >= m_Resources.size())
                        continue;
                    const std::string& resourceName = m_Resources[read.Handle.Index].Name;
                    if (readSeen.insert(resourceName).second)
                        readResourceNames.push_back(resourceName);
                }

                for (const WriteDecl& write : pass.Writes)
                {
                    if (write.TargetHandle.Index >= m_Resources.size())
                        continue;
                    const std::string& resourceName = m_Resources[write.TargetHandle.Index].Name;
                    if (writeSeen.insert(resourceName).second)
                        writeResourceNames.push_back(resourceName);
                }

                auto joinNames = [](const std::vector<std::string>& names) -> std::string
                {
                    if (names.empty())
                        return "-";

                    std::string joined;
                    for (size_t i = 0; i < names.size(); ++i)
                    {
                        if (i > 0)
                            joined += ", ";
                        joined += names[i];
                    }
                    return joined;
                };

                std::string label = "Pass[" + std::to_string(passIndex) + "]\n" + pass.Name;
                label += "\nstatus=";
                label += isActive ? "active" : "culled";
                label += "\nreads=" + std::to_string(pass.Reads.size());
                label += "\nwrites=" + std::to_string(pass.Writes.size());
                label += "\nreadRes=" + joinNames(readResourceNames);
                label += "\nwriteRes=" + joinNames(writeResourceNames);
                if (pass.SideEffect)
                    label += "\nsideEffect=true";

                dot << "    pass_" << passIndex
                    << " [label=\"" << EscapeDotLabel(label) << "\""
                    << ", style=\"filled,rounded\""
                    << ", fillcolor=\"" << (isActive ? "palegreen" : "lightgray") << "\""
                    << "];\n";
            }
            dot << "  }\n";

            // Pass-to-pass dependency edges.
            for (uint32_t from = 0; from < m_Edges.size(); ++from)
            {
                for (uint32_t to : m_Edges[from])
                {
                    dot << "  pass_" << from << " -> pass_" << to
                        << " [label=\"depends\", color=\"gray45\", style=solid, penwidth=1.2, arrowsize=0.8];\n";
                }
            }

            dot << "}\n";
        }

        const std::filesystem::path jsonPath = prefix.string() + ".json";
        std::ofstream json(jsonPath, std::ios::out | std::ios::trunc);
        if (json.is_open())
        {
            json << "{\n";
            json << "  \"passes\": [\n";
            for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
            {
                const PassNode& pass = m_Passes[passIndex];
                json << "    {\n";
                json << "      \"index\": " << passIndex << ",\n";
                json << "      \"name\": \"" << EscapeJson(pass.Name) << "\",\n";
                json << "      \"active\": " << (activeSet.find(passIndex) != activeSet.end() ? "true" : "false") << ",\n";
                json << "      \"sideEffect\": " << (pass.SideEffect ? "true" : "false") << ",\n";

                json << "      \"reads\": [";
                for (size_t i = 0; i < pass.Reads.size(); ++i)
                {
                    const ReadDecl& read = pass.Reads[i];
                    const ResourceNode& resource = m_Resources[read.Handle.Index];
                    json << "{"
                         << "\"resource\":\"" << EscapeJson(resource.Name) << "\","
                         << "\"version\":" << read.Handle.Version << ","
                         << "\"temporalOffset\":" << read.TemporalLayerOffset
                         << "}";
                    if (i + 1 < pass.Reads.size())
                        json << ",";
                }
                json << "],\n";

                json << "      \"writes\": [";
                for (size_t i = 0; i < pass.Writes.size(); ++i)
                {
                    const WriteDecl& write = pass.Writes[i];
                    const ResourceNode& resource = m_Resources[write.TargetHandle.Index];
                    json << "{"
                         << "\"resource\":\"" << EscapeJson(resource.Name) << "\","
                         << "\"sourceVersion\":" << write.SourceHandle.Version << ","
                         << "\"targetVersion\":" << write.TargetHandle.Version << ","
                         << "\"temporalOffset\":" << write.TemporalLayerOffset
                         << "}";
                    if (i + 1 < pass.Writes.size())
                        json << ",";
                }
                json << "]\n";

                json << "    }";
                if (passIndex + 1 < m_Passes.size())
                    json << ",";
                json << "\n";
            }
            json << "  ],\n";

            json << "  \"resources\": [\n";
            for (uint32_t resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
            {
                const ResourceNode& resource = m_Resources[resourceIndex];
                const ResourceLifetimeInfo& lifetime = m_ResourceLifetimes[resourceIndex];
                json << "    {\n";
                json << "      \"index\": " << resourceIndex << ",\n";
                json << "      \"name\": \"" << EscapeJson(resource.Name) << "\",\n";
                json << "      \"imported\": " << (resource.Imported ? "true" : "false") << ",\n";
                json << "      \"temporalLayers\": " << std::max(1u, resource.Desc.TemporalLayers) << ",\n";
                json << "      \"firstPass\": " << lifetime.FirstPass << ",\n";
                json << "      \"lastPass\": " << lifetime.LastPass << "\n";
                json << "    }";
                if (resourceIndex + 1 < m_Resources.size())
                    json << ",";
                json << "\n";
            }
            json << "  ]\n";
            json << "}\n";
        }
    }

    GpuResource& PassExecutionContext::GetResource(ResourceHandle handle, int32_t temporalLayerOffset)
    {
        GpuResource* resource = m_Graph.ResolveResource(handle.Index, m_FrameIndex, temporalLayerOffset);
        ASSERT(resource != nullptr, "RenderGraph: requested resource is null in pass context");
        return *resource;
    }

    ColorBuffer& PassExecutionContext::GetColor(ResourceHandle handle, int32_t temporalLayerOffset)
    {
        GpuResource& resource = GetResource(handle, temporalLayerOffset);
        auto* color = dynamic_cast<ColorBuffer*>(&resource);
        ASSERT(color != nullptr, "RenderGraph: requested color resource is not a ColorBuffer");
        return *color;
    }

    DepthBuffer& PassExecutionContext::GetDepth(ResourceHandle handle, int32_t temporalLayerOffset)
    {
        GpuResource& resource = GetResource(handle, temporalLayerOffset);
        auto* depth = dynamic_cast<DepthBuffer*>(&resource);
        ASSERT(depth != nullptr, "RenderGraph: requested depth resource is not a DepthBuffer");
        return *depth;
    }
} // namespace RenderGraph

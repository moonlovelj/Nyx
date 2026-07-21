#pragma once

#include "RenderGraph.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RenderGraph::Detail
{
    struct ResourceDescriptor;

    enum class ExternalResourceType : uint8_t
    {
        None,
        Opaque,
        D3D12GpuResource,
    };

    struct ResourceResolveRequest
    {
        ResourceId LogicalResourceId;
        MemorySlotId MemorySlot;
        const ResourceDescriptor& Descriptor;
        uint32_t RequiredUsageMask;
        bool Imported;
        void* ExternalResource;
        ExternalResourceType ExternalType;
        ResourceState DeclaredInitialState;
    };

    struct ResourceResolveResult
    {
        void* RuntimeResource = nullptr;
        const void* NativeResourceIdentity = nullptr;
    };

    // Runtime seam shared by the D3D12 adapter and deterministic unit tests.
    // The graph owns scheduling and batching; a sink resolves/validates runtime
    // resources, records one barrier, and flushes the current batch.
    class ExecutionBackend
    {
      public:
        virtual ~ExecutionBackend() = default;

        virtual DiagnosticCode ValidateExecution(std::string& message) const = 0;

        virtual DiagnosticCode ResolveResource(
            const ResourceResolveRequest& request,
            ResourceResolveResult& result,
            std::string& message) = 0;

        virtual DiagnosticCode RecordBarrier(
            const Barrier& barrier,
            void* beforeResource,
            void* afterResource,
            std::string& message) = 0;

        virtual void FlushBarrierBatch() = 0;
    };

    struct ResourceDescriptor
    {
        ResourceKind Kind = ResourceKind::Texture;
        TextureDesc Texture;
        BufferDesc Buffer;

        bool operator==(const ResourceDescriptor& rhs) const
        {
            if (Kind != rhs.Kind)
                return false;
            return Kind == ResourceKind::Texture ? Texture == rhs.Texture : Buffer == rhs.Buffer;
        }
    };

    inline ResourceDescriptor MakeDescriptor(const TextureDesc& desc)
    {
        ResourceDescriptor result;
        result.Kind = ResourceKind::Texture;
        result.Texture = desc;
        return result;
    }

    inline ResourceDescriptor MakeDescriptor(const BufferDesc& desc)
    {
        ResourceDescriptor result;
        result.Kind = ResourceKind::Buffer;
        result.Buffer = desc;
        return result;
    }

    constexpr bool Reads(AccessMode mode)
    {
        return mode != AccessMode::Write;
    }

    constexpr bool Writes(AccessMode mode)
    {
        return mode != AccessMode::Read;
    }

    struct UsageTraits
    {
        uint8_t AllowedKinds = 0;
        uint8_t AllowedAccessModes = 0;
        ResourceFlags RequiredFlags = ResourceFlags::None;
        bool UsesShaderStages = false;
    };

    constexpr uint8_t kTextureKind = 1u << 0;
    constexpr uint8_t kBufferKind = 1u << 1;
    constexpr uint8_t kReadMode = 1u << 0;
    constexpr uint8_t kWriteMode = 1u << 1;
    constexpr uint8_t kReadWriteMode = 1u << 2;

    constexpr std::array<UsageTraits, 11> kUsageTraits = {{
        {kTextureKind | kBufferKind, 0, ResourceFlags::None, false},
        {kTextureKind | kBufferKind, 0, ResourceFlags::None, false},
        {kTextureKind | kBufferKind, kReadMode, ResourceFlags::None, true},
        {kTextureKind | kBufferKind, kReadMode | kWriteMode | kReadWriteMode,
         ResourceFlags::AllowUnorderedAccess, true},
        {kTextureKind, kWriteMode | kReadWriteMode, ResourceFlags::AllowRenderTarget, false},
        {kTextureKind, kReadMode, ResourceFlags::AllowDepthStencil, false},
        {kTextureKind, kWriteMode | kReadWriteMode, ResourceFlags::AllowDepthStencil, false},
        {kTextureKind | kBufferKind, kReadMode, ResourceFlags::None, false},
        {kTextureKind | kBufferKind, kWriteMode, ResourceFlags::None, false},
        {kBufferKind, kReadMode, ResourceFlags::None, false},
        {kTextureKind, 0, ResourceFlags::None, false},
    }};

    constexpr const UsageTraits* GetUsageTraits(Usage usage)
    {
        const size_t index = static_cast<size_t>(usage);
        return index < kUsageTraits.size() ? &kUsageTraits[index] : nullptr;
    }

    constexpr bool IsShaderUsage(Usage usage)
    {
        const UsageTraits* traits = GetUsageTraits(usage);
        return traits != nullptr && traits->UsesShaderStages;
    }

    inline ResourceState MakeCanonicalResourceState(
        Usage usage,
        ShaderStage stages = ShaderStage::None)
    {
        if (IsShaderUsage(usage) && stages == ShaderStage::None)
            stages = ShaderStage::All;
        if (!IsShaderUsage(usage))
            stages = ShaderStage::None;
        return {usage, stages};
    }

    inline bool RequiresTransition(const ResourceState& before, const ResourceState& after)
    {
        if (before.UsageType != after.UsageType)
            return true;
        if (before.UsageType == Usage::UnorderedAccess)
            return false;
        return before.Stages != after.Stages;
    }

    inline void AddDiagnostic(
        std::vector<Diagnostic>& diagnostics,
        DiagnosticCode code,
        std::string message,
        PassId pass = kInvalidIndex,
        ResourceId resource = kInvalidIndex,
        DiagnosticSeverity severity = DiagnosticSeverity::Error)
    {
        diagnostics.push_back({severity, code, std::move(message), pass, resource});
    }

    inline bool HasErrors(const std::vector<Diagnostic>& diagnostics)
    {
        return std::any_of(
            diagnostics.begin(),
            diagnostics.end(),
            [](const Diagnostic& diagnostic)
            {
                return diagnostic.Severity == DiagnosticSeverity::Error;
            });
    }

    inline bool IsValidDescriptor(const ResourceDescriptor& desc)
    {
        if (desc.Kind == ResourceKind::Texture)
        {
            return desc.Texture.Width > 0 && desc.Texture.Height > 0 &&
                   desc.Texture.DepthOrArraySize > 0 && desc.Texture.MipLevels > 0 &&
                   desc.Texture.SampleCount > 0;
        }

        return desc.Buffer.SizeInBytes > 0 &&
               (desc.Buffer.StrideInBytes == 0 || desc.Buffer.StrideInBytes <= desc.Buffer.SizeInBytes);
    }

    inline ResourceFlags GetFlags(const ResourceDescriptor& desc)
    {
        return desc.Kind == ResourceKind::Texture ? desc.Texture.Flags : desc.Buffer.Flags;
    }

    inline bool IsUsageValidForKind(const ResourceDescriptor& desc, Usage usage)
    {
        const UsageTraits* traits = GetUsageTraits(usage);
        if (traits == nullptr)
            return false;

        const uint8_t kind = desc.Kind == ResourceKind::Texture ? kTextureKind : kBufferKind;
        if ((traits->AllowedKinds & kind) == 0)
            return false;
        return traits->RequiredFlags == ResourceFlags::None ||
               HasAny(GetFlags(desc), traits->RequiredFlags);
    }

    inline bool IsAccessUsageValid(
        const ResourceDescriptor& desc,
        AccessMode mode,
        Usage usage)
    {
        const UsageTraits* traits = GetUsageTraits(usage);
        if (traits == nullptr || !IsUsageValidForKind(desc, usage))
            return false;

        const uint8_t modeBit = mode == AccessMode::Read
                                    ? kReadMode
                                    : (mode == AccessMode::Write ? kWriteMode : kReadWriteMode);
        return (traits->AllowedAccessModes & modeBit) != 0;
    }

    struct VersionDef
    {
        PassId Producer = kInvalidIndex;
        std::vector<PassId> Readers;
    };

    struct ResourceDef
    {
        std::string Name;
        ResourceDescriptor Desc;
        bool Imported = false;
        void* ExternalResource = nullptr;
        ExternalResourceType ExternalType = ExternalResourceType::None;
        ResourceState InitialState;
        std::vector<VersionDef> Versions;

        uint32_t GetLatestVersion() const
        {
            return static_cast<uint32_t>(Versions.size() - 1);
        }

        bool IsInitialized(uint32_t version) const
        {
            return version < Versions.size() &&
                   ((version == 0 && Imported) || Versions[version].Producer != kInvalidIndex);
        }
    };

    struct AccessRecord
    {
        ResourceId Resource = kInvalidIndex;
        uint32_t InputVersion = 0;
        uint32_t OutputVersion = 0;
        AccessMode Mode = AccessMode::Read;
        ResourceState State;
    };

    struct PassDef
    {
        std::string Name;
        PassFlags Flags = PassFlags::None;
        std::vector<AccessRecord> Accesses;
        PassBuilder::ExecuteCallback Execute;
        bool BuilderOpen = true;
    };

    struct ExportDef
    {
        ResourceId Resource = kInvalidIndex;
        uint32_t Version = 0;
        Usage FinalUsage = Usage::Undefined;
    };

    struct EdgeRecord
    {
        PassId From = kInvalidIndex;
        PassId To = kInvalidIndex;
        EdgeKind Kind = EdgeKind::Data;
        ResourceId Resource = kInvalidIndex;
        uint32_t Version = 0;

        bool operator==(const EdgeRecord&) const = default;
    };

    // Mutable, per-frame authoring IR.  It owns declarations and callbacks only;
    // compiler-derived state must live in CompiledPlan.
    struct GraphDefinition
    {
        std::vector<ResourceDef> Resources;
        std::vector<PassDef> Passes;
        std::vector<ExportDef> Exports;

        void Clear()
        {
            Resources.clear();
            Passes.clear();
            Exports.clear();
        }
    };

    struct Lifetime
    {
        uint32_t First = kInvalidIndex;
        uint32_t Last = kInvalidIndex;

        void Include(uint32_t point)
        {
            First = First == kInvalidIndex ? point : (std::min)(First, point);
            Last = Last == kInvalidIndex ? point : (std::max)(Last, point);
        }

        void ExtendTo(uint32_t point)
        {
            if (First != kInvalidIndex)
                Last = point;
        }

        bool IsEmpty() const
        {
            return First == kInvalidIndex;
        }
    };

    struct PassPlan
    {
        bool Live = false;
        bool Root = false;
        uint32_t ExecutionIndex = kInvalidIndex;
    };

    struct ResourcePlan
    {
        Lifetime LifetimeRange;
        std::vector<Lifetime> VersionLifetimes;
        PassId LastAccessPass = kInvalidIndex;
        MemorySlotId MemorySlot = kInvalidIndex;
    };

    struct MemoryUse
    {
        ResourceId Resource = kInvalidIndex;
        Lifetime LifetimeRange;
    };

    // One allocation shared by distinct native resources whose lifetimes do not
    // overlap. SlotUse order is also the required alias-barrier order.
    struct MemorySlotPlan
    {
        uint32_t LastUse = kInvalidIndex;
        std::vector<MemoryUse> Uses;
    };

    // Indices into CompiledPlan::Barriers, grouped for a backend executor without
    // duplicating the public, deterministic flat barrier list.
    struct BarrierSchedule
    {
        std::vector<std::vector<uint32_t>> BeforePass;
        std::vector<uint32_t> Epilogue;

        void Clear()
        {
            BeforePass.clear();
            Epilogue.clear();
        }
    };

    // Derived execution data.  The definition remains untouched during compilation,
    // so invalidation is a single Plan.Clear() instead of resetting every node.
    struct CompiledPlan
    {
        std::vector<PassPlan> Passes;
        std::vector<ResourcePlan> Resources;
        std::vector<MemorySlotPlan> MemorySlots;
        std::vector<EdgeRecord> Edges;
        std::vector<PassId> ExecutionOrder;
        std::vector<Barrier> Barriers;
        BarrierSchedule BarrierBatches;

        void Clear()
        {
            Passes.clear();
            Resources.clear();
            MemorySlots.clear();
            Edges.clear();
            ExecutionOrder.clear();
            Barriers.clear();
            BarrierBatches.Clear();
        }
    };

    // A failed compile intentionally keeps its partial plan for diagnostics and the
    // graph preview.  Result.Succeeded alone determines whether Execute is allowed.
    struct CompileArtifact
    {
        bool Attempted = false;
        CompileOptions Options;
        CompileResult Result;
        std::vector<Diagnostic> Diagnostics;
        CompiledPlan Plan;

        void Invalidate()
        {
            Attempted = false;
            Result = {};
            Diagnostics.clear();
            Plan.Clear();
        }

        void ResetForNewDefinition()
        {
            Invalidate();
            Options = {};
        }
    };

    void CompileGraph(
        const GraphDefinition& definition,
        uint32_t openBuilderCount,
        const std::vector<Diagnostic>& buildDiagnostics,
        const CompileOptions& options,
        CompileArtifact& artifact);

} // namespace RenderGraph::Detail

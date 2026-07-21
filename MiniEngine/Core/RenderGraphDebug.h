#pragma once

#include "RenderGraph.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RenderGraph
{
    enum class CompileState : uint8_t
    {
        Uncompiled,
        Succeeded,
        Failed,
    };

    struct DebugAccess
    {
        ResourceId Resource = kInvalidIndex;
        uint32_t InputVersion = 0;
        uint32_t OutputVersion = 0;
        AccessMode Mode = AccessMode::Read;
        ResourceState State;
    };

    struct DebugPass
    {
        PassId Id = kInvalidIndex;
        std::string Name;
        uint32_t InsertionIndex = kInvalidIndex;
        uint32_t ExecutionIndex = kInvalidIndex;
        PassFlags Flags = PassFlags::None;
        bool Live = false;
        bool Root = false;
        std::vector<DebugAccess> Accesses;
    };

    struct DebugResourceVersion
    {
        ResourceId Resource = kInvalidIndex;
        uint32_t Version = 0;
        std::string Name;
        ResourceKind Kind = ResourceKind::Texture;
        bool Imported = false;
        bool Exported = false;
        bool Initialized = false;
        PassId Producer = kInvalidIndex;
        std::vector<PassId> Readers;
        uint32_t FirstUse = kInvalidIndex;
        uint32_t LastUse = kInvalidIndex;
        MemorySlotId MemorySlot = kInvalidIndex;
    };

    struct DebugEdge
    {
        PassId From = kInvalidIndex;
        PassId To = kInvalidIndex;
        EdgeKind Kind = EdgeKind::Data;
        ResourceId Resource = kInvalidIndex;
        uint32_t Version = 0;
    };

    struct DebugSnapshot
    {
        uint32_t SchemaVersion = 2;
        std::string GraphName;
        uint64_t Epoch = 0;
        CompileState State = CompileState::Uncompiled;
        CompileOptions Options;
        CompileResult Result;
        std::vector<DebugPass> Passes;
        std::vector<DebugResourceVersion> ResourceVersions;
        std::vector<DebugEdge> Edges;
        std::vector<Barrier> Barriers;
        std::vector<Diagnostic> Diagnostics;
    };

    namespace Debug
    {
        std::string ToHtml(const DebugSnapshot& snapshot);

        bool WriteHtml(
            const DebugSnapshot& snapshot,
            const std::filesystem::path& outputPath,
            std::string* errorMessage = nullptr);
    } // namespace Debug
} // namespace RenderGraph

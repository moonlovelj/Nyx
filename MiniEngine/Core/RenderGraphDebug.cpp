#include "pch.h"
#include "RenderGraphDebug.h"
#include "RenderGraphViewerTemplate.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace RenderGraph::Debug
{
    namespace
    {
        const char* ToString(ResourceKind value)
        {
            return value == ResourceKind::Texture ? "Texture" : "Buffer";
        }

        const char* ToString(Usage value)
        {
            switch (value)
            {
            case Usage::Undefined:
                return "Undefined";
            case Usage::Common:
                return "Common";
            case Usage::ShaderResource:
                return "ShaderResource";
            case Usage::UnorderedAccess:
                return "UnorderedAccess";
            case Usage::RenderTarget:
                return "RenderTarget";
            case Usage::DepthRead:
                return "DepthRead";
            case Usage::DepthWrite:
                return "DepthWrite";
            case Usage::CopySource:
                return "CopySource";
            case Usage::CopyDestination:
                return "CopyDestination";
            case Usage::IndirectArgument:
                return "IndirectArgument";
            case Usage::Present:
                return "Present";
            }
            return "Unknown";
        }

        std::string ToString(ShaderStage value)
        {
            if (value == ShaderStage::None)
                return "None";
            std::string result;
            auto append = [&](const char* name)
            {
                if (!result.empty())
                    result += "|";
                result += name;
            };
            if (HasAny(value, ShaderStage::Vertex))
                append("Vertex");
            if (HasAny(value, ShaderStage::Pixel))
                append("Pixel");
            if (HasAny(value, ShaderStage::Compute))
                append("Compute");
            return result;
        }

        const char* ToString(AccessMode value)
        {
            switch (value)
            {
            case AccessMode::Read:
                return "Read";
            case AccessMode::Write:
                return "Write";
            case AccessMode::ReadWrite:
                return "ReadWrite";
            }
            return "Unknown";
        }

        const char* ToString(EdgeKind value)
        {
            switch (value)
            {
            case EdgeKind::Data:
                return "Data";
            case EdgeKind::WAR:
                return "WAR";
            case EdgeKind::WAW:
                return "WAW";
            }
            return "Unknown";
        }

        const char* ToString(BarrierKind value)
        {
            switch (value)
            {
            case BarrierKind::Aliasing:
                return "Aliasing";
            case BarrierKind::Transition:
                return "Transition";
            case BarrierKind::UnorderedAccess:
                return "UnorderedAccess";
            case BarrierKind::FinalTransition:
                return "FinalTransition";
            }
            return "Unknown";
        }

        const char* ToString(CompileState value)
        {
            switch (value)
            {
            case CompileState::Uncompiled:
                return "Uncompiled";
            case CompileState::Succeeded:
                return "Succeeded";
            case CompileState::Failed:
                return "Failed";
            }
            return "Unknown";
        }

        const char* ToString(DiagnosticSeverity value)
        {
            return value == DiagnosticSeverity::Error ? "Error" : "Warning";
        }

        const char* ToString(DiagnosticCode value)
        {
            switch (value)
            {
            case DiagnosticCode::None:
                return "None";
            case DiagnosticCode::InvalidResourceDesc:
                return "InvalidResourceDesc";
            case DiagnosticCode::InvalidHandle:
                return "InvalidHandle";
            case DiagnosticCode::InvalidHandleEpoch:
                return "InvalidHandleEpoch";
            case DiagnosticCode::ResourceKindMismatch:
                return "ResourceKindMismatch";
            case DiagnosticCode::StaleVersion:
                return "StaleVersion";
            case DiagnosticCode::UninitializedRead:
                return "UninitializedRead";
            case DiagnosticCode::InvalidUsage:
                return "InvalidUsage";
            case DiagnosticCode::ConflictingUsage:
                return "ConflictingUsage";
            case DiagnosticCode::BuilderClosed:
                return "BuilderClosed";
            case DiagnosticCode::InvalidBuilderEpoch:
                return "InvalidBuilderEpoch";
            case DiagnosticCode::BuilderStillOpen:
                return "BuilderStillOpen";
            case DiagnosticCode::MissingExecuteCallback:
                return "MissingExecuteCallback";
            case DiagnosticCode::DuplicateExecuteCallback:
                return "DuplicateExecuteCallback";
            case DiagnosticCode::InvalidExport:
                return "InvalidExport";
            case DiagnosticCode::ConflictingFinalUsage:
                return "ConflictingFinalUsage";
            case DiagnosticCode::CycleDetected:
                return "CycleDetected";
            case DiagnosticCode::ExecuteBeforeCompile:
                return "ExecuteBeforeCompile";
            case DiagnosticCode::MutationDuringExecution:
                return "MutationDuringExecution";
            case DiagnosticCode::UndeclaredResourceAccess:
                return "UndeclaredResourceAccess";
            case DiagnosticCode::UnresolvedRuntimeResource:
                return "UnresolvedRuntimeResource";
            case DiagnosticCode::DuplicateRuntimeResource:
                return "DuplicateRuntimeResource";
            case DiagnosticCode::RuntimeResourceDescMismatch:
                return "RuntimeResourceDescMismatch";
            case DiagnosticCode::RuntimeResourceStateMismatch:
                return "RuntimeResourceStateMismatch";
            case DiagnosticCode::UnsupportedCommandContext:
                return "UnsupportedCommandContext";
            case DiagnosticCode::BarrierRecordingFailed:
                return "BarrierRecordingFailed";
            }
            return "Unknown";
        }

        std::string PassFlagsToString(PassFlags flags)
        {
            std::string result;
            if (HasAny(flags, PassFlags::SideEffect))
                result = "SideEffect";
            if (HasAny(flags, PassFlags::NeverCull))
            {
                if (!result.empty())
                    result += "|";
                result += "NeverCull";
            }
            return result.empty() ? "None" : result;
        }

        std::string EscapeJson(std::string_view value)
        {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '"':
                    stream << "\\\"";
                    break;
                case '\\':
                    stream << "\\\\";
                    break;
                case '\b':
                    stream << "\\b";
                    break;
                case '\f':
                    stream << "\\f";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    if (character < 0x20)
                    {
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<uint32_t>(character) << std::dec;
                    }
                    else
                    {
                        stream << static_cast<char>(character);
                    }
                    break;
                }
            }
            return stream.str();
        }

        std::string EscapeHtmlJson(std::string json)
        {
            std::string result;
            result.reserve(json.size());
            for (char character : json)
            {
                switch (character)
                {
                case '<':
                    result += "\\u003c";
                    break;
                case '>':
                    result += "\\u003e";
                    break;
                case '&':
                    result += "\\u0026";
                    break;
                default:
                    result += character;
                    break;
                }
            }
            return result;
        }

        void WriteNullableIndex(std::ostringstream& stream, uint32_t value)
        {
            if (value == kInvalidIndex)
                stream << "null";
            else
                stream << value;
        }

        void WriteState(std::ostringstream& stream, const ResourceState& state)
        {
            stream << "{\"usage\":\"" << ToString(state.UsageType)
                   << "\",\"stages\":\"" << EscapeJson(ToString(state.Stages)) << "\"}";
        }

        bool WriteTextFile(
            const std::filesystem::path& path,
            const std::string& contents,
            std::string* errorMessage)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                if (errorMessage != nullptr)
                    *errorMessage = "Unable to open '" + path.string() + "' for writing.";
                return false;
            }
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!file)
            {
                if (errorMessage != nullptr)
                    *errorMessage = "Unable to write '" + path.string() + "'.";
                return false;
            }

            file.flush();
            if (!file)
            {
                if (errorMessage != nullptr)
                    *errorMessage = "Unable to flush '" + path.string() + "'.";
                return false;
            }

            file.close();
            if (file.fail())
            {
                if (errorMessage != nullptr)
                    *errorMessage = "Unable to close '" + path.string() + "'.";
                return false;
            }
            return true;
        }

        std::string SerializeSnapshot(const DebugSnapshot& snapshot)
        {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << "{";
            stream << "\"schemaVersion\":" << snapshot.SchemaVersion;
            stream << ",\"graphName\":\"" << EscapeJson(snapshot.GraphName) << "\"";
            stream << ",\"epoch\":" << snapshot.Epoch;
            stream << ",\"compileState\":\"" << ToString(snapshot.State) << "\"";
            stream << ",\"options\":{";
            stream << "\"passCulling\":" << (snapshot.Options.EnablePassCulling ? "true" : "false");
            stream << ",\"memoryAliasing\":" << (snapshot.Options.EnableMemoryAliasing ? "true" : "false");
            stream << ",\"extendTransientLifetimes\":"
                   << (snapshot.Options.ExtendTransientLifetimes ? "true" : "false") << "}";
            stream << ",\"result\":{";
            stream << "\"succeeded\":" << (snapshot.Result.Succeeded ? "true" : "false");
            stream << ",\"livePassCount\":" << snapshot.Result.LivePassCount;
            stream << ",\"culledPassCount\":" << snapshot.Result.CulledPassCount;
            stream << ",\"barrierCount\":" << snapshot.Result.BarrierCount;
            stream << ",\"memorySlotCount\":" << snapshot.Result.MemorySlotCount << "}";

            stream << ",\"passes\":[";
            for (size_t passIndex = 0; passIndex < snapshot.Passes.size(); ++passIndex)
            {
                if (passIndex != 0)
                    stream << ",";
                const DebugPass& pass = snapshot.Passes[passIndex];
                stream << "{\"id\":" << pass.Id;
                stream << ",\"name\":\"" << EscapeJson(pass.Name) << "\"";
                stream << ",\"insertionIndex\":" << pass.InsertionIndex;
                stream << ",\"executionIndex\":";
                WriteNullableIndex(stream, pass.ExecutionIndex);
                stream << ",\"flags\":\"" << PassFlagsToString(pass.Flags) << "\"";
                stream << ",\"live\":" << (pass.Live ? "true" : "false");
                stream << ",\"root\":" << (pass.Root ? "true" : "false");
                stream << ",\"accesses\":[";
                for (size_t accessIndex = 0; accessIndex < pass.Accesses.size(); ++accessIndex)
                {
                    if (accessIndex != 0)
                        stream << ",";
                    const DebugAccess& access = pass.Accesses[accessIndex];
                    stream << "{\"resource\":" << access.Resource;
                    stream << ",\"inputVersion\":" << access.InputVersion;
                    stream << ",\"outputVersion\":" << access.OutputVersion;
                    stream << ",\"mode\":\"" << ToString(access.Mode) << "\"";
                    stream << ",\"state\":";
                    WriteState(stream, access.State);
                    stream << "}";
                }
                stream << "]}";
            }
            stream << "]";

            stream << ",\"resources\":[";
            for (size_t resourceIndex = 0;
                 resourceIndex < snapshot.ResourceVersions.size();
                 ++resourceIndex)
            {
                if (resourceIndex != 0)
                    stream << ",";
                const DebugResourceVersion& resource =
                    snapshot.ResourceVersions[resourceIndex];
                stream << "{\"resource\":" << resource.Resource;
                stream << ",\"version\":" << resource.Version;
                stream << ",\"name\":\"" << EscapeJson(resource.Name) << "\"";
                stream << ",\"kind\":\"" << ToString(resource.Kind) << "\"";
                stream << ",\"imported\":" << (resource.Imported ? "true" : "false");
                stream << ",\"exported\":" << (resource.Exported ? "true" : "false");
                stream << ",\"initialized\":" << (resource.Initialized ? "true" : "false");
                stream << ",\"producer\":";
                WriteNullableIndex(stream, resource.Producer);
                stream << ",\"readers\":[";
                for (size_t readerIndex = 0; readerIndex < resource.Readers.size(); ++readerIndex)
                {
                    if (readerIndex != 0)
                        stream << ",";
                    stream << resource.Readers[readerIndex];
                }
                stream << "]";
                stream << ",\"firstUse\":";
                WriteNullableIndex(stream, resource.FirstUse);
                stream << ",\"lastUse\":";
                WriteNullableIndex(stream, resource.LastUse);
                stream << ",\"memorySlot\":";
                WriteNullableIndex(stream, resource.MemorySlot);
                stream << "}";
            }
            stream << "]";

            stream << ",\"edges\":[";
            for (size_t edgeIndex = 0; edgeIndex < snapshot.Edges.size(); ++edgeIndex)
            {
                if (edgeIndex != 0)
                    stream << ",";
                const DebugEdge& edge = snapshot.Edges[edgeIndex];
                stream << "{\"from\":" << edge.From << ",\"to\":" << edge.To;
                stream << ",\"kind\":\"" << ToString(edge.Kind) << "\"";
                stream << ",\"resource\":" << edge.Resource << ",\"version\":" << edge.Version << "}";
            }
            stream << "]";

            stream << ",\"barriers\":[";
            for (size_t barrierIndex = 0; barrierIndex < snapshot.Barriers.size(); ++barrierIndex)
            {
                if (barrierIndex != 0)
                    stream << ",";
                const Barrier& barrier = snapshot.Barriers[barrierIndex];
                stream << "{\"kind\":\"" << ToString(barrier.Kind) << "\"";
                stream << ",\"resource\":" << barrier.Resource << ",\"version\":" << barrier.Version;
                stream << ",\"aliasedResourceBefore\":";
                WriteNullableIndex(stream, barrier.AliasedResourceBefore);
                stream << ",\"pass\":";
                WriteNullableIndex(stream, barrier.Pass);
                stream << ",\"afterPass\":" << (barrier.AfterPass ? "true" : "false");
                stream << ",\"before\":";
                WriteState(stream, barrier.Before);
                stream << ",\"after\":";
                WriteState(stream, barrier.After);
                stream << "}";
            }
            stream << "]";

            stream << ",\"diagnostics\":[";
            for (size_t diagnosticIndex = 0; diagnosticIndex < snapshot.Diagnostics.size(); ++diagnosticIndex)
            {
                if (diagnosticIndex != 0)
                    stream << ",";
                const Diagnostic& diagnostic = snapshot.Diagnostics[diagnosticIndex];
                stream << "{\"severity\":\"" << ToString(diagnostic.Severity) << "\"";
                stream << ",\"code\":\"" << ToString(diagnostic.Code) << "\"";
                stream << ",\"message\":\"" << EscapeJson(diagnostic.Message) << "\"";
                stream << ",\"pass\":";
                WriteNullableIndex(stream, diagnostic.Pass);
                stream << ",\"resource\":";
                WriteNullableIndex(stream, diagnostic.Resource);
                stream << "}";
            }
            stream << "]}";
            return stream.str();
        }
    } // namespace

    std::string ToHtml(const DebugSnapshot& snapshot)
    {
        const std::string json = EscapeHtmlJson(SerializeSnapshot(snapshot));
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << Viewer::kHtmlPrefix;
        stream << json;
        stream << Viewer::kHtmlSuffix;
        return stream.str();
    }

    bool WriteHtml(
        const DebugSnapshot& snapshot,
        const std::filesystem::path& outputPath,
        std::string* errorMessage)
    {
        try
        {
            if (errorMessage != nullptr)
                errorMessage->clear();

            const std::filesystem::path parent = outputPath.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);

            return WriteTextFile(outputPath, ToHtml(snapshot), errorMessage);
        }
        catch (const std::exception& exception)
        {
            if (errorMessage != nullptr)
                *errorMessage = exception.what();
            return false;
        }
    }
} // namespace RenderGraph::Debug

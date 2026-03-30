#include "../Core/pch.h"
#include "../Core/DepthBuffer.h"
#include "../Core/ColorBuffer.h"

#include "../Core/RenderGraph.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace RenderGraph;

namespace RenderGraph
{
    struct GraphTestAccess
    {
        static void InjectRead(Graph& graph, uint32_t passIndex, ResourceHandle handle, AccessType access = AccessType::ReadSrv)
        {
            if (passIndex >= graph.m_Passes.size())
                throw std::runtime_error("InjectRead: pass index out of range.");
            if (!handle.IsValid())
                throw std::runtime_error("InjectRead: invalid handle.");

            graph.m_Passes[passIndex].Reads.push_back(Graph::ReadDecl{ handle, access });
        }
    };
}

namespace
{
    struct DummyResource final : public GpuResource
    {
    };

    using TestFn = std::function<void()>;

    void Require(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    bool Contains(const std::string& text, const std::string& token)
    {
        return text.find(token) != std::string::npos;
    }

    bool FileContains(const std::filesystem::path& path, const std::string& token)
    {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open())
            return false;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return Contains(content, token);
    }

    CompileOptions NoDump()
    {
        CompileOptions options;
        options.DumpGraph = false;
        options.EnablePassCulling = true;
        return options;
    }

    uint32_t RandRange(std::mt19937& rng, uint32_t minValue, uint32_t maxValue)
    {
        std::uniform_int_distribution<uint32_t> dist(minValue, maxValue);
        return dist(rng);
    }

    void Test_StaleHandleDetection()
    {
        DummyResource resource;
        Graph graph;
        auto base = graph.Import("R", resource);
        auto latest = base;

        graph.AddPass(
            "Writer",
            [&](PassBuilder& builder)
            {
                latest = builder.Write(latest, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "ReadStale",
            [&](PassBuilder& builder)
            {
                builder.Read(base, AccessType::ReadSrv);
            },
            [](PassExecutionContext&) {});

        Require(!graph.Compile(NoDump()), "Expected compile to fail for stale handle.");
        Require(Contains(graph.GetLastCompileError(), "stale handle"), "Expected stale handle error message.");
    }

    void Test_SideEffectCullingKeepsDependencyChain()
    {
        DummyResource resourceA;
        DummyResource resourceB;
        Graph graph;
        auto a = graph.Import("A", resourceA);
        auto b = graph.Import("B", resourceB);

        graph.AddPass(
            "ProduceA",
            [&](PassBuilder& builder)
            {
                a = builder.Write(a, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "ConsumeA_SideEffect",
            [&](PassBuilder& builder)
            {
                builder.Read(a, AccessType::ReadSrv);
                builder.SetSideEffect();
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "UnrelatedB",
            [&](PassBuilder& builder)
            {
                b = builder.Write(b, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        Require(graph.Compile(NoDump()), "Compile should succeed.");
        const auto& infos = graph.GetPassInfos();
        Require(infos.size() == 3, "Expected 3 pass infos.");
        Require(!infos[0].Culled, "Producer dependency should stay active.");
        Require(!infos[1].Culled, "Side-effect pass should stay active.");
        Require(infos[2].Culled, "Unrelated non-root pass should be culled.");
    }

    void Test_ExportCullingKeepsProducerChain()
    {
        DummyResource resourceA;
        DummyResource resourceB;
        Graph graph;
        auto a = graph.Import("A", resourceA);
        auto b = graph.Import("B", resourceB);

        graph.AddPass(
            "WriteA_v1",
            [&](PassBuilder& builder)
            {
                a = builder.Write(a, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "WriteA_v2",
            [&](PassBuilder& builder)
            {
                builder.Read(a, AccessType::ReadSrv);
                a = builder.Write(a, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "UnrelatedB",
            [&](PassBuilder& builder)
            {
                b = builder.Write(b, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.Export(a);

        Require(graph.Compile(NoDump()), "Compile should succeed.");
        const auto& infos = graph.GetPassInfos();
        Require(infos.size() == 3, "Expected 3 pass infos.");
        Require(!infos[0].Culled, "Export chain producer should stay active.");
        Require(!infos[1].Culled, "Export chain writer should stay active.");
        Require(infos[2].Culled, "Unrelated non-export pass should be culled.");
    }

    void Test_CycleDetection_InjectedBackEdge()
    {
        DummyResource resource;
        Graph graph;
        auto base = graph.Import("R", resource);
        auto v1 = base;
        auto v2 = base;

        graph.AddPass(
            "Pass0_WriteV1",
            [&](PassBuilder& builder)
            {
                v1 = builder.Write(v1, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "Pass1_ReadV1_WriteV2",
            [&](PassBuilder& builder)
            {
                builder.Read(v1, AccessType::ReadSrv);
                v2 = builder.Write(v1, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        // Inject back-edge: pass0 reads v2 (written by pass1), forming pass0 <-> pass1 cycle.
        RenderGraph::GraphTestAccess::InjectRead(graph, 0, v2, AccessType::ReadSrv);

        Require(!graph.Compile(NoDump()), "Expected compile to fail when cycle exists.");
        Require(Contains(graph.GetLastCompileError(), "cycle detected"), "Expected cycle detected error message.");
    }

    void Test_BarrierDedup_WriteDominates()
    {
        DummyResource resource;
        Graph graph;
        auto handle = graph.Import("R", resource);

        graph.AddPass(
            "MixedAccess",
            [&](PassBuilder& builder)
            {
                builder.Read(handle, AccessType::ReadSrv);
                builder.Read(handle, AccessType::CopySrc);
                handle = builder.Write(handle, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        Require(graph.Compile(NoDump()), "Compile should succeed.");
        const auto& batches = graph.GetBarrierBatches();
        Require(batches.size() == 1, "Expected exactly one active pass barrier batch.");
        Require(batches[0].size() == 1, "Expected deduped single barrier op for same resource.");
        Require(batches[0][0].State == D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Write state should dominate mixed access.");
    }

    void Test_ResourceKindValidation()
    {
        ColorBuffer color;
        DepthBuffer depth;
        DummyResource buffer;

        {
            Graph graph;
            auto colorHandle = graph.ImportColor("Color", color);
            graph.AddPass(
                "ColorReadDepth_Invalid",
                [&](PassBuilder& builder)
                {
                    builder.Read(colorHandle, AccessType::ReadDepth);
                },
                [](PassExecutionContext&) {});

            Require(!graph.Compile(NoDump()), "Expected compile failure for color ReadDepth.");
            Require(Contains(graph.GetLastCompileError(), "invalid access"), "Expected invalid access error for color.");
        }

        {
            Graph graph;
            auto depthHandle = graph.ImportDepth("Depth", depth);
            graph.AddPass(
                "DepthWriteRT_Invalid",
                [&](PassBuilder& builder)
                {
                    depthHandle = builder.Write(depthHandle, AccessType::WriteRenderTarget);
                },
                [](PassExecutionContext&) {});

            Require(!graph.Compile(NoDump()), "Expected compile failure for depth WriteRenderTarget.");
            Require(Contains(graph.GetLastCompileError(), "invalid access"), "Expected invalid access error for depth.");
        }

        {
            Graph graph;
            auto bufferHandle = graph.ImportBuffer("Buffer", buffer);
            graph.AddPass(
                "BufferReadSrv_WriteUav_Valid",
                [&](PassBuilder& builder)
                {
                    builder.Read(bufferHandle, AccessType::ReadSrv);
                    bufferHandle = builder.Write(bufferHandle, AccessType::WriteUav);
                },
                [](PassExecutionContext&) {});

            Require(graph.Compile(NoDump()), "Expected compile success for valid buffer accesses.");
        }
    }

    void Test_LifetimeAnalysis_FirstLastUse()
    {
        DummyResource resourceA;
        DummyResource resourceB;
        Graph graph;
        auto a = graph.Import("A", resourceA);
        auto b = graph.Import("B", resourceB);

        graph.AddPass(
            "ReadA",
            [&](PassBuilder& builder)
            {
                builder.Read(a, AccessType::ReadSrv);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "WriteAAndB",
            [&](PassBuilder& builder)
            {
                a = builder.Write(a, AccessType::WriteUav);
                b = builder.Write(b, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "ReadAAfterWrite",
            [&](PassBuilder& builder)
            {
                builder.Read(a, AccessType::ReadSrv);
            },
            [](PassExecutionContext&) {});

        Require(graph.Compile(NoDump()), "Compile should succeed.");
        const auto& lifetimes = graph.GetResourceLifetimes();
        Require(lifetimes.size() >= 2, "Expected at least 2 resource lifetimes.");
        Require(lifetimes[0].FirstPass == 0 && lifetimes[0].LastPass == 2, "Resource A lifetime mismatch.");
        Require(lifetimes[1].FirstPass == 1 && lifetimes[1].LastPass == 1, "Resource B lifetime mismatch.");
    }

    void Test_DiagnosticsDump_ContainsPassStatus()
    {
        DummyResource resourceA;
        DummyResource resourceB;
        Graph graph;
        auto a = graph.Import("A", resourceA);
        auto b = graph.Import("B", resourceB);

        graph.AddPass(
            "ActiveSideEffect",
            [&](PassBuilder& builder)
            {
                a = builder.Write(a, AccessType::WriteUav);
                builder.SetSideEffect();
            },
            [](PassExecutionContext&) {});

        graph.AddPass(
            "CulledPass",
            [&](PassBuilder& builder)
            {
                b = builder.Write(b, AccessType::WriteUav);
            },
            [](PassExecutionContext&) {});

        const std::filesystem::path dumpPrefix =
            std::filesystem::temp_directory_path() / "NyxRenderGraphTests" / "diag_status";
        const std::filesystem::path dotPath = dumpPrefix.string() + ".dot";
        const std::filesystem::path jsonPath = dumpPrefix.string() + ".json";

        std::error_code ec;
        std::filesystem::create_directories(dumpPrefix.parent_path(), ec);
        std::filesystem::remove(dotPath, ec);
        std::filesystem::remove(jsonPath, ec);

        CompileOptions options;
        options.EnablePassCulling = true;
        options.DumpGraph = true;
        options.DumpPathPrefix = dumpPrefix.string();

        Require(graph.Compile(options), "Compile should succeed.");
        Require(std::filesystem::exists(dotPath), "DOT file should be generated.");
        Require(std::filesystem::exists(jsonPath), "JSON file should be generated.");
        Require(FileContains(dotPath, "Pass Dependency Graph"), "DOT should contain pass graph title.");
        Require(FileContains(dotPath, "status=active"), "DOT should contain active pass status.");
        Require(FileContains(dotPath, "status=culled"), "DOT should contain culled pass status.");
        Require(FileContains(jsonPath, "\"active\": true"), "JSON should contain active pass status.");
        Require(FileContains(jsonPath, "\"active\": false"), "JSON should contain culled pass status.");
    }

    void Test_RandomGraph_CullingProperty()
    {
        constexpr uint32_t kBaseSeed = 0xC0FFEEu;
        constexpr uint32_t kCases = 64;

        for (uint32_t caseIndex = 0; caseIndex < kCases; ++caseIndex)
        {
            std::mt19937 rng(kBaseSeed + caseIndex * 7919u);

            const uint32_t resourceCount = RandRange(rng, 2u, 8u);
            const uint32_t passCount = RandRange(rng, 4u, 24u);

            Graph graph;
            std::vector<std::unique_ptr<DummyResource>> resources(resourceCount);
            std::vector<ResourceHandle> latest(resourceCount);
            std::vector<int32_t> lastWriter(resourceCount, -1);

            for (uint32_t r = 0; r < resourceCount; ++r)
            {
                resources[r] = std::make_unique<DummyResource>();
                latest[r] = graph.Import("R" + std::to_string(r), *resources[r]);
            }

            std::vector<std::vector<uint32_t>> reverseEdges(passCount);
            std::vector<uint32_t> roots;
            std::unordered_set<uint64_t> seenEdges;

            auto addEdge = [&](uint32_t from, uint32_t to)
            {
                if (from == to)
                    return;
                const uint64_t key = (uint64_t(from) << 32ull) | uint64_t(to);
                if (!seenEdges.insert(key).second)
                    return;
                reverseEdges[to].push_back(from);
            };

            for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
            {
                const uint32_t readCount = RandRange(rng, 0u, resourceCount);
                std::vector<uint32_t> readResources;
                readResources.reserve(readCount);
                for (uint32_t i = 0; i < readCount; ++i)
                    readResources.push_back(RandRange(rng, 0u, resourceCount - 1u));

                const bool doWrite = RandRange(rng, 0u, 99u) < 65u;
                const uint32_t writeResource = RandRange(rng, 0u, resourceCount - 1u);

                const bool sideEffect = (RandRange(rng, 0u, 99u) < 25u);

                for (uint32_t resourceIndex : readResources)
                {
                    if (lastWriter[resourceIndex] >= 0)
                        addEdge(static_cast<uint32_t>(lastWriter[resourceIndex]), passIndex);
                }
                if (doWrite && lastWriter[writeResource] >= 0)
                    addEdge(static_cast<uint32_t>(lastWriter[writeResource]), passIndex);

                graph.AddPass(
                    "RandPass_" + std::to_string(caseIndex) + "_" + std::to_string(passIndex),
                    [readResources, doWrite, writeResource, sideEffect, &latest](PassBuilder& builder)
                    {
                        for (uint32_t resourceIndex : readResources)
                            builder.Read(latest[resourceIndex], AccessType::ReadSrv);

                        if (doWrite)
                            latest[writeResource] = builder.Write(latest[writeResource], AccessType::WriteUav);

                        if (sideEffect)
                            builder.SetSideEffect();
                    },
                    [](PassExecutionContext&) {});

                if (doWrite)
                    lastWriter[writeResource] = static_cast<int32_t>(passIndex);
                if (sideEffect)
                    roots.push_back(passIndex);
            }

            Require(graph.Compile(NoDump()), "Random graph compile should succeed.");

            std::vector<bool> expectedLive(passCount, false);
            if (roots.empty())
            {
                std::fill(expectedLive.begin(), expectedLive.end(), true);
            }
            else
            {
                std::deque<uint32_t> queue(roots.begin(), roots.end());
                while (!queue.empty())
                {
                    const uint32_t p = queue.front();
                    queue.pop_front();
                    if (expectedLive[p])
                        continue;

                    expectedLive[p] = true;
                    for (uint32_t producer : reverseEdges[p])
                        queue.push_back(producer);
                }
            }

            const auto& infos = graph.GetPassInfos();
            Require(infos.size() == passCount, "Random graph: unexpected pass info size.");

            for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
            {
                const bool actualLive = !infos[passIndex].Culled;
                if (actualLive != expectedLive[passIndex])
                {
                    throw std::runtime_error(
                        "Random graph culling mismatch at case=" + std::to_string(caseIndex) +
                        ", seed=" + std::to_string(kBaseSeed + caseIndex * 7919u) +
                        ", pass=" + std::to_string(passIndex));
                }
            }
        }
    }

} // namespace

int main()
{
    struct Case
    {
        const char* Name;
        TestFn Fn;
    };

    const std::vector<Case> tests = {
        { "StaleHandleDetection", Test_StaleHandleDetection },
        { "SideEffectCullingKeepsDependencyChain", Test_SideEffectCullingKeepsDependencyChain },
        { "ExportCullingKeepsProducerChain", Test_ExportCullingKeepsProducerChain },
        { "CycleDetection_InjectedBackEdge", Test_CycleDetection_InjectedBackEdge },
        { "BarrierDedup_WriteDominates", Test_BarrierDedup_WriteDominates },
        { "ResourceKindValidation", Test_ResourceKindValidation },
        { "LifetimeAnalysis_FirstLastUse", Test_LifetimeAnalysis_FirstLastUse },
        { "DiagnosticsDump_ContainsPassStatus", Test_DiagnosticsDump_ContainsPassStatus },
        { "RandomGraph_CullingProperty", Test_RandomGraph_CullingProperty },
    };

    uint32_t passed = 0;
    uint32_t failed = 0;

    for (const Case& test : tests)
    {
        try
        {
            test.Fn();
            ++passed;
            std::cout << "[PASS] " << test.Name << "\n";
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::cout << "[FAIL] " << test.Name << " : " << ex.what() << "\n";
        }
        catch (...)
        {
            ++failed;
            std::cout << "[FAIL] " << test.Name << " : unknown exception\n";
        }
    }

    std::cout << "\nRenderGraph tests: " << passed << " passed, " << failed << " failed.\n";
    return failed == 0 ? 0 : 1;
}

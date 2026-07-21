#include "RenderGraphTestSupport.h"

namespace NyxRenderGraphTests
{
    TEST_CLASS(BarrierExecutionTests)
    {
      public:
        TEST_METHOD(D3D12StateMappingCoversEveryUsageAndShaderStage)
        {
            Assert::IsFalse(RenderGraph::TryGetD3D12ResourceState(
                                {RenderGraph::Usage::Undefined})
                                .has_value());
            Assert::IsFalse(RenderGraph::TryGetD3D12ResourceState(
                                {RenderGraph::Usage::ShaderResource,
                                 static_cast<RenderGraph::ShaderStage>(1u << 8)})
                                .has_value());
            AssertNativeState(
                {RenderGraph::Usage::Common},
                D3D12_RESOURCE_STATE_COMMON);
            AssertNativeState(
                {RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Pixel},
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            AssertNativeState(
                {RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Vertex},
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            AssertNativeState(
                {RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Compute},
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            AssertNativeState(
                {RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::All},
                static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            AssertNativeState(
                {RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::None},
                static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            AssertNativeState(
                {RenderGraph::Usage::UnorderedAccess, RenderGraph::ShaderStage::Compute},
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            AssertNativeState(
                {RenderGraph::Usage::RenderTarget},
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            AssertNativeState(
                {RenderGraph::Usage::DepthRead},
                D3D12_RESOURCE_STATE_DEPTH_READ);
            AssertNativeState(
                {RenderGraph::Usage::DepthWrite},
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            AssertNativeState(
                {RenderGraph::Usage::CopySource},
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            AssertNativeState(
                {RenderGraph::Usage::CopyDestination},
                D3D12_RESOURCE_STATE_COPY_DEST);
            AssertNativeState(
                {RenderGraph::Usage::IndirectArgument},
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            AssertNativeState(
                {RenderGraph::Usage::Present},
                D3D12_RESOURCE_STATE_PRESENT);

            Assert::IsTrue(RenderGraph::IsD3D12ResourceStateCompatible(
                D3D12_RESOURCE_STATE_GENERIC_READ,
                static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
            Assert::IsFalse(RenderGraph::IsD3D12ResourceStateCompatible(
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

            int external = 0;
            RenderGraph::Graph stageGraph("Imported stage");
            const auto buffer = stageGraph.ImportBuffer(
                "Pixel SRV",
                BufferDesc(),
                &external,
                RenderGraph::Usage::ShaderResource,
                RenderGraph::ShaderStage::Pixel);
            auto computeRead = BeginPass(stageGraph,
                                         "Compute read",
                                         RenderGraph::PassFlags::SideEffect);
            computeRead.ReadSRV(buffer, RenderGraph::ShaderStage::Compute);
            SetNoop(computeRead);
            Assert::IsTrue(stageGraph.Compile().Succeeded);
            Assert::AreEqual<size_t>(1, stageGraph.GetBarriers().size());
            Assert::IsTrue(
                stageGraph.GetBarriers().front().Before.Stages ==
                RenderGraph::ShaderStage::Pixel);
            Assert::IsTrue(
                stageGraph.GetBarriers().front().After.Stages ==
                RenderGraph::ShaderStage::Compute);
        }

        TEST_METHOD(RuntimeOrdersBarrierFlushPassAndEpilogue)
        {
            int external = 0;
            std::vector<RuntimeEvent> events;
            RenderGraph::Graph graph("Runtime ordering");
            auto texture = graph.ImportTexture(
                "Back buffer",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget),
                &external,
                RenderGraph::Usage::Common);

            auto render = BeginPass(graph, "Render", RenderGraph::PassFlags::SideEffect);
            const auto renderId = render.GetPassId();
            texture = render.WriteRTV(texture);
            SetExecute(render, [&](RenderGraph::PassContext& context)
                       {
                Assert::IsNull(context.GetCommandContext());
                events.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });

            auto sample = BeginPass(graph, "Sample", RenderGraph::PassFlags::SideEffect);
            const auto sampleId = sample.GetPassId();
            sample.ReadSRV(texture, RenderGraph::ShaderStage::Pixel);
            SetExecute(sample, [&](RenderGraph::PassContext& context)
                       { events.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });
            graph.Export(texture, RenderGraph::Usage::Present);

            Assert::IsTrue(graph.Compile().Succeeded);
            RecordingExecutionBackend sink(events);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(graph, sink));

            Assert::AreEqual<size_t>(8, events.size());
            Assert::IsTrue(events[0].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(events[0].After == RenderGraph::Usage::RenderTarget);
            Assert::IsTrue(events[1].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(events[2].Kind == RuntimeEventKind::Pass);
            Assert::AreEqual(renderId, events[2].Pass);
            Assert::IsTrue(events[3].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(events[3].After == RenderGraph::Usage::ShaderResource);
            Assert::IsTrue(events[4].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(events[5].Kind == RuntimeEventKind::Pass);
            Assert::AreEqual(sampleId, events[5].Pass);
            Assert::IsTrue(events[6].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(events[6].BarrierKind == RenderGraph::BarrierKind::FinalTransition);
            Assert::IsTrue(events[6].After == RenderGraph::Usage::Present);
            Assert::IsTrue(events[7].Kind == RuntimeEventKind::BarrierBatchFlush);
        }

        TEST_METHOD(RuntimeBatchesTransitionsAndRecordsExplicitUavBarrier)
        {
            int firstExternal = 0;
            int secondExternal = 0;
            std::vector<RuntimeEvent> batchEvents;
            RenderGraph::Graph batchGraph("Barrier batch");
            auto first = batchGraph.ImportBuffer(
                "First", BufferDesc(), &firstExternal, RenderGraph::Usage::Common);
            auto second = batchGraph.ImportBuffer(
                "Second", BufferDesc(), &secondExternal, RenderGraph::Usage::Common);
            auto copy = BeginPass(batchGraph, "Copy destinations", RenderGraph::PassFlags::SideEffect);
            first = copy.CopyDst(first);
            second = copy.CopyDst(second);
            SetExecute(copy, [&](RenderGraph::PassContext& context)
                       { batchEvents.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });
            Assert::IsTrue(batchGraph.Compile().Succeeded);
            RecordingExecutionBackend batchSink(batchEvents);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(batchGraph, batchSink));
            Assert::AreEqual<size_t>(4, batchEvents.size());
            Assert::IsTrue(batchEvents[0].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(batchEvents[1].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(batchEvents[2].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(batchEvents[3].Kind == RuntimeEventKind::Pass);

            int uavExternal = 0;
            std::vector<RuntimeEvent> uavEvents;
            RenderGraph::Graph uavGraph("UAV recording");
            auto buffer = uavGraph.ImportBuffer(
                "UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &uavExternal,
                RenderGraph::Usage::Common);
            auto firstWrite = BeginPass(uavGraph, "Write 0", RenderGraph::PassFlags::SideEffect);
            buffer = firstWrite.WriteUAV(buffer, RenderGraph::ShaderStage::Compute);
            SetExecute(firstWrite, [&](RenderGraph::PassContext& context)
                       { uavEvents.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });
            auto secondWrite = BeginPass(uavGraph, "Write 1", RenderGraph::PassFlags::SideEffect);
            buffer = secondWrite.WriteUAV(buffer, RenderGraph::ShaderStage::Compute);
            SetExecute(secondWrite, [&](RenderGraph::PassContext& context)
                       { uavEvents.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });
            Assert::IsTrue(uavGraph.Compile().Succeeded);
            RecordingExecutionBackend uavSink(uavEvents);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(uavGraph, uavSink));
            Assert::AreEqual<size_t>(6, uavEvents.size());
            Assert::IsTrue(uavEvents[0].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(uavEvents[1].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(uavEvents[2].Kind == RuntimeEventKind::Pass);
            Assert::IsTrue(uavEvents[3].Kind == RuntimeEventKind::UnorderedAccess);
            Assert::IsTrue(uavEvents[4].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(uavEvents[5].Kind == RuntimeEventKind::Pass);
        }

        TEST_METHOD(RuntimeSupportsMoreThanOneCommandContextBarrierBufferOfUavs)
        {
            constexpr size_t resourceCount = 17;
            std::array<int, resourceCount> externalResources{};
            std::vector<RuntimeEvent> events;
            RenderGraph::Graph graph("Large UAV batch");
            std::vector<RenderGraph::BufferHandle> buffers;
            buffers.reserve(resourceCount);
            for (size_t index = 0; index < resourceCount; ++index)
            {
                buffers.push_back(graph.ImportBuffer(
                    "UAV " + std::to_string(index),
                    BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                    &externalResources[index],
                    RenderGraph::Usage::UnorderedAccess));
            }

            auto pass = BeginPass(graph, "Read all UAVs", RenderGraph::PassFlags::SideEffect);
            for (const auto buffer : buffers)
                pass.ReadUAV(buffer, RenderGraph::ShaderStage::Compute);
            SetExecute(pass, [&](RenderGraph::PassContext& context)
                       { events.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(resourceCount, CountBarriers(
                                                        graph,
                                                        RenderGraph::BarrierKind::UnorderedAccess));
            RecordingExecutionBackend sink(events);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(graph, sink));
            Assert::AreEqual<size_t>(resourceCount + 2, events.size());
            for (size_t index = 0; index < resourceCount; ++index)
                Assert::IsTrue(events[index].Kind == RuntimeEventKind::UnorderedAccess);
            Assert::IsTrue(events[resourceCount].Kind == RuntimeEventKind::BarrierBatchFlush);
            Assert::IsTrue(events.back().Kind == RuntimeEventKind::Pass);
        }

        TEST_METHOD(RuntimeRecordsAliasingBeforeTransitionsAndResolvesDistinctResources)
        {
            std::vector<RuntimeEvent> events;
            RenderGraph::Graph graph("Aliasing execution");
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget);
            auto first = graph.CreateTexture("First", desc);
            auto second = graph.CreateTexture("Second", desc);

            auto firstPass = BeginPass(graph, "First", RenderGraph::PassFlags::SideEffect);
            first = firstPass.WriteRTV(first);
            SetExecute(firstPass, [&](RenderGraph::PassContext& context)
                       {
                Assert::IsNotNull(context.GetResource(first));
                events.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()});
            });
            auto secondPass = BeginPass(graph, "Second", RenderGraph::PassFlags::SideEffect);
            second = secondPass.WriteRTV(second);
            SetExecute(secondPass, [&](RenderGraph::PassContext& context)
                       {
                Assert::IsNotNull(context.GetResource(second));
                events.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()});
            });

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
            RecordingExecutionBackend backend(events, true);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(graph, backend));

            Assert::AreEqual<size_t>(8, events.size());
            Assert::IsTrue(events[0].Kind == RuntimeEventKind::Aliasing);
            Assert::IsNull(events[0].BeforeRuntimeResource);
            Assert::IsNotNull(events[0].AfterRuntimeResource);
            Assert::IsTrue(events[1].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(events[3].Kind == RuntimeEventKind::Pass);
            Assert::IsTrue(events[4].Kind == RuntimeEventKind::Aliasing);
            Assert::AreEqual(
                events[0].AfterRuntimeResource,
                events[4].BeforeRuntimeResource);
            Assert::IsTrue(
                events[4].BeforeRuntimeResource != events[4].AfterRuntimeResource);
            Assert::IsTrue(events[5].Kind == RuntimeEventKind::Transition);
            Assert::IsTrue(events[7].Kind == RuntimeEventKind::Pass);
        }

        TEST_METHOD(RuntimePreflightsWholeGraphAndIgnoresCulledTransientResources)
        {
            int external = 0;
            int callbackCount = 0;
            std::vector<RuntimeEvent> events;
            RenderGraph::Graph failingGraph("Preflight failure");
            const auto imported = failingGraph.ImportBuffer(
                "Imported", BufferDesc(), &external, RenderGraph::Usage::Common);
            auto importedPass = BeginPass(failingGraph, "Would execute", RenderGraph::PassFlags::SideEffect);
            importedPass.CopySrc(imported);
            SetExecute(importedPass, [&](RenderGraph::PassContext&)
                       { ++callbackCount; });

            auto transient = failingGraph.CreateBuffer(
                "Transient", BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto transientPass = BeginPass(failingGraph, "Missing later", RenderGraph::PassFlags::SideEffect);
            transient = transientPass.WriteUAV(transient);
            SetExecute(transientPass, [&](RenderGraph::PassContext&)
                       { ++callbackCount; });

            Assert::IsTrue(failingGraph.Compile().Succeeded);
            RecordingExecutionBackend failingSink(events);
            Assert::IsFalse(RenderGraph::Detail::GraphTestAccess::Execute(failingGraph, failingSink));
            Assert::AreEqual(0, callbackCount);
            Assert::AreEqual<size_t>(0, events.size());
            Assert::IsTrue(HasDiagnostic(
                failingGraph,
                RenderGraph::DiagnosticCode::UnresolvedRuntimeResource));

            int liveExternal = 0;
            std::vector<RuntimeEvent> liveEvents;
            RenderGraph::Graph culledGraph("Culled transient");
            const auto live = culledGraph.ImportBuffer(
                "Live",
                BufferDesc(),
                &liveExternal,
                RenderGraph::Usage::ShaderResource);
            auto livePass = BeginPass(culledGraph, "Barrierless", RenderGraph::PassFlags::SideEffect);
            livePass.ReadSRV(live);
            SetExecute(livePass, [&](RenderGraph::PassContext& context)
                       { liveEvents.push_back({RuntimeEventKind::Pass, {}, context.GetPassId()}); });
            auto dead = culledGraph.CreateBuffer(
                "Dead transient", BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto deadPass = BeginPass(culledGraph, "Dead");
            dead = deadPass.WriteUAV(dead);
            SetNoop(deadPass);

            Assert::IsTrue(culledGraph.Compile().Succeeded);
            RecordingExecutionBackend liveSink(liveEvents);
            Assert::IsTrue(RenderGraph::Detail::GraphTestAccess::Execute(culledGraph, liveSink));
            Assert::AreEqual<size_t>(1, liveEvents.size());
            Assert::IsTrue(liveEvents.front().Kind == RuntimeEventKind::Pass);
        }
    };

} // namespace NyxRenderGraphTests

#include "RenderGraphTestSupport.h"

namespace NyxRenderGraphTests
{
    TEST_CLASS(DependencyAndCullingTests)
    {
      public:
        TEST_METHOD(LinearForkJoinBuildsDataDependenciesAndStableOrder)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess);
            RenderGraph::Graph graph("Fork Join");
            auto a = graph.CreateTexture("A", desc);
            auto b = graph.CreateTexture("B", desc);
            auto c = graph.CreateTexture("C", desc);
            auto output = graph.CreateTexture("Output", desc);

            auto produce = BeginPass(graph, "Produce A");
            const RenderGraph::PassId produceId = produce.GetPassId();
            a = produce.Write(a, RenderGraph::Usage::UnorderedAccess);
            SetNoop(produce);

            auto left = BeginPass(graph, "Left branch");
            const RenderGraph::PassId leftId = left.GetPassId();
            left.Read(a);
            b = left.Write(b, RenderGraph::Usage::UnorderedAccess);
            SetNoop(left);

            auto right = BeginPass(graph, "Right branch");
            const RenderGraph::PassId rightId = right.GetPassId();
            right.Read(a);
            c = right.Write(c, RenderGraph::Usage::UnorderedAccess);
            SetNoop(right);

            auto join = BeginPass(graph, "Join");
            const RenderGraph::PassId joinId = join.GetPassId();
            join.Read(b);
            join.Read(c);
            output = join.Write(output, RenderGraph::Usage::UnorderedAccess);
            SetNoop(join);
            graph.Export(output);

            const RenderGraph::CompileResult compileResult = graph.Compile();
            Assert::IsTrue(compileResult.Succeeded);
            const auto& order = graph.GetExecutionOrder();
            Assert::AreEqual<size_t>(4, order.size());
            Assert::AreEqual<uint32_t>(4, compileResult.LivePassCount);
            Assert::AreEqual<uint32_t>(0, compileResult.CulledPassCount);
            Assert::AreEqual<uint32_t>(
                static_cast<uint32_t>(graph.GetBarriers().size()),
                compileResult.BarrierCount);
            Assert::AreEqual(produceId, order[0]);
            Assert::AreEqual(leftId, order[1]);
            Assert::AreEqual(rightId, order[2]);
            Assert::AreEqual(joinId, order[3]);

            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::IsTrue(HasEdge(snapshot, produceId, leftId, RenderGraph::EdgeKind::Data));
            Assert::IsTrue(HasEdge(snapshot, produceId, rightId, RenderGraph::EdgeKind::Data));
            Assert::IsTrue(HasEdge(snapshot, leftId, joinId, RenderGraph::EdgeKind::Data));
            Assert::IsTrue(HasEdge(snapshot, rightId, joinId, RenderGraph::EdgeKind::Data));
        }

        TEST_METHOD(ReadReadAddsNoEdgeWhileWARAndWAWOrderWrites)
        {
            int external = 0;
            RenderGraph::Graph graph("Hazards");
            auto buffer = graph.ImportBuffer(
                "Shared",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::ShaderResource);

            auto read0 = BeginPass(graph, "Read 0", RenderGraph::PassFlags::SideEffect);
            const auto read0Id = read0.GetPassId();
            read0.Read(buffer);
            SetNoop(read0);

            auto read1 = BeginPass(graph, "Read 1", RenderGraph::PassFlags::SideEffect);
            const auto read1Id = read1.GetPassId();
            read1.Read(buffer);
            SetNoop(read1);

            auto write0 = BeginPass(graph, "Write 0", RenderGraph::PassFlags::SideEffect);
            const auto write0Id = write0.GetPassId();
            buffer = write0.Write(buffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(write0);

            auto write1 = BeginPass(graph, "Write 1", RenderGraph::PassFlags::SideEffect);
            const auto write1Id = write1.GetPassId();
            buffer = write1.Write(buffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(write1);
            graph.Export(buffer);

            Assert::IsTrue(graph.Compile().Succeeded);
            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::IsFalse(HasEdge(snapshot, read0Id, read1Id, RenderGraph::EdgeKind::Data));
            Assert::IsFalse(HasEdge(snapshot, read1Id, read0Id, RenderGraph::EdgeKind::Data));
            Assert::IsFalse(HasAnyEdge(snapshot, read0Id, read1Id));
            Assert::IsTrue(HasEdge(snapshot, read0Id, write0Id, RenderGraph::EdgeKind::WAR));
            Assert::IsTrue(HasEdge(snapshot, read1Id, write0Id, RenderGraph::EdgeKind::WAR));
            Assert::IsTrue(HasEdge(snapshot, write0Id, write1Id, RenderGraph::EdgeKind::WAW));
        }

        TEST_METHOD(CulledWriterContractsLiveWawAndWarHazards)
        {
            RenderGraph::Graph graph("Contracted hazards");
            auto texture = graph.CreateTexture(
                "Version chain",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));

            // Pass IDs intentionally oppose the order in which versions are declared.
            auto finalWriter = BeginPass(graph, "Final writer");
            const auto finalId = finalWriter.GetPassId();
            auto deadWriter = BeginPass(graph, "Dead writer");
            const auto deadId = deadWriter.GetPassId();
            auto reader = BeginPass(graph, "Live reader", RenderGraph::PassFlags::SideEffect);
            const auto readerId = reader.GetPassId();
            auto firstWriter = BeginPass(graph, "First writer", RenderGraph::PassFlags::SideEffect);
            const auto firstId = firstWriter.GetPassId();

            texture = firstWriter.Write(texture, RenderGraph::Usage::UnorderedAccess);
            reader.Read(texture);
            texture = deadWriter.Write(texture, RenderGraph::Usage::UnorderedAccess);
            texture = finalWriter.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(firstWriter);
            SetNoop(reader);
            SetNoop(deadWriter);
            SetNoop(finalWriter);
            graph.Export(texture);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(graph.IsPassLive(deadId));
            const auto& order = graph.GetExecutionOrder();
            Assert::AreEqual<size_t>(3, order.size());
            Assert::AreEqual(firstId, order[0]);
            Assert::AreEqual(readerId, order[1]);
            Assert::AreEqual(finalId, order[2]);

            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::IsTrue(HasEdge(snapshot, firstId, finalId, RenderGraph::EdgeKind::WAW));
            Assert::IsTrue(HasEdge(snapshot, readerId, finalId, RenderGraph::EdgeKind::WAR));
        }

        TEST_METHOD(InterleavedBuildersCanExposeDependencyCycle)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess);
            RenderGraph::Graph graph("Cycle");
            auto x = graph.CreateTexture("X", desc);
            auto y = graph.CreateTexture("Y", desc);
            auto passA = BeginPass(graph, "A", RenderGraph::PassFlags::SideEffect);
            auto passB = BeginPass(graph, "B");

            x = passA.Write(x, RenderGraph::Usage::UnorderedAccess);
            passB.Read(x);
            y = passB.Write(y, RenderGraph::Usage::UnorderedAccess);
            passA.Read(y);
            SetNoop(passA);
            SetNoop(passB);

            Assert::IsFalse(graph.Compile().Succeeded);
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::CycleDetected));
            Assert::IsTrue(graph.CaptureDebugSnapshot().State == RenderGraph::CompileState::Failed);
        }

        TEST_METHOD(DiscardOverwriteDoesNotKeepPreviousProducer)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "Overwritten",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto oldWriter = BeginPass(graph, "Old writer");
            const auto oldWriterId = oldWriter.GetPassId();
            texture = oldWriter.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(oldWriter);

            auto finalWriter = BeginPass(graph, "Final writer");
            const auto finalWriterId = finalWriter.GetPassId();
            texture = finalWriter.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(finalWriter);
            graph.Export(texture);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(graph.IsPassLive(oldWriterId));
            Assert::IsTrue(graph.IsPassLive(finalWriterId));
            Assert::AreEqual<size_t>(1, graph.GetExecutionOrder().size());
        }

        TEST_METHOD(ReadWriteKeepsPreviousProducer)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "Accumulation",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto producer = BeginPass(graph, "Producer");
            const auto producerId = producer.GetPassId();
            texture = producer.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(producer);

            auto accumulate = BeginPass(graph, "Accumulate");
            const auto accumulateId = accumulate.GetPassId();
            texture = accumulate.ReadWrite(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(accumulate);
            graph.Export(texture);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(graph.IsPassLive(producerId));
            Assert::IsTrue(graph.IsPassLive(accumulateId));
            Assert::IsTrue(HasEdge(
                graph.CaptureDebugSnapshot(),
                producerId,
                accumulateId,
                RenderGraph::EdgeKind::Data));
        }

        TEST_METHOD(MultipleExportsSideEffectsAndNeverCullAreIndependentRoots)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess);
            RenderGraph::Graph graph;
            auto a = graph.CreateTexture("A", desc);
            auto b = graph.CreateTexture("B", desc);
            auto dead = graph.CreateTexture("Dead", desc);

            auto passA = BeginPass(graph, "A writer");
            const auto passAId = passA.GetPassId();
            a = passA.Write(a, RenderGraph::Usage::UnorderedAccess);
            SetNoop(passA);

            auto passB = BeginPass(graph, "B writer");
            const auto passBId = passB.GetPassId();
            b = passB.Write(b, RenderGraph::Usage::UnorderedAccess);
            SetNoop(passB);

            auto deadPass = BeginPass(graph, "Dead writer");
            const auto deadId = deadPass.GetPassId();
            dead = deadPass.Write(dead, RenderGraph::Usage::UnorderedAccess);
            SetNoop(deadPass);

            auto sideEffect = BeginPass(graph, "Side effect", RenderGraph::PassFlags::SideEffect);
            const auto sideEffectId = sideEffect.GetPassId();
            SetNoop(sideEffect);

            auto neverCull = BeginPass(graph, "Never cull", RenderGraph::PassFlags::NeverCull);
            const auto neverCullId = neverCull.GetPassId();
            SetNoop(neverCull);

            graph.Export(a);
            graph.Export(b);
            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(graph.IsPassLive(passAId));
            Assert::IsTrue(graph.IsPassLive(passBId));
            Assert::IsFalse(graph.IsPassLive(deadId));
            Assert::IsTrue(graph.IsPassLive(sideEffectId));
            Assert::IsTrue(graph.IsPassLive(neverCullId));
        }

        TEST_METHOD(DisableCullingKeepsIndependentPassesInInsertionOrder)
        {
            RenderGraph::Graph graph;
            for (int index = 0; index < 3; ++index)
            {
                auto pass = BeginPass(graph, "Independent " + std::to_string(index));
                SetNoop(pass);
            }

            RenderGraph::CompileOptions options;
            options.EnablePassCulling = false;
            Assert::IsTrue(graph.Compile(options).Succeeded);
            const auto& order = graph.GetExecutionOrder();
            Assert::AreEqual<size_t>(3, order.size());
            Assert::AreEqual<RenderGraph::PassId>(0, order[0]);
            Assert::AreEqual<RenderGraph::PassId>(1, order[1]);
            Assert::AreEqual<RenderGraph::PassId>(2, order[2]);
        }
    };

} // namespace NyxRenderGraphTests


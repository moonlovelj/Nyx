#include "RenderGraphTestSupport.h"

namespace NyxRenderGraphTests
{
    TEST_CLASS(ConstructionAndValidationTests)
    {
      public:
        TEST_METHOD(EmptyGraphCompilesAndExecutes)
        {
            RenderGraph::Graph graph("Empty");
            const RenderGraph::CompileResult result = graph.Compile();

            Assert::IsTrue(result.Succeeded);
            Assert::AreEqual<size_t>(0, graph.GetExecutionOrder().size());
            Assert::IsTrue(ExecuteCallbacks(graph));
        }

        TEST_METHOD(ImportedVersionZeroIsInitializedButTransientVersionZeroIsNot)
        {
            int external = 7;
            RenderGraph::Graph importedGraph;
            const auto imported = importedGraph.ImportTexture(
                "Imported",
                TextureDesc(),
                &external,
                RenderGraph::Usage::Common);
            auto importedPass = BeginPass(importedGraph, "Read imported", RenderGraph::PassFlags::SideEffect);
            importedPass.Read(imported, RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Pixel);
            SetNoop(importedPass);
            Assert::IsTrue(importedGraph.Compile().Succeeded);

            RenderGraph::Graph transientGraph;
            const auto transient = transientGraph.CreateTexture("Transient", TextureDesc());
            auto transientPass = BeginPass(transientGraph, "Read transient");
            transientPass.Read(transient);
            SetNoop(transientPass);
            Assert::IsFalse(transientGraph.Compile().Succeeded);
            Assert::IsTrue(HasDiagnostic(transientGraph, RenderGraph::DiagnosticCode::UninitializedRead));
        }

        TEST_METHOD(DefaultResetForeignAndStaleHandlesAreRejected)
        {
            {
                RenderGraph::Graph graph;
                RenderGraph::TextureHandle invalid;
                auto pass = BeginPass(graph, "Invalid handle");
                pass.Read(invalid);
                SetNoop(pass);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::InvalidHandle));
            }

            {
                RenderGraph::Graph graph;
                const auto oldHandle = graph.CreateTexture("Old", TextureDesc());
                const uint64_t oldEpoch = graph.GetEpoch();
                graph.Reset();
                Assert::IsTrue(oldEpoch != graph.GetEpoch());
                auto pass = BeginPass(graph, "Old epoch");
                pass.Read(oldHandle);
                SetNoop(pass);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::InvalidHandleEpoch));
            }

            {
                RenderGraph::Graph first;
                RenderGraph::Graph second;
                const auto foreign = first.CreateTexture("Foreign", TextureDesc());
                auto pass = BeginPass(second, "Foreign graph");
                pass.Read(foreign);
                SetNoop(pass);
                Assert::IsFalse(second.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(second, RenderGraph::DiagnosticCode::InvalidHandleEpoch));
            }

            {
                RenderGraph::Graph graph;
                const auto original = graph.CreateTexture(
                    "Versioned",
                    TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget));
                auto writer = BeginPass(graph, "Writer");
                const auto latest = writer.Write(original, RenderGraph::Usage::RenderTarget);
                SetNoop(writer);
                Assert::AreEqual<uint32_t>(1, latest.GetVersion());

                auto staleReader = BeginPass(graph, "Stale reader");
                staleReader.Read(original);
                SetNoop(staleReader);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::StaleVersion));
            }
        }

        TEST_METHOD(InvalidUsageAndConflictingDeclarationsAreRejected)
        {
            {
                RenderGraph::Graph graph;
                const auto buffer = graph.CreateBuffer("Buffer", BufferDesc());
                auto pass = BeginPass(graph, "Invalid RTV buffer");
                pass.Write(buffer, RenderGraph::Usage::RenderTarget);
                SetNoop(pass);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::InvalidUsage));
            }

            {
                RenderGraph::Graph graph;
                int external = 0;
                const auto texture = graph.ImportTexture(
                    "Texture",
                    TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                    &external,
                    RenderGraph::Usage::Common);
                auto pass = BeginPass(graph, "Conflicting declaration");
                pass.Read(texture, RenderGraph::Usage::ShaderResource);
                pass.Write(texture, RenderGraph::Usage::UnorderedAccess);
                SetNoop(pass);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::ConflictingUsage));
            }
        }

        TEST_METHOD(BuilderLifecycleAndExecuteCallbackAreValidated)
        {
            RenderGraph::Graph graph;
            auto openPass = BeginPass(graph, "Open pass");
            Assert::IsFalse(graph.Compile().Succeeded);
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::BuilderStillOpen));
            SetExecute(openPass, [](RenderGraph::PassContext&) {});
            Assert::IsTrue(graph.Compile().Succeeded);

            RenderGraph::Graph missingCallbackGraph;
            auto missingCallback = BeginPass(missingCallbackGraph, "Missing callback");
            Close(missingCallback);
            Assert::IsFalse(missingCallbackGraph.Compile().Succeeded);
            Assert::IsTrue(HasDiagnostic(
                missingCallbackGraph,
                RenderGraph::DiagnosticCode::MissingExecuteCallback));
        }

        TEST_METHOD(OnlyLatestInitializedVersionCanBeExported)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "Output",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto first = BeginPass(graph, "First write");
            texture = first.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(first);
            graph.Export(texture);

            auto second = BeginPass(graph, "Second write");
            texture = second.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(second);

            Assert::IsFalse(graph.Compile().Succeeded);
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::InvalidExport));
        }

        TEST_METHOD(DescriptorsFinalUsageAndReadWriteInitializationAreValidated)
        {
            {
                RenderGraph::Graph graph;
                auto invalidDesc = TextureDesc();
                invalidDesc.Width = 0;
                Assert::IsFalse(graph.CreateTexture("Invalid", invalidDesc).IsValid());
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::InvalidResourceDesc));
            }

            {
                RenderGraph::Graph graph;
                int external = 0;
                const auto imported = graph.ImportTexture(
                    "Imported",
                    TextureDesc(),
                    &external,
                    RenderGraph::Usage::Common);
                graph.Export(imported, RenderGraph::Usage::Present);
                graph.Export(imported, RenderGraph::Usage::Common);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::ConflictingFinalUsage));
            }

            {
                RenderGraph::Graph graph;
                const auto transient = graph.CreateTexture(
                    "Uninitialized RMW",
                    TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
                auto pass = BeginPass(graph, "RMW");
                pass.ReadWrite(transient);
                SetNoop(pass);
                Assert::IsFalse(graph.Compile().Succeeded);
                Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::UninitializedRead));
            }
        }

        TEST_METHOD(ResetInvalidatesOpenBuilderWithoutClosingNewPass)
        {
            RenderGraph::Graph graph("Persistent name");
            auto staleBuilder = BeginPass(graph, "Old pass");
            graph.Reset();
            Assert::AreEqual(std::string("Persistent name"), std::string(graph.GetName()));
            Assert::IsTrue(graph.CollectDiagnostics().empty());
            Assert::AreEqual<size_t>(0, graph.GetExecutionOrder().size());

            auto currentBuilder = BeginPass(graph, "Current pass", RenderGraph::PassFlags::SideEffect);
            Close(staleBuilder);
            SetNoop(currentBuilder);
            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(1, graph.GetExecutionOrder().size());

            RenderGraph::PassBuilder outlivesGraph;
            {
                RenderGraph::Graph temporary;
                outlivesGraph = BeginPass(temporary, "Temporary");
            }
            Close(outlivesGraph);
            Assert::IsFalse(IsOpen(outlivesGraph));
        }

        TEST_METHOD(DuplicateShaderReadsMergeStagesAndMarkSideEffectRootsPass)
        {
            int external = 0;
            RenderGraph::Graph graph;
            const auto texture = graph.ImportTexture(
                "Shared shader input",
                TextureDesc(),
                &external,
                RenderGraph::Usage::Common);
            auto pass = BeginPass(graph, "Multi-stage read");
            pass.Read(texture, RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Vertex);
            pass.Read(texture, RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Pixel);
            pass.MarkSideEffect();
            SetNoop(pass);

            Assert::IsTrue(graph.Compile().Succeeded);
            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::AreEqual<size_t>(1, snapshot.Passes[0].Accesses.size());
            Assert::IsTrue(
                snapshot.Passes[0].Accesses[0].State.Stages == RenderGraph::ShaderStage::AllGraphics);
            Assert::IsTrue(snapshot.Passes[0].Root);
        }
    };

    TEST_CLASS(SemanticPassBuilderApiTests)
    {
      public:
        TEST_METHOD(SemanticAccessorsMapToCoreAccessRecordsAndVersions)
        {
            int external[9] = {};
            RenderGraph::Graph graph("Semantic access mapping");

            const auto srvDefault = graph.ImportTexture(
                "Default SRV",
                TextureDesc(),
                &external[0],
                RenderGraph::Usage::Common);
            const auto srvPixel = graph.ImportBuffer(
                "Pixel SRV",
                BufferDesc(),
                &external[1],
                RenderGraph::Usage::Common);
            const auto readUav = graph.ImportBuffer(
                "Read UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external[2],
                RenderGraph::Usage::Common);
            const auto writeUav = graph.CreateBuffer(
                "Write UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            const auto readWriteUav = graph.ImportTexture(
                "Read-write UAV",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external[3],
                RenderGraph::Usage::Common);
            const auto writeRtv = graph.CreateTexture(
                "Write RTV",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget));
            const auto readWriteRtv = graph.ImportTexture(
                "Read-write RTV",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget),
                &external[4],
                RenderGraph::Usage::Common);
            const auto readDepth = graph.ImportTexture(
                "Read depth",
                TextureDesc(RenderGraph::ResourceFlags::AllowDepthStencil),
                &external[5],
                RenderGraph::Usage::Common);
            const auto writeDepth = graph.CreateTexture(
                "Write depth",
                TextureDesc(RenderGraph::ResourceFlags::AllowDepthStencil));
            const auto readWriteDepth = graph.ImportTexture(
                "Read-write depth",
                TextureDesc(RenderGraph::ResourceFlags::AllowDepthStencil),
                &external[6],
                RenderGraph::Usage::Common);
            const auto copySource = graph.ImportTexture(
                "Copy source",
                TextureDesc(),
                &external[7],
                RenderGraph::Usage::Common);
            const auto copyDestination = graph.CreateBuffer("Copy destination", BufferDesc());
            const auto indirect = graph.ImportBuffer(
                "Indirect arguments",
                BufferDesc(),
                &external[8],
                RenderGraph::Usage::Common);

            auto pass = BeginPass(graph, "Semantic declarations", RenderGraph::PassFlags::SideEffect);
            const auto srvDefaultResult = pass.ReadSRV(srvDefault);
            const auto srvPixelResult = pass.ReadSRV(srvPixel, RenderGraph::ShaderStage::Pixel);
            const auto readUavResult = pass.ReadUAV(readUav);
            const auto writeUavResult = pass.WriteUAV(writeUav);
            const auto readWriteUavResult =
                pass.ReadWriteUAV(readWriteUav, RenderGraph::ShaderStage::Compute);
            const auto writeRtvResult = pass.WriteRTV(writeRtv);
            const auto readWriteRtvResult = pass.ReadWriteRTV(readWriteRtv);
            const auto readDepthResult = pass.ReadDepth(readDepth);
            const auto writeDepthResult = pass.WriteDepth(writeDepth);
            const auto readWriteDepthResult = pass.ReadWriteDepth(readWriteDepth);
            const auto copySourceResult = pass.CopySrc(copySource);
            const auto copyDestinationResult = pass.CopyDst(copyDestination);
            const auto indirectResult = pass.ReadIndirectArgument(indirect);
            SetNoop(pass);

            Assert::IsTrue(srvDefaultResult == srvDefault);
            Assert::IsTrue(srvPixelResult == srvPixel);
            Assert::IsTrue(readUavResult == readUav);
            Assert::AreEqual<uint32_t>(1, writeUavResult.GetVersion());
            Assert::AreEqual<uint32_t>(1, readWriteUavResult.GetVersion());
            Assert::AreEqual<uint32_t>(1, writeRtvResult.GetVersion());
            Assert::AreEqual<uint32_t>(1, readWriteRtvResult.GetVersion());
            Assert::IsTrue(readDepthResult == readDepth);
            Assert::AreEqual<uint32_t>(1, writeDepthResult.GetVersion());
            Assert::AreEqual<uint32_t>(1, readWriteDepthResult.GetVersion());
            Assert::IsTrue(copySourceResult == copySource);
            Assert::AreEqual<uint32_t>(1, copyDestinationResult.GetVersion());
            Assert::IsTrue(indirectResult == indirect);

            Assert::IsTrue(graph.Compile().Succeeded);
            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::AreEqual<size_t>(1, snapshot.Passes.size());
            const auto& accesses = snapshot.Passes[0].Accesses;
            Assert::AreEqual<size_t>(13, accesses.size());
            AssertAccess(
                accesses[0], srvDefault.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::All, 0, 0);
            AssertAccess(
                accesses[1], srvPixel.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Pixel, 0, 0);
            AssertAccess(
                accesses[2], readUav.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::UnorderedAccess, RenderGraph::ShaderStage::All, 0, 0);
            AssertAccess(
                accesses[3], writeUav.GetResourceIndex(), RenderGraph::AccessMode::Write,
                RenderGraph::Usage::UnorderedAccess, RenderGraph::ShaderStage::All, 0, 1);
            AssertAccess(
                accesses[4], readWriteUav.GetResourceIndex(), RenderGraph::AccessMode::ReadWrite,
                RenderGraph::Usage::UnorderedAccess, RenderGraph::ShaderStage::Compute, 0, 1);
            AssertAccess(
                accesses[5], writeRtv.GetResourceIndex(), RenderGraph::AccessMode::Write,
                RenderGraph::Usage::RenderTarget, RenderGraph::ShaderStage::None, 0, 1);
            AssertAccess(
                accesses[6], readWriteRtv.GetResourceIndex(), RenderGraph::AccessMode::ReadWrite,
                RenderGraph::Usage::RenderTarget, RenderGraph::ShaderStage::None, 0, 1);
            AssertAccess(
                accesses[7], readDepth.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::DepthRead, RenderGraph::ShaderStage::None, 0, 0);
            AssertAccess(
                accesses[8], writeDepth.GetResourceIndex(), RenderGraph::AccessMode::Write,
                RenderGraph::Usage::DepthWrite, RenderGraph::ShaderStage::None, 0, 1);
            AssertAccess(
                accesses[9], readWriteDepth.GetResourceIndex(), RenderGraph::AccessMode::ReadWrite,
                RenderGraph::Usage::DepthWrite, RenderGraph::ShaderStage::None, 0, 1);
            AssertAccess(
                accesses[10], copySource.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::CopySource, RenderGraph::ShaderStage::None, 0, 0);
            AssertAccess(
                accesses[11], copyDestination.GetResourceIndex(), RenderGraph::AccessMode::Write,
                RenderGraph::Usage::CopyDestination, RenderGraph::ShaderStage::None, 0, 1);
            AssertAccess(
                accesses[12], indirect.GetResourceIndex(), RenderGraph::AccessMode::Read,
                RenderGraph::Usage::IndirectArgument, RenderGraph::ShaderStage::None, 0, 0);
        }

        TEST_METHOD(SemanticAccessorsProduceExpectedBarrierSequence)
        {
            int external = 0;
            RenderGraph::Graph graph("Semantic barrier sequence");
            auto texture = graph.ImportTexture(
                "Shared texture",
                TextureDesc(
                    RenderGraph::ResourceFlags::AllowRenderTarget |
                    RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::Common);

            auto render = BeginPass(graph, "Render", RenderGraph::PassFlags::SideEffect);
            texture = render.WriteRTV(texture);
            SetNoop(render);

            auto sample = BeginPass(graph, "Sample", RenderGraph::PassFlags::SideEffect);
            sample.ReadSRV(texture, RenderGraph::ShaderStage::Pixel);
            SetNoop(sample);

            auto update = BeginPass(graph, "Update", RenderGraph::PassFlags::SideEffect);
            texture = update.ReadWriteUAV(texture, RenderGraph::ShaderStage::Compute);
            SetNoop(update);

            auto inspect = BeginPass(graph, "Inspect", RenderGraph::PassFlags::SideEffect);
            inspect.ReadUAV(texture, RenderGraph::ShaderStage::Compute);
            SetNoop(inspect);
            graph.Export(texture, RenderGraph::Usage::ShaderResource);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(5, graph.GetBarriers().size());
            Assert::AreEqual<size_t>(3, CountBarriers(graph, RenderGraph::BarrierKind::Transition));
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::UnorderedAccess));
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::FinalTransition));

            const auto& barriers = graph.GetBarriers();
            Assert::IsTrue(barriers[0].Kind == RenderGraph::BarrierKind::Transition);
            Assert::IsTrue(barriers[0].Before.UsageType == RenderGraph::Usage::Common);
            Assert::IsTrue(barriers[0].After.UsageType == RenderGraph::Usage::RenderTarget);
            Assert::IsTrue(barriers[1].Kind == RenderGraph::BarrierKind::Transition);
            Assert::IsTrue(barriers[1].Before.UsageType == RenderGraph::Usage::RenderTarget);
            Assert::IsTrue(barriers[1].After.UsageType == RenderGraph::Usage::ShaderResource);
            Assert::IsTrue(barriers[1].After.Stages == RenderGraph::ShaderStage::Pixel);
            Assert::IsTrue(barriers[2].Kind == RenderGraph::BarrierKind::Transition);
            Assert::IsTrue(barriers[2].Before.UsageType == RenderGraph::Usage::ShaderResource);
            Assert::IsTrue(barriers[2].Before.Stages == RenderGraph::ShaderStage::Pixel);
            Assert::IsTrue(barriers[2].After.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(barriers[2].After.Stages == RenderGraph::ShaderStage::Compute);
            Assert::IsTrue(barriers[3].Kind == RenderGraph::BarrierKind::UnorderedAccess);
            Assert::IsTrue(barriers[3].Before.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(barriers[3].After.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(barriers[4].Kind == RenderGraph::BarrierKind::FinalTransition);
            Assert::IsTrue(barriers[4].AfterPass);
            Assert::IsTrue(barriers[4].Before.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(barriers[4].After.UsageType == RenderGraph::Usage::ShaderResource);
            Assert::IsTrue(barriers[4].After.Stages == RenderGraph::ShaderStage::All);
        }

        TEST_METHOD(SemanticAccessorsEnforceRequiredResourceFlags)
        {
            RenderGraph::Graph graph("Semantic validation");
            const auto rtv = graph.CreateTexture("Missing RTV flag", TextureDesc());
            const auto depth = graph.CreateTexture("Missing depth flag", TextureDesc());
            const auto uav = graph.CreateBuffer("Missing UAV flag", BufferDesc());

            auto pass = BeginPass(graph, "Invalid semantic declarations", RenderGraph::PassFlags::SideEffect);
            const auto invalidRtv = pass.WriteRTV(rtv);
            const auto invalidDepth = pass.WriteDepth(depth);
            const auto invalidUav = pass.WriteUAV(uav);
            SetNoop(pass);

            Assert::IsFalse(invalidRtv.IsValid());
            Assert::IsFalse(invalidDepth.IsValid());
            Assert::IsFalse(invalidUav.IsValid());
            Assert::IsFalse(graph.Compile().Succeeded);

            const auto diagnostics = graph.CollectDiagnostics();
            const size_t invalidUsageCount = static_cast<size_t>(std::count_if(
                diagnostics.begin(),
                diagnostics.end(),
                [](const RenderGraph::Diagnostic& diagnostic)
                {
                    return diagnostic.Code == RenderGraph::DiagnosticCode::InvalidUsage;
                }));
            Assert::AreEqual<size_t>(3, invalidUsageCount);

            const auto hasResourceDiagnostic = [&](RenderGraph::ResourceId resource)
            {
                return std::any_of(
                    diagnostics.begin(),
                    diagnostics.end(),
                    [&](const RenderGraph::Diagnostic& diagnostic)
                    {
                        return diagnostic.Code == RenderGraph::DiagnosticCode::InvalidUsage &&
                               diagnostic.Resource == resource;
                    });
            };
            Assert::IsTrue(hasResourceDiagnostic(rtv.GetResourceIndex()));
            Assert::IsTrue(hasResourceDiagnostic(depth.GetResourceIndex()));
            Assert::IsTrue(hasResourceDiagnostic(uav.GetResourceIndex()));
        }
    };

    TEST_CLASS(LambdaPassApiTests)
    {
      public:
        TEST_METHOD(SetupRunsImmediatelyExecuteIsDeferredAndBuilderAutoCloses)
        {
            struct PassData
            {
                RenderGraph::BufferHandle Output;
                RenderGraph::PassId Pass = RenderGraph::kInvalidIndex;
                int Marker = 0;
            };

            RenderGraph::Graph graph("Lambda lifecycle");
            const auto buffer = graph.CreateBuffer(
                "Output",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            int setupCount = 0;
            int executeCount = 0;

            PassData result = graph.AddPass<PassData>(
                "Scoped pass",
                [&](RenderGraph::PassBuilder& builder, PassData& data)
                {
                    ++setupCount;
                    data.Pass = builder.GetPassId();
                    data.Output = builder.Write(buffer, RenderGraph::Usage::UnorderedAccess);
                    data.Marker = 17;
                },
                [&](const PassData& data, RenderGraph::PassContext& context)
                {
                    ++executeCount;
                    Assert::AreEqual(17, data.Marker);
                    Assert::AreEqual(data.Pass, context.GetPassId());
                    Assert::IsTrue(context.IsDeclared(data.Output));
                },
                RenderGraph::PassFlags::SideEffect);

            Assert::AreEqual(1, setupCount);
            Assert::AreEqual(0, executeCount);
            Assert::IsTrue(result.Output.IsValid());
            Assert::IsTrue(result.Pass != RenderGraph::kInvalidIndex);
            result.Marker = 99;

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(HasDiagnostic(graph, RenderGraph::DiagnosticCode::BuilderStillOpen));
            Assert::IsFalse(HasDiagnostic(graph, RenderGraph::DiagnosticCode::MissingExecuteCallback));
            Assert::AreEqual(0, executeCount);
            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::AreEqual(2, executeCount);
        }

        TEST_METHOD(ReturnedPassDataChainsProducerAndConsumer)
        {
            struct ProducerData
            {
                RenderGraph::BufferHandle Output;
                RenderGraph::PassId Pass = RenderGraph::kInvalidIndex;
            };
            struct ConsumerData
            {
                RenderGraph::BufferHandle Input;
                RenderGraph::BufferHandle Output;
                RenderGraph::PassId Pass = RenderGraph::kInvalidIndex;
            };

            RenderGraph::Graph graph("Lambda chain");
            const auto intermediate = graph.CreateBuffer(
                "Intermediate",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            const auto finalBuffer = graph.CreateBuffer(
                "Final",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            std::vector<std::string> order;

            const ProducerData producer = graph.AddPass<ProducerData>(
                "Produce",
                [&](RenderGraph::PassBuilder& builder, ProducerData& data)
                {
                    data.Pass = builder.GetPassId();
                    data.Output = builder.Write(intermediate, RenderGraph::Usage::UnorderedAccess);
                },
                [&](const ProducerData& data, RenderGraph::PassContext& context)
                {
                    Assert::IsTrue(context.IsDeclared(data.Output));
                    order.push_back("Produce");
                });

            const ConsumerData consumer = graph.AddPass<ConsumerData>(
                "Consume",
                [&](RenderGraph::PassBuilder& builder, ConsumerData& data)
                {
                    data.Pass = builder.GetPassId();
                    data.Input = builder.Read(
                        producer.Output,
                        RenderGraph::Usage::ShaderResource,
                        RenderGraph::ShaderStage::Compute);
                    data.Output = builder.Write(finalBuffer, RenderGraph::Usage::UnorderedAccess);
                },
                [&](const ConsumerData& data, RenderGraph::PassContext& context)
                {
                    Assert::IsTrue(context.IsDeclared(data.Input));
                    Assert::IsTrue(context.IsDeclared(data.Output));
                    order.push_back("Consume");
                });
            graph.Export(consumer.Output, RenderGraph::Usage::ShaderResource);

            Assert::IsTrue(producer.Output.IsValid());
            Assert::IsTrue(consumer.Output.IsValid());
            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(2, graph.GetExecutionOrder().size());
            Assert::AreEqual(producer.Pass, graph.GetExecutionOrder()[0]);
            Assert::AreEqual(consumer.Pass, graph.GetExecutionOrder()[1]);
            Assert::IsTrue(HasEdge(
                graph.CaptureDebugSnapshot(),
                producer.Pass,
                consumer.Pass,
                RenderGraph::EdgeKind::Data));

            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::AreEqual<size_t>(2, order.size());
            Assert::AreEqual(std::string("Produce"), order[0]);
            Assert::AreEqual(std::string("Consume"), order[1]);
        }

        TEST_METHOD(CulledLambdaRunsSetupButNeverExecute)
        {
            struct PassData
            {
                RenderGraph::BufferHandle Output;
                RenderGraph::PassId Pass = RenderGraph::kInvalidIndex;
            };

            RenderGraph::Graph graph("Lambda culling");
            const auto deadBuffer = graph.CreateBuffer(
                "Dead",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            const auto liveBuffer = graph.CreateBuffer(
                "Live",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            int setupCount = 0;
            int deadExecuteCount = 0;
            int liveExecuteCount = 0;

            const PassData dead = graph.AddPass<PassData>(
                "Dead pass",
                [&](RenderGraph::PassBuilder& builder, PassData& data)
                {
                    ++setupCount;
                    data.Pass = builder.GetPassId();
                    data.Output = builder.Write(deadBuffer, RenderGraph::Usage::UnorderedAccess);
                },
                [&](const PassData&, RenderGraph::PassContext&)
                { ++deadExecuteCount; });

            const PassData live = graph.AddPass<PassData>(
                "Live pass",
                [&](RenderGraph::PassBuilder& builder, PassData& data)
                {
                    ++setupCount;
                    data.Pass = builder.GetPassId();
                    data.Output = builder.Write(liveBuffer, RenderGraph::Usage::UnorderedAccess);
                },
                [&](const PassData&, RenderGraph::PassContext&)
                { ++liveExecuteCount; },
                RenderGraph::PassFlags::SideEffect);

            Assert::AreEqual(2, setupCount);
            const auto result = graph.Compile();
            Assert::IsTrue(result.Succeeded);
            Assert::AreEqual<uint32_t>(1, result.LivePassCount);
            Assert::AreEqual<uint32_t>(1, result.CulledPassCount);
            Assert::IsFalse(graph.IsPassLive(dead.Pass));
            Assert::IsTrue(graph.IsPassLive(live.Pass));
            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::AreEqual(0, deadExecuteCount);
            Assert::AreEqual(1, liveExecuteCount);
        }

        TEST_METHOD(LambdaAddPassIsRejectedDuringExecutionWithoutRunningNestedSetup)
        {
            struct RootData
            {
            };
            struct NestedData
            {
                int Marker = 7;
            };

            RenderGraph::Graph graph("Lambda execution guard");
            bool nestedSetupRan = false;
            bool nestedExecuteRan = false;
            graph.AddPass<RootData>(
                "Root",
                [](RenderGraph::PassBuilder&, RootData&) {},
                [&](const RootData&, RenderGraph::PassContext&)
                {
                    const NestedData rejected = graph.AddPass<NestedData>(
                        "Rejected",
                        [&](RenderGraph::PassBuilder&, NestedData& data)
                        {
                            nestedSetupRan = true;
                            data.Marker = 99;
                        },
                        [&](const NestedData&, RenderGraph::PassContext&)
                        {
                            nestedExecuteRan = true;
                        },
                        RenderGraph::PassFlags::SideEffect);
                    Assert::AreEqual(7, rejected.Marker);
                },
                RenderGraph::PassFlags::SideEffect);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(ExecuteCallbacks(graph));
            Assert::IsFalse(nestedSetupRan);
            Assert::IsFalse(nestedExecuteRan);
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::MutationDuringExecution));
            Assert::AreEqual<size_t>(1, graph.GetExecutionOrder().size());
        }

        TEST_METHOD(SetupExceptionPropagatesClosesBuilderAndResetRecovers)
        {
            struct PassData
            {
                RenderGraph::BufferHandle Output;
            };

            RenderGraph::Graph graph("Lambda setup exception");
            const auto buffer = graph.CreateBuffer(
                "Output",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));

            Assert::ExpectException<std::runtime_error>([&]()
                                                        { graph.AddPass<PassData>(
                                                              "Throwing setup",
                                                              [&](RenderGraph::PassBuilder& builder, PassData& data)
                                                              {
                                                                  data.Output = builder.Write(buffer, RenderGraph::Usage::UnorderedAccess);
                                                                  throw std::runtime_error("expected setup exception");
                                                              },
                                                              [](const PassData&, RenderGraph::PassContext&) {}); });

            Assert::IsFalse(graph.Compile().Succeeded);
            Assert::IsFalse(HasDiagnostic(graph, RenderGraph::DiagnosticCode::BuilderStillOpen));
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::MissingExecuteCallback));

            graph.Reset();
            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(graph.CollectDiagnostics().empty());
        }
    };

} // namespace NyxRenderGraphTests

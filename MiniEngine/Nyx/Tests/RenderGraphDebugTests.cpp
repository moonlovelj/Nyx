#include "RenderGraphTestSupport.h"

namespace NyxRenderGraphTests
{
    TEST_CLASS(ExecutionAndVisualizationTests)
    {
      public:
        TEST_METHOD(CallbacksRunInCompiledOrderAndPassContextIsScoped)
        {
            int externalValue = 42;
            int undeclaredValue = 13;
            RenderGraph::Graph graph;
            const auto imported = graph.ImportBuffer(
                "Imported",
                BufferDesc(),
                &externalValue,
                RenderGraph::Usage::Common);
            const auto undeclared = graph.ImportBuffer(
                "Undeclared",
                BufferDesc(),
                &undeclaredValue,
                RenderGraph::Usage::Common);
            auto transient = graph.CreateBuffer(
                "Transient",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));

            auto producer = BeginPass(graph, "Producer");
            const auto producerId = producer.GetPassId();
            producer.Read(imported, RenderGraph::Usage::CopySource);
            transient = producer.Write(transient, RenderGraph::Usage::UnorderedAccess);
            SetExecute(producer, [producerId, imported, undeclared, transient, &externalValue](RenderGraph::PassContext& context)
                       {
                auto* order = context.GetUserContext<std::vector<std::string>>();
                order->push_back(std::string(context.GetPassName()));
                Assert::AreEqual(producerId, context.GetPassId());
                Assert::AreEqual<uint32_t>(0, context.GetExecutionIndex());
                Assert::IsTrue(context.IsDeclared(imported));
                Assert::AreEqual(&externalValue, context.GetResource<RenderGraph::ResourceKind::Buffer, int>(imported));
                Assert::IsFalse(context.IsDeclared(undeclared));
                Assert::IsTrue(context.IsDeclared(transient)); });

            auto consumer = BeginPass(graph, "Consumer", RenderGraph::PassFlags::SideEffect);
            const auto consumerId = consumer.GetPassId();
            consumer.Read(transient);
            SetExecute(consumer, [consumerId](RenderGraph::PassContext& context)
                       {
                Assert::AreEqual(consumerId, context.GetPassId());
                Assert::AreEqual<uint32_t>(1, context.GetExecutionIndex());
                context.GetUserContext<std::vector<std::string>>()->push_back(std::string(context.GetPassName())); });

            int deadExecutions = 0;
            auto dead = BeginPass(graph, "Dead");
            SetExecute(dead, [&](RenderGraph::PassContext&)
                       { ++deadExecutions; });

            Assert::IsFalse(ExecuteCallbacks(graph));
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::ExecuteBeforeCompile));
            Assert::IsTrue(graph.Compile().Succeeded);
            std::vector<std::string> order;
            Assert::IsTrue(ExecuteCallbacks(graph, &order));
            Assert::AreEqual<size_t>(2, order.size());
            Assert::AreEqual(std::string("Producer"), order[0]);
            Assert::AreEqual(std::string("Consumer"), order[1]);
            Assert::AreEqual(0, deadExecutions);
        }

        TEST_METHOD(PassContextExposesOnlyVersionsDeclaredByEachAccessMode)
        {
            int external = 42;
            RenderGraph::Graph graph;
            const auto original = graph.ImportBuffer(
                "Imported UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::UnorderedAccess);

            bool readDeclared = false;
            auto reader = BeginPass(graph, "Read", RenderGraph::PassFlags::SideEffect);
            reader.Read(original, RenderGraph::Usage::UnorderedAccess);
            SetExecute(reader, [original, &readDeclared](RenderGraph::PassContext& context)
                       { readDeclared = context.IsDeclared(original); });

            bool writeInputDeclared = true;
            bool writeOutputDeclared = false;
            bool writeOutputResolved = false;
            auto writer = BeginPass(graph, "Discard write", RenderGraph::PassFlags::SideEffect);
            const auto written = writer.Write(original, RenderGraph::Usage::UnorderedAccess);
            SetExecute(writer,
                       [original, written, &external, &writeInputDeclared, &writeOutputDeclared, &writeOutputResolved](RenderGraph::PassContext& context)
                       {
                           writeInputDeclared = context.IsDeclared(original);
                           writeOutputDeclared = context.IsDeclared(written);
                           writeOutputResolved =
                               context.GetResource<RenderGraph::ResourceKind::Buffer, int>(written) == &external;
                       });

            bool readWriteInputDeclared = false;
            bool readWriteOutputDeclared = false;
            auto updater = BeginPass(graph, "Read write", RenderGraph::PassFlags::SideEffect);
            const auto updated = updater.ReadWrite(written, RenderGraph::Usage::UnorderedAccess);
            SetExecute(updater,
                       [written, updated, &readWriteInputDeclared, &readWriteOutputDeclared](RenderGraph::PassContext& context)
                       {
                           readWriteInputDeclared = context.IsDeclared(written);
                           readWriteOutputDeclared = context.IsDeclared(updated);
                       });

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::IsTrue(readDeclared);
            Assert::IsFalse(writeInputDeclared);
            Assert::IsTrue(writeOutputDeclared);
            Assert::IsTrue(writeOutputResolved);
            Assert::IsTrue(readWriteInputDeclared);
            Assert::IsTrue(readWriteOutputDeclared);
        }

        TEST_METHOD(CallbackExceptionDoesNotLeaveExecutionGuardSet)
        {
            RenderGraph::Graph graph;
            bool shouldThrow = true;
            int executions = 0;
            auto pass = BeginPass(graph, "May throw", RenderGraph::PassFlags::SideEffect);
            SetExecute(pass, [&](RenderGraph::PassContext&)
                       {
                ++executions;
                if (shouldThrow)
                    throw std::runtime_error("expected test exception"); });

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::ExpectException<std::runtime_error>([&]()
                                                        { ExecuteCallbacks(graph); });
            shouldThrow = false;
            Assert::IsTrue(ExecuteCallbacks(graph));
            Assert::AreEqual(2, executions);
        }

        TEST_METHOD(UndeclaredRuntimeResolutionFailsExecution)
        {
            int declaredValue = 1;
            int undeclaredValue = 2;
            RenderGraph::Graph graph;
            const auto declared = graph.ImportBuffer(
                "Declared",
                BufferDesc(),
                &declaredValue,
                RenderGraph::Usage::Common);
            const auto undeclared = graph.ImportBuffer(
                "Undeclared",
                BufferDesc(),
                &undeclaredValue,
                RenderGraph::Usage::Common);
            auto pass = BeginPass(graph, "Resolve", RenderGraph::PassFlags::SideEffect);
            pass.Read(declared, RenderGraph::Usage::CopySource);
            SetExecute(pass, [undeclared](RenderGraph::PassContext& context)
                       { Assert::IsNull(context.GetResource<RenderGraph::ResourceKind::Buffer, int>(undeclared)); });

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(ExecuteCallbacks(graph));
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::UndeclaredResourceAccess));
        }

        TEST_METHOD(ExecutionRejectsMutationCompilationAndRecursion)
        {
            RenderGraph::Graph graph("Execution guard");
            bool nestedExecuteResult = true;
            RenderGraph::CompileResult nestedCompileResult;
            auto pass = BeginPass(graph, "Mutating callback", RenderGraph::PassFlags::SideEffect);
            SetExecute(pass, [&](RenderGraph::PassContext&)
                       {
                graph.Reset();
                nestedCompileResult = graph.Compile();
                nestedExecuteResult = ExecuteCallbacks(graph);
                Assert::IsFalse(IsOpen(BeginPass(graph, "Rejected"))); });

            Assert::IsTrue(graph.Compile().Succeeded);
            const uint64_t epoch = graph.GetEpoch();
            Assert::IsFalse(ExecuteCallbacks(graph));
            Assert::AreEqual(epoch, graph.GetEpoch());
            Assert::IsTrue(graph.IsCompiled());
            Assert::IsFalse(nestedCompileResult.Succeeded);
            Assert::IsFalse(nestedExecuteResult);
            Assert::IsTrue(HasDiagnostic(graph, RenderGraph::DiagnosticCode::MutationDuringExecution));
            Assert::AreEqual<size_t>(1, graph.GetExecutionOrder().size());
        }

        TEST_METHOD(GraphMutationInvalidatesPlanAndRepeatedCompileIsDeterministic)
        {
            RenderGraph::Graph graph("Deterministic");
            auto pass = BeginPass(graph, "Root", RenderGraph::PassFlags::SideEffect);
            SetNoop(pass);
            Assert::IsTrue(graph.Compile().Succeeded);
            const std::string first = RenderGraph::Debug::ToHtml(graph.CaptureDebugSnapshot());
            Assert::IsTrue(graph.Compile().Succeeded);
            const std::string second = RenderGraph::Debug::ToHtml(graph.CaptureDebugSnapshot());
            Assert::AreEqual(first, second);

            auto mutation = BeginPass(graph, "Mutation", RenderGraph::PassFlags::SideEffect);
            SetNoop(mutation);
            Assert::IsFalse(graph.IsCompiled());
            Assert::IsFalse(ExecuteCallbacks(graph));
            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(2, graph.GetExecutionOrder().size());
        }

        TEST_METHOD(HtmlIsDeterministicEscapedAndSelfContained)
        {
            int external = 0;
            RenderGraph::Graph graph("</script><script>alert(\"graph\")</script>");
            auto texture = graph.ImportTexture(
                "颜色 \"A\"\n</script><script>alert(\"resource\")</script>",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::Common);

            auto reader = BeginPass(graph, "Read \"quoted\"", RenderGraph::PassFlags::SideEffect);
            reader.Read(texture);
            SetNoop(reader);
            auto writer = BeginPass(graph, "Write", RenderGraph::PassFlags::SideEffect);
            texture = writer.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(writer);
            graph.Export(texture, RenderGraph::Usage::Present);
            Assert::IsTrue(graph.Compile().Succeeded);

            const auto snapshot = graph.CaptureDebugSnapshot();
            const std::string html = RenderGraph::Debug::ToHtml(snapshot);
            Assert::AreEqual(html, RenderGraph::Debug::ToHtml(snapshot));
            Assert::IsTrue(html.find("\\\"A\\\"") != std::string::npos);
            Assert::IsTrue(html.find("\\n") != std::string::npos);
            Assert::IsTrue(html.find("\"kind\":\"WAR\"") != std::string::npos);
            Assert::IsTrue(html.find("Pass DAG") != std::string::npos);
            Assert::IsTrue(html.find("Resource lifetime matrix") != std::string::npos);
            Assert::IsTrue(html.find("Barrier plan") != std::string::npos);
            Assert::IsTrue(html.find("\"memorySlotCount\"") != std::string::npos);
            Assert::IsTrue(html.find("physicalSlot") == std::string::npos);
            Assert::IsTrue(html.find("\\u003cscript\\u003e") != std::string::npos);
            Assert::IsTrue(html.find("</script><script>alert") == std::string::npos);
            Assert::IsTrue(html.find("<script src=") == std::string::npos);
        }

        TEST_METHOD(FailedCompileStillProducesDiagnosticPreview)
        {
            RenderGraph::Graph graph("Invalid graph");
            const auto texture = graph.CreateTexture("Uninitialized", TextureDesc());
            auto pass = BeginPass(graph, "Bad read");
            pass.Read(texture);
            SetNoop(pass);
            Assert::IsFalse(graph.Compile().Succeeded);

            const auto snapshot = graph.CaptureDebugSnapshot();
            Assert::IsTrue(snapshot.State == RenderGraph::CompileState::Failed);
            Assert::IsTrue(
                RenderGraph::Debug::ToHtml(snapshot).find("Diagnostics") != std::string::npos);
            Assert::IsTrue(
                RenderGraph::Debug::ToHtml(snapshot).find("UninitializedRead") != std::string::npos);
        }

        TEST_METHOD(WriteHtmlCreatesSelfContainedPreview)
        {
            RenderGraph::Graph graph("HTML preview");
            Assert::IsTrue(graph.Compile().Succeeded);
            const auto snapshot = graph.CaptureDebugSnapshot();
            const std::string expectedHtml = RenderGraph::Debug::ToHtml(snapshot);
            std::filesystem::path path =
                UniqueArtifactPath("nyx_render_graph_", graph.GetEpoch());
            path.replace_extension(".html");
            std::string error = "stale error";
            Assert::IsTrue(RenderGraph::Debug::WriteHtml(snapshot, path, &error));
            Assert::IsTrue(error.empty());
            Assert::IsTrue(FileSize(path) > 0);
            Assert::AreEqual(expectedHtml, ReadTextFile(path));

            // Existing previews can be refreshed in place.
            Assert::IsTrue(RenderGraph::Debug::WriteHtml(snapshot, path, &error));
            Assert::AreEqual(expectedHtml, ReadTextFile(path));
            std::filesystem::remove(path);
        }

        TEST_METHOD(GenerateRenderGraphVisualization)
        {
            using namespace RenderGraph;

            struct DepthPassData
            {
                TextureHandle Depth;
            };

            struct GeometryPassData
            {
                TextureHandle Depth;
                TextureHandle SceneColor;
            };

            struct BloomPassData
            {
                TextureHandle Input;
                TextureHandle Output;
            };

            struct CompositePassData
            {
                TextureHandle SceneColor;
                TextureHandle Bloom;
                TextureHandle BackBuffer;
            };

            struct DeadPassData
            {
                TextureHandle Output;
            };

            Graph graph("Model Render Graph Preview");

            RenderGraph::TextureDesc sceneDesc;
            sceneDesc.Width = 1920;
            sceneDesc.Height = 1080;
            sceneDesc.Format = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
            sceneDesc.Flags =
                ResourceFlags::AllowRenderTarget |
                ResourceFlags::AllowUnorderedAccess;

            RenderGraph::TextureDesc depthDesc;
            depthDesc.Width = 1920;
            depthDesc.Height = 1080;
            depthDesc.Format = static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT);
            depthDesc.Flags = ResourceFlags::AllowDepthStencil;

            RenderGraph::TextureDesc bloomDesc;
            bloomDesc.Width = 960;
            bloomDesc.Height = 540;
            bloomDesc.Format = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
            bloomDesc.Flags = ResourceFlags::AllowUnorderedAccess;

            RenderGraph::TextureDesc backBufferDesc;
            backBufferDesc.Width = 1920;
            backBufferDesc.Height = 1080;
            backBufferDesc.Format = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
            backBufferDesc.Flags = ResourceFlags::AllowRenderTarget;

            int fakeBackBuffer = 0;
            const TextureHandle backBuffer = graph.ImportTexture(
                "Back Buffer",
                backBufferDesc,
                &fakeBackBuffer,
                Usage::Present);
            const TextureHandle sceneColor = graph.CreateTexture("Scene Color", sceneDesc);
            const TextureHandle sceneDepth = graph.CreateTexture("Scene Depth", depthDesc);
            const TextureHandle bloomA = graph.CreateTexture("Bloom A", bloomDesc);
            const TextureHandle bloomB = graph.CreateTexture("Bloom B", bloomDesc);
            const TextureHandle aliasPreview = graph.CreateTexture("Alias Preview", sceneDesc);
            const TextureHandle culledScratch = graph.CreateTexture("Culled Scratch", bloomDesc);

            const DepthPassData depthPass = graph.AddPass<DepthPassData>(
                "Depth Pre-Pass",
                [sceneDepth](PassBuilder& builder, DepthPassData& data)
                {
                    data.Depth = builder.WriteDepth(sceneDepth);
                },
                [](const DepthPassData&, PassContext&) {});

            const GeometryPassData geometryPass = graph.AddPass<GeometryPassData>(
                "Model Geometry",
                [sceneColor, depth = depthPass.Depth](PassBuilder& builder, GeometryPassData& data)
                {
                    data.Depth = builder.ReadDepth(depth);
                    data.SceneColor = builder.WriteRTV(sceneColor);
                },
                [](const GeometryPassData&, PassContext&) {});

            const BloomPassData bloomExtract = graph.AddPass<BloomPassData>(
                "Bloom Extract",
                [input = geometryPass.SceneColor, bloomA](PassBuilder& builder, BloomPassData& data)
                {
                    data.Input = builder.ReadSRV(input, ShaderStage::Compute);
                    data.Output = builder.WriteUAV(bloomA, ShaderStage::Compute);
                },
                [](const BloomPassData&, PassContext&) {});

            const BloomPassData bloomBlur = graph.AddPass<BloomPassData>(
                "Bloom Blur",
                [input = bloomExtract.Output, bloomB](PassBuilder& builder, BloomPassData& data)
                {
                    data.Input = builder.ReadSRV(input, ShaderStage::Compute);
                    data.Output = builder.WriteUAV(bloomB, ShaderStage::Compute);
                },
                [](const BloomPassData&, PassContext&) {});

            const CompositePassData composite = graph.AddPass<CompositePassData>(
                "Final Composite",
                [scene = geometryPass.SceneColor, bloom = bloomBlur.Output, backBuffer](
                    PassBuilder& builder,
                    CompositePassData& data)
                {
                    data.SceneColor = builder.ReadSRV(scene, ShaderStage::Pixel);
                    data.Bloom = builder.ReadSRV(bloom, ShaderStage::Pixel);
                    data.BackBuffer = builder.WriteRTV(backBuffer);
                },
                [](const CompositePassData&, PassContext&) {});

            graph.AddPass<DeadPassData>(
                "Alias Preview Pass",
                [aliasPreview](PassBuilder& builder, DeadPassData& data)
                {
                    data.Output = builder.WriteRTV(aliasPreview);
                },
                [](const DeadPassData&, PassContext&) {},
                PassFlags::SideEffect);

            graph.AddPass<DeadPassData>(
                "Unused Debug Pass",
                [culledScratch](PassBuilder& builder, DeadPassData& data)
                {
                    data.Output = builder.WriteUAV(culledScratch, ShaderStage::Compute);
                },
                [](const DeadPassData&, PassContext&) {});

            graph.Export(composite.BackBuffer, Usage::Present);

            const CompileResult result = graph.Compile();
            Assert::IsTrue(result.Succeeded);
            Assert::AreEqual<uint32_t>(6, result.LivePassCount);
            Assert::AreEqual<uint32_t>(1, result.CulledPassCount);
            Assert::IsTrue(result.BarrierCount > 0);
            Assert::IsTrue(result.MemorySlotCount > 0);
            Assert::IsTrue(
                CountBarriers(graph, BarrierKind::Aliasing) > 0);

            const std::filesystem::path outputPath =
                std::filesystem::temp_directory_path() / "Nyx_RenderGraph_Preview.html";
            std::string error;
            const bool written = Debug::WriteHtml(
                graph.CaptureDebugSnapshot(),
                outputPath,
                &error);
            if (!written)
                Logger::WriteMessage(error.c_str());
            Assert::IsTrue(written);
            Assert::IsTrue(FileSize(outputPath) > 0);
            Assert::IsTrue(
                Debug::ToHtml(graph.CaptureDebugSnapshot()).find("\"kind\":\"Aliasing\"") !=
                std::string::npos);

            const std::string message =
                "Render Graph preview generated at:\n" + outputPath.string();
            Logger::WriteMessage(message.c_str());
        }

        TEST_METHOD(WriteHtmlReportsFileFailure)
        {
            RenderGraph::Graph graph("HTML preview failure");
            Assert::IsTrue(graph.Compile().Succeeded);
            const auto snapshot = graph.CaptureDebugSnapshot();
            std::filesystem::path path =
                UniqueArtifactPath("nyx_render_graph_failure_", graph.GetEpoch());
            path.replace_extension(".html");
            Assert::IsTrue(std::filesystem::create_directory(path));

            std::string error;
            Assert::IsFalse(RenderGraph::Debug::WriteHtml(snapshot, path, &error));
            Assert::IsFalse(error.empty());
            Assert::IsTrue(std::filesystem::is_directory(path));
            std::filesystem::remove(path);
        }
    };
} // namespace NyxRenderGraphTests

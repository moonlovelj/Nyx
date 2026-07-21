#include "RenderGraphTestSupport.h"

namespace NyxRenderGraphTests
{
    TEST_CLASS(BarrierLifetimeAndReuseTests)
    {
      public:
        TEST_METHOD(SkyboxPatternTransitionsImportedColorAndDepth)
        {
            int sceneColorResource = 0;
            int sceneDepthResource = 0;
            RenderGraph::Graph graph("Skybox pattern");
            const auto sceneColor = graph.ImportTexture(
                "Scene Color",
                TextureDesc(
                    RenderGraph::ResourceFlags::AllowRenderTarget |
                    RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &sceneColorResource,
                RenderGraph::Usage::UnorderedAccess,
                RenderGraph::ShaderStage::Compute);
            const auto sceneDepth = graph.ImportTexture(
                "Scene Depth",
                TextureDesc(RenderGraph::ResourceFlags::AllowDepthStencil),
                &sceneDepthResource,
                RenderGraph::Usage::ShaderResource,
                RenderGraph::ShaderStage::Compute);

            struct PassData
            {
                RenderGraph::TextureHandle SceneColor;
                RenderGraph::TextureHandle SceneDepth;
            };
            const PassData skybox = graph.AddPass<PassData>(
                "Skybox",
                [sceneColor, sceneDepth](RenderGraph::PassBuilder& builder, PassData& data)
                {
                    data.SceneColor = builder.ReadWriteRTV(sceneColor);
                    data.SceneDepth = builder.ReadDepth(sceneDepth);
                },
                [](const PassData&, RenderGraph::PassContext&) {});
            graph.Export(skybox.SceneColor, RenderGraph::Usage::RenderTarget);

            const RenderGraph::CompileResult result = graph.Compile();
            Assert::IsTrue(result.Succeeded);
            Assert::AreEqual<uint32_t>(1, result.LivePassCount);
            Assert::AreEqual<uint32_t>(2, result.BarrierCount);
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Transition));
            Assert::AreEqual<size_t>(
                0,
                CountBarriers(graph, RenderGraph::BarrierKind::FinalTransition));

            const auto& colorBarrier = graph.GetBarriers()[0];
            Assert::IsTrue(colorBarrier.Before.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(colorBarrier.After.UsageType == RenderGraph::Usage::RenderTarget);
            const auto& depthBarrier = graph.GetBarriers()[1];
            Assert::IsTrue(depthBarrier.Before.UsageType == RenderGraph::Usage::ShaderResource);
            Assert::IsTrue(depthBarrier.After.UsageType == RenderGraph::Usage::DepthRead);
        }

        TEST_METHOD(TransitionUavAndFinalBarriersArePlanned)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "UAV",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto writer = BeginPass(graph, "UAV writer");
            texture = writer.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(writer);

            auto reader = BeginPass(graph, "UAV reader", RenderGraph::PassFlags::SideEffect);
            const auto readerId = reader.GetPassId();
            reader.Read(texture, RenderGraph::Usage::UnorderedAccess, RenderGraph::ShaderStage::Compute);
            SetNoop(reader);
            graph.Export(texture, RenderGraph::Usage::ShaderResource);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::UnorderedAccess));
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::FinalTransition));
            const auto& finalBarrier = graph.GetBarriers().back();
            Assert::AreEqual(readerId, finalBarrier.Pass);
            Assert::IsTrue(finalBarrier.AfterPass);
            Assert::IsTrue(finalBarrier.Before.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(finalBarrier.After.UsageType == RenderGraph::Usage::ShaderResource);
        }

        TEST_METHOD(UavWriteExportedAsUavGetsAnEpilogueBarrier)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "Exported UAV",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto writer = BeginPass(graph, "Writer");
            const auto writerId = writer.GetPassId();
            texture = writer.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(writer);
            graph.Export(texture, RenderGraph::Usage::UnorderedAccess);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::UnorderedAccess));
            const auto& barrier = graph.GetBarriers().back();
            Assert::AreEqual(writerId, barrier.Pass);
            Assert::IsTrue(barrier.AfterPass);
            Assert::IsTrue(barrier.Before.UsageType == RenderGraph::Usage::UnorderedAccess);
            Assert::IsTrue(barrier.After.UsageType == RenderGraph::Usage::UnorderedAccess);
        }

        TEST_METHOD(UavReadExportedAsUavGetsAConservativeEpilogueBarrier)
        {
            int external = 0;
            RenderGraph::Graph graph;
            const auto buffer = graph.ImportBuffer(
                "Read UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::UnorderedAccess);
            auto reader = BeginPass(graph, "Reader", RenderGraph::PassFlags::SideEffect);
            reader.ReadUAV(buffer, RenderGraph::ShaderStage::Compute);
            SetNoop(reader);
            graph.Export(buffer, RenderGraph::Usage::UnorderedAccess);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(2, CountBarriers(
                                            graph,
                                            RenderGraph::BarrierKind::UnorderedAccess));
            const auto& epilogue = graph.GetBarriers().back();
            Assert::IsTrue(epilogue.AfterPass);
            Assert::IsTrue(epilogue.After.UsageType == RenderGraph::Usage::UnorderedAccess);
        }

        TEST_METHOD(ImportedOnlyExportCanPlanAGraphEpilogueTransition)
        {
            int external = 0;
            RenderGraph::Graph graph;
            const auto buffer = graph.ImportBuffer(
                "Imported",
                BufferDesc(),
                &external,
                RenderGraph::Usage::Common);
            graph.Export(buffer, RenderGraph::Usage::CopySource);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(0, graph.GetExecutionOrder().size());
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::FinalTransition));
            const auto& barrier = graph.GetBarriers().front();
            Assert::AreEqual(RenderGraph::kInvalidIndex, barrier.Pass);
            Assert::IsTrue(barrier.AfterPass);
            Assert::IsTrue(barrier.Before.UsageType == RenderGraph::Usage::Common);
            Assert::IsTrue(barrier.After.UsageType == RenderGraph::Usage::CopySource);
        }

        TEST_METHOD(ImportedUavBoundaryAndReadWriteHazardsAreConservative)
        {
            int external = 0;
            RenderGraph::Graph writeGraph;
            auto buffer = writeGraph.ImportBuffer(
                "Imported UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::UnorderedAccess);
            auto reader = BeginPass(writeGraph, "Reader", RenderGraph::PassFlags::SideEffect);
            reader.Read(buffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(reader);
            auto writer = BeginPass(writeGraph, "Writer", RenderGraph::PassFlags::SideEffect);
            buffer = writer.Write(buffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(writer);
            writeGraph.Export(buffer);
            Assert::IsTrue(writeGraph.Compile().Succeeded);
            Assert::AreEqual<size_t>(2, CountBarriers(writeGraph, RenderGraph::BarrierKind::UnorderedAccess));

            RenderGraph::Graph readGraph;
            const auto readOnlyBuffer = readGraph.ImportBuffer(
                "Read-only UAV",
                BufferDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess),
                &external,
                RenderGraph::Usage::UnorderedAccess);
            auto firstRead = BeginPass(readGraph, "Read 0", RenderGraph::PassFlags::SideEffect);
            firstRead.Read(readOnlyBuffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(firstRead);
            auto secondRead = BeginPass(readGraph, "Read 1", RenderGraph::PassFlags::SideEffect);
            secondRead.Read(readOnlyBuffer, RenderGraph::Usage::UnorderedAccess);
            SetNoop(secondRead);
            Assert::IsTrue(readGraph.Compile().Succeeded);
            Assert::AreEqual<size_t>(1, CountBarriers(readGraph, RenderGraph::BarrierKind::UnorderedAccess));
        }

        TEST_METHOD(UavTransitionDoesNotAddRedundantUavBarrier)
        {
            RenderGraph::Graph graph;
            auto texture = graph.CreateTexture(
                "UAV to SRV",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess));
            auto writer = BeginPass(graph, "Writer");
            texture = writer.Write(texture, RenderGraph::Usage::UnorderedAccess);
            SetNoop(writer);

            auto reader = BeginPass(graph, "Reader", RenderGraph::PassFlags::SideEffect);
            reader.Read(texture, RenderGraph::Usage::ShaderResource, RenderGraph::ShaderStage::Compute);
            SetNoop(reader);
            graph.Export(texture, RenderGraph::Usage::ShaderResource);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual<size_t>(0, CountBarriers(graph, RenderGraph::BarrierKind::UnorderedAccess));
            Assert::AreEqual<size_t>(2, CountBarriers(graph, RenderGraph::BarrierKind::Transition));
            Assert::AreEqual<size_t>(1, CountBarriers(graph, RenderGraph::BarrierKind::FinalTransition));
        }

        TEST_METHOD(CulledPassProducesNoBarrier)
        {
            int external = 0;
            RenderGraph::Graph graph;
            const auto texture = graph.ImportTexture(
                "Unused",
                TextureDesc(),
                &external,
                RenderGraph::Usage::Common);
            auto dead = BeginPass(graph, "Dead read");
            const auto deadId = dead.GetPassId();
            dead.Read(texture);
            SetNoop(dead);
            auto root = BeginPass(graph, "Root", RenderGraph::PassFlags::SideEffect);
            SetNoop(root);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsFalse(graph.IsPassLive(deadId));
            Assert::AreEqual<size_t>(0, graph.GetBarriers().size());
        }

        TEST_METHOD(NonOverlappingRenderTargetsShareMemorySlot)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget);
            RenderGraph::Graph graph;
            auto a = graph.CreateTexture("A", desc);
            auto b = graph.CreateTexture("B", desc);

            auto writeA = BeginPass(graph, "Write A", RenderGraph::PassFlags::SideEffect);
            a = writeA.WriteRTV(a);
            SetNoop(writeA);
            auto readA = BeginPass(graph, "Read A", RenderGraph::PassFlags::SideEffect);
            readA.Read(a);
            SetNoop(readA);
            auto writeB = BeginPass(graph, "Write B", RenderGraph::PassFlags::SideEffect);
            b = writeB.WriteRTV(b);
            SetNoop(writeB);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(GetMemorySlot(graph, a.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::IsTrue(GetMemorySlot(graph, b.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::AreEqual(
                GetMemorySlot(graph, a.GetResourceIndex()),
                GetMemorySlot(graph, b.GetResourceIndex()));
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
        }

        TEST_METHOD(NonOverlappingRenderTargetAndDepthShareMemorySlot)
        {
            auto depthDesc = TextureDesc(RenderGraph::ResourceFlags::AllowDepthStencil);
            depthDesc.Format = static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT);
            RenderGraph::Graph graph;
            auto color = graph.CreateTexture(
                "Color",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget));
            auto depth = graph.CreateTexture("Depth", depthDesc);

            auto writeColor = BeginPass(graph, "Write color", RenderGraph::PassFlags::SideEffect);
            color = writeColor.WriteRTV(color);
            SetNoop(writeColor);
            auto writeDepth = BeginPass(graph, "Write depth", RenderGraph::PassFlags::SideEffect);
            depth = writeDepth.WriteDepth(depth);
            SetNoop(writeDepth);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual(
                GetMemorySlot(graph, color.GetResourceIndex()),
                GetMemorySlot(graph, depth.GetResourceIndex()));
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
        }

        TEST_METHOD(NonOverlappingBuffersShareMemorySlot)
        {
            RenderGraph::Graph graph;
            auto a = graph.CreateBuffer("A", BufferDesc());
            auto b = graph.CreateBuffer("B", BufferDesc());

            auto writeA = BeginPass(graph, "Write A", RenderGraph::PassFlags::SideEffect);
            a = writeA.CopyDst(a);
            SetNoop(writeA);
            auto writeB = BeginPass(graph, "Write B", RenderGraph::PassFlags::SideEffect);
            b = writeB.CopyDst(b);
            SetNoop(writeB);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual(
                GetMemorySlot(graph, a.GetResourceIndex()),
                GetMemorySlot(graph, b.GetResourceIndex()));
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
        }

        TEST_METHOD(NonOverlappingNonRenderTargetTexturesShareMemorySlot)
        {
            RenderGraph::Graph graph;
            auto a = graph.CreateTexture("A", TextureDesc());
            auto b = graph.CreateTexture(
                "B",
                TextureDesc(RenderGraph::ResourceFlags::AllowUnorderedAccess, 128, 64));

            auto writeA = BeginPass(graph, "Write A", RenderGraph::PassFlags::SideEffect);
            a = writeA.CopyDst(a);
            SetNoop(writeA);
            auto writeB = BeginPass(graph, "Write B", RenderGraph::PassFlags::SideEffect);
            b = writeB.WriteUAV(b);
            SetNoop(writeB);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::AreEqual(
                GetMemorySlot(graph, a.GetResourceIndex()),
                GetMemorySlot(graph, b.GetResourceIndex()));
            Assert::AreEqual<size_t>(
                2,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
        }

        TEST_METHOD(DifferentAliasClassesUseDifferentMemorySlots)
        {
            RenderGraph::Graph graph;
            auto buffer = graph.CreateBuffer("Buffer", BufferDesc());
            auto texture = graph.CreateTexture("Texture", TextureDesc());
            auto renderTarget = graph.CreateTexture(
                "Render Target",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget));

            auto writeBuffer = BeginPass(graph, "Write buffer", RenderGraph::PassFlags::SideEffect);
            buffer = writeBuffer.CopyDst(buffer);
            SetNoop(writeBuffer);
            auto writeTexture = BeginPass(graph, "Write texture", RenderGraph::PassFlags::SideEffect);
            texture = writeTexture.CopyDst(texture);
            SetNoop(writeTexture);
            auto writeRenderTarget = BeginPass(
                graph,
                "Write render target",
                RenderGraph::PassFlags::SideEffect);
            renderTarget = writeRenderTarget.WriteRTV(renderTarget);
            SetNoop(writeRenderTarget);

            Assert::IsTrue(graph.Compile().Succeeded);
            const auto bufferSlot = GetMemorySlot(graph, buffer.GetResourceIndex());
            const auto textureSlot = GetMemorySlot(graph, texture.GetResourceIndex());
            const auto renderTargetSlot =
                GetMemorySlot(graph, renderTarget.GetResourceIndex());
            Assert::IsTrue(bufferSlot != textureSlot);
            Assert::IsTrue(bufferSlot != renderTargetSlot);
            Assert::IsTrue(textureSlot != renderTargetSlot);
            Assert::AreEqual<uint32_t>(3, graph.GetCompileResult().MemorySlotCount);
            Assert::AreEqual<size_t>(
                0,
                CountBarriers(graph, RenderGraph::BarrierKind::Aliasing));
        }

        TEST_METHOD(OverlappingResourcesUseDifferentSlotsAndDescriptorsMayAlias)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget);
            RenderGraph::Graph graph;
            auto a = graph.CreateTexture("A", desc);
            auto b = graph.CreateTexture("B", desc);
            auto different = graph.CreateTexture(
                "Different",
                TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget, 128, 64));
            int externalValue = 0;
            const auto imported = graph.ImportBuffer(
                "Imported",
                BufferDesc(),
                &externalValue,
                RenderGraph::Usage::Common);

            auto writeA = BeginPass(graph, "Write A");
            a = writeA.WriteRTV(a);
            SetNoop(writeA);
            auto writeB = BeginPass(graph, "Write B");
            b = writeB.WriteRTV(b);
            SetNoop(writeB);
            auto consumeBoth = BeginPass(graph, "Consume both", RenderGraph::PassFlags::SideEffect);
            consumeBoth.Read(a);
            consumeBoth.Read(b);
            SetNoop(consumeBoth);
            auto writeDifferent = BeginPass(graph, "Write different", RenderGraph::PassFlags::SideEffect);
            different = writeDifferent.WriteRTV(different);
            SetNoop(writeDifferent);
            auto readImported = BeginPass(graph, "Read imported", RenderGraph::PassFlags::SideEffect);
            readImported.Read(imported, RenderGraph::Usage::CopySource);
            SetNoop(readImported);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(GetMemorySlot(graph, a.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::IsTrue(GetMemorySlot(graph, b.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::IsTrue(GetMemorySlot(graph, different.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::IsTrue(
                GetMemorySlot(graph, a.GetResourceIndex()) !=
                GetMemorySlot(graph, b.GetResourceIndex()));
            Assert::AreEqual(
                GetMemorySlot(graph, a.GetResourceIndex()),
                GetMemorySlot(graph, different.GetResourceIndex()));
            Assert::AreEqual(
                RenderGraph::kInvalidIndex,
                GetMemorySlot(graph, imported.GetResourceIndex()));
            Assert::AreEqual<uint32_t>(2, graph.GetCompileResult().MemorySlotCount);
        }

        TEST_METHOD(AliasingOptionsAndExportedLifetimePreventMemoryReuse)
        {
            const auto desc = TextureDesc(RenderGraph::ResourceFlags::AllowRenderTarget);
            RenderGraph::Graph graph;
            auto a = graph.CreateTexture("A", desc);
            auto b = graph.CreateTexture("B", desc);
            auto passA = BeginPass(graph, "A", RenderGraph::PassFlags::SideEffect);
            a = passA.WriteRTV(a);
            SetNoop(passA);
            auto passB = BeginPass(graph, "B", RenderGraph::PassFlags::SideEffect);
            b = passB.WriteRTV(b);
            SetNoop(passB);

            Assert::IsTrue(graph.Compile().Succeeded);
            Assert::IsTrue(GetMemorySlot(graph, a.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::IsTrue(GetMemorySlot(graph, b.GetResourceIndex()) != RenderGraph::kInvalidIndex);
            Assert::AreEqual(
                GetMemorySlot(graph, a.GetResourceIndex()),
                GetMemorySlot(graph, b.GetResourceIndex()));

            RenderGraph::CompileOptions noReuse;
            noReuse.EnableMemoryAliasing = false;
            Assert::IsTrue(graph.Compile(noReuse).Succeeded);
            Assert::IsTrue(
                GetMemorySlot(graph, a.GetResourceIndex()) !=
                GetMemorySlot(graph, b.GetResourceIndex()));

            RenderGraph::CompileOptions extended;
            extended.ExtendTransientLifetimes = true;
            Assert::IsTrue(graph.Compile(extended).Succeeded);
            Assert::IsTrue(
                GetMemorySlot(graph, a.GetResourceIndex()) !=
                GetMemorySlot(graph, b.GetResourceIndex()));

            RenderGraph::Graph exportedGraph;
            auto exported = exportedGraph.CreateTexture("Exported", desc);
            auto later = exportedGraph.CreateTexture("Later", desc);
            auto exportPass = BeginPass(exportedGraph, "Export pass");
            exported = exportPass.WriteRTV(exported);
            SetNoop(exportPass);
            exportedGraph.Export(exported);
            auto laterPass = BeginPass(exportedGraph, "Later pass", RenderGraph::PassFlags::SideEffect);
            later = laterPass.WriteRTV(later);
            SetNoop(laterPass);
            Assert::IsTrue(exportedGraph.Compile().Succeeded);
            Assert::IsTrue(
                GetMemorySlot(exportedGraph, exported.GetResourceIndex()) !=
                GetMemorySlot(exportedGraph, later.GetResourceIndex()));
        }
    };

} // namespace NyxRenderGraphTests

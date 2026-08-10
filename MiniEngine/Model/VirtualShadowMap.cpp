#include "VirtualShadowMap.h"

#include "CommandBucketer.h"
#include "GeometryStreaming.h"
#include "Renderer.h"
#include "TemporalEffects.h"
#include "../Core/BufferManager.h"
#include "../Core/DepthBuffer.h"
#include "../Core/EngineProfiling.h"
#include "../Core/GpuBuffer.h"
#include "../Core/HierarchicalDepthBuffer.h"
#include "../Core/PipelineState.h"
#include "../Core/ProgramBinder.h"
#include "../Core/ProgramUtils.h"
#include "../Core/Utility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace Graphics;

namespace Renderer::VirtualShadowMap
{
    namespace
    {
        bool s_Initialized = false;

        ComputePSO s_MarkRequestedPagesPSO(L"VSM: Mark Requested Pages");
        std::shared_ptr<Program> s_MarkRequestedPagesProgram;
        ComputePSO s_ReuseRequestedPagesPSO(L"VSM: Reuse Requested Pages");
        ComputePSO s_BuildFreePhysicalPageListPSO(L"VSM: Build Free Physical Page List");
        ComputePSO s_AllocateNewPagesPSO(L"VSM: Allocate New Pages");
        ComputePSO s_MarkPhysicalPageRenderedPSO(L"VSM: Mark Physical Page Rendered");
        ComputePSO s_BuildPhysicalPageViewsPSO(L"VSM: Build Physical Page Views");
        ComputePSO s_PhysicalPageInstanceCullPSOs[2] = {
            ComputePSO(L"VSM: Physical Page Instance Cull Pass 0"),
            ComputePSO(L"VSM: Physical Page Instance Cull Pass 1")
        };
        ComputePSO s_PhysicalPageDAGCullPSOs[2] = {
            ComputePSO(L"VSM: Physical Page DAG Cull Pass 0"),
            ComputePSO(L"VSM: Physical Page DAG Cull Pass 1")
        };
        ComputePSO s_PhysicalPageMeshBufferGenPSO(L"VSM: Physical Page Mesh Buffer Gen");
        GraphicsPSO s_ClearRequestedPhysicalPagePSO(L"VSM: Clear Requested Physical Page");
        MeshShaderPSO s_PhysicalPageDepthPSOs[2] = {
            MeshShaderPSO(L"VSM: Physical Page Depth Pass 0"),
            MeshShaderPSO(L"VSM: Physical Page Depth Pass 1")
        };
        std::shared_ptr<Program> s_ReuseRequestedPagesProgram;
        std::shared_ptr<Program> s_BuildFreePhysicalPageListProgram;
        std::shared_ptr<Program> s_AllocateNewPagesProgram;
        std::shared_ptr<Program> s_MarkPhysicalPageRenderedProgram;
        std::shared_ptr<Program> s_BuildPhysicalPageViewsProgram;
        std::shared_ptr<Program> s_ClearRequestedPhysicalPageProgram;
        std::shared_ptr<Program> s_PhysicalPageInstanceCullPrograms[2];
        std::shared_ptr<Program> s_PhysicalPageDAGCullPrograms[2];
        std::shared_ptr<Program> s_PhysicalPageMeshBufferGenProgram;
        std::shared_ptr<Program> s_PhysicalPageDepthPrograms[2];

        StructuredBuffer s_ShadowViewsGpu;
        StructuredBuffer s_DirectionalAddressesGpu;
        StructuredBuffer s_ProjectionsGpu;
        ByteAddressBuffer s_PageRequestMaskGpu;
        ByteAddressBuffer s_RequestStatisticsGpu;
        std::array<StructuredBuffer, 2> s_PageTablesGpu;
        StructuredBuffer s_PageRenderRequestsGpu;
        StructuredBuffer s_PhysicalPageMetadataGpu;
        StructuredBuffer s_FreePhysicalPagesGpu;
        ByteAddressBuffer s_PhysicalPageUsedMaskGpu;
        ByteAddressBuffer s_PageManagementCountersGpu;
        StructuredBuffer s_PhysicalPageViewsGpu;
        DepthBuffer s_PhysicalPagePool;
        std::array<HierarchicalDepthBuffer, 2> s_PhysicalHZBs;

        std::vector<VsmShadowView> s_Views;
        std::vector<VsmShadowView> s_PreviousViews;
        std::vector<DirectionalVsmAddressGpu> s_DirectionalAddressesGpuData;
        std::vector<VsmProjectionGpu> s_ProjectionsGpuData;
        uint32_t s_CurrentPageTableIndex = 0;
        uint32_t s_CommittedPhysicalHZBIndex = 0;
        uint32_t s_FrameNumber = 0;
        bool s_PhysicalPagePoolInitialized = false;

        void AddPhysicalPagePassRootSRVs(ProgramDesc& desc)
        {
            desc.AddRootBufferSRV("g_VsmPageRenderRequests");
            desc.AddRootBufferSRV("g_VsmPageManagementCounters");
            desc.AddRootBufferSRV("g_VsmPhysicalPageViews");
        }

        void BindPhysicalPagePassResources(ProgramBinder& binder, uint32_t renderRequestIndex)
        {
            binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.SetRootBufferSRV("g_VsmPhysicalPageViews", s_PhysicalPageViewsGpu);
            binder["g_VsmPhysicalPagePass"]["RenderRequestIndex"].Set(renderRequestIndex);
        }

        void SetCommonResources(ProgramBinder& binder, const Renderer::FrameConstants& frame)
        {
            ProgramVar commonResources = binder["g_CommonResources"];
            commonResources["BindlessResourcesBaseIndex"].Set(frame.BindlessResourcesBaseIndex);
            commonResources["FrameIndexMod2"].Set(frame.FrameIndexMod2);
        }

        StructuredBuffer& GetCurrentPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex];
        }

        StructuredBuffer& GetPreviousPageTable()
        {
            return s_PageTablesGpu[s_CurrentPageTableIndex ^ 1u];
        }

        HierarchicalDepthBuffer& GetCommittedPhysicalHZB()
        {
            return s_PhysicalHZBs[s_CommittedPhysicalHZBIndex];
        }

        HierarchicalDepthBuffer& GetPendingPhysicalHZB()
        {
            return s_PhysicalHZBs[s_CommittedPhysicalHZBIndex ^ 1u];
        }

        void CommitPendingPhysicalHZB()
        {
            s_CommittedPhysicalHZBIndex ^= 1u;
        }

        Renderer::HZBResources GetPhysicalHZBResources()
        {
            const uint32_t descriptorBaseIndex = Renderer::GetBindlessResourcesBaseOffset();

            Renderer::HZBResources resources;
            resources.Previous = &GetCommittedPhysicalHZB();
            resources.Current = &GetPendingPhysicalHZB();
            resources.PreviousSRVIndex = descriptorBaseIndex + SRV_VSM_PHYSICAL_HZB0 + s_CommittedPhysicalHZBIndex;
            resources.CurrentSRVIndex = descriptorBaseIndex + SRV_VSM_PHYSICAL_HZB0 +
                (s_CommittedPhysicalHZBIndex ^ 1u);
            return resources;
        }

        void BindPageManagementConstants(ProgramBinder& binder)
        {
            binder["g_VsmPageManagement"]["FrameNumber"].Set(s_FrameNumber);
        }

        void GeneratePendingPhysicalHZB(GraphicsContext& gfxContext)
        {
            ScopedTimer timer(L"VSM: Generate Physical HZB", gfxContext);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GetPendingPhysicalHZB().GenerateHZB(gfxContext, s_PhysicalPagePool);
        }

        void BindPhysicalHZBConstants(ProgramVar constants, const Renderer::HZBResources& hzbResources)
        {
            const float width = static_cast<float>(hzbResources.Current->GetWidth());
            const float height = static_cast<float>(hzbResources.Current->GetHeight());
            constants["HZBSizeAndInv"].Set(DirectX::XMFLOAT4(width, height, 1.0f / width, 1.0f / height));
            constants["PreviousHZB"].Set(SlangDescriptorHandle{ hzbResources.PreviousSRVIndex, 0 });
            constants["CurrentHZB"].Set(SlangDescriptorHandle{ hzbResources.CurrentSRVIndex, 0 });
        }

        bool ArePhysicalPageRenderProgramsReady()
        {
            if (!s_MarkPhysicalPageRenderedProgram || !s_PhysicalPageMeshBufferGenProgram)
                return false;

            for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
            {
                if (!s_PhysicalPageInstanceCullPrograms[passIndex] ||
                    !s_PhysicalPageDAGCullPrograms[passIndex] ||
                    !s_PhysicalPageDepthPrograms[passIndex])
                {
                    return false;
                }
            }
            return true;
        }

        void MarkPhysicalPageRendered(GraphicsContext& gfxContext, uint32_t renderRequestIndex)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_MarkPhysicalPageRenderedProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_MarkPhysicalPageRenderedPSO);
            binder.SetRootBufferSRV("g_VsmPageRenderRequestsSRV", s_PageRenderRequestsGpu);
            binder.SetRootBufferSRV("g_VsmPageManagementCountersSRV", s_PageManagementCountersGpu);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder["g_MarkPhysicalPageRendered"]["RenderRequestIndex"].Set(renderRequestIndex);
            binder.Apply();

            context.Dispatch(1, 1, 1);
            context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();
        }

        void DispatchPhysicalPageCull(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            const Renderer::HZBResources& hzbResources,
            uint32_t renderRequestIndex,
            uint32_t passIndex)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(
                DrawCommandManager::GetPotentialDrawItemsGPU(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                DrawCommandManager::GetTaskQueueStateGPU(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(GetCommittedPhysicalHZB(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(GetPendingPhysicalHZB(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.FlushResourceBarriers();

            context.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());
            context.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                Renderer::s_SamplerHeap.GetHeapPointer());

            {
                ProgramBinder binder(*s_PhysicalPageInstanceCullPrograms[passIndex], context);
                binder.SetRootSignature();
                context.SetPipelineState(s_PhysicalPageInstanceCullPSOs[passIndex]);

                ProgramVar constants = binder["g_InstanceCull"];
                constants["ViewportWidth"].Set(kPageSize);
                constants["ViewportHeight"].Set(kPageSize);
                constants["MaxCommands"].Set(DrawCommandManager::GetNumPotentialDrawItems());
                BindPhysicalHZBConstants(constants, hzbResources);
                BindPhysicalPagePassResources(binder, renderRequestIndex);
                SetCommonResources(binder, frame);
                binder.Apply();

                context.Dispatch1D(DrawCommandManager::GetNumPotentialDrawItems());
            }

            context.TransitionResource(
                DrawCommandManager::GetVisibleMeshletBufferGPU(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                DrawCommandManager::GetCandidateMeshletGPU(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.TransitionResource(
                GeometryStreaming::m_GeometryStreamingRequestMaskGPU,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_PhysicalPageDAGCullPrograms[passIndex], context);
            binder.SetRootSignature();
            context.SetPipelineState(s_PhysicalPageDAGCullPSOs[passIndex]);

            ProgramVar constants = binder["g_DAGCull"];
            constants["PixelErrorThreshold"].Set(Renderer::GetPixelErrorThreshold());
            constants["ViewportWidth"].Set(kPageSize);
            constants["ViewportHeight"].Set(kPageSize);
            BindPhysicalHZBConstants(constants, hzbResources);
            BindPhysicalPagePassResources(binder, renderRequestIndex);
            SetCommonResources(binder, frame);
            binder.SetRootBufferUAV("g_TaskQueueStateUAV", DrawCommandManager::GetTaskQueueStateGPU());
            binder.SetRootBufferUAV("g_TaskQueueUAV", DrawCommandManager::GetTaskQueueGPU());
            binder.SetRootBufferUAV("g_MeshletBatchUAV", DrawCommandManager::GetMeshletBatchGPU());
            binder.SetRootBufferUAV("g_CandidateMeshletUAV", DrawCommandManager::GetCandidateMeshletGPU());
            binder.Apply();

            constexpr uint32_t kDAGCullGroupSize = DAG_CULL_GROUP_SIZE;
            context.Dispatch1D(Renderer::GetDAGCullGroupCount() * kDAGCullGroupSize, kDAGCullGroupSize);
        }

        void BuildPhysicalPageDrawCommand(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            uint32_t passIndex)
        {
            ComputeContext& context = gfxContext.GetComputeContext();
            context.TransitionResource(
                DrawCommandManager::GetTaskQueueStateGPU(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(
                DrawCommandManager::GetIndirectDispatchMeshGPU(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.FlushResourceBarriers();

            ProgramBinder binder(*s_PhysicalPageMeshBufferGenProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_PhysicalPageMeshBufferGenPSO);
            binder["g_MeshBufferGen"]["PassIndex"].Set(passIndex);
            SetCommonResources(binder, frame);
            binder.Apply();
            context.Dispatch1D(1, 1);
        }

        void DrawPhysicalPageDepth(
            GraphicsContext& gfxContext,
            const Renderer::FrameConstants& frame,
            uint32_t renderRequestIndex,
            uint32_t passIndex)
        {
            constexpr D3D12_RESOURCE_STATES kGraphicsShaderResourceState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            gfxContext.TransitionResource(
                DrawCommandManager::GetIndirectDispatchMeshGPU(),
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            gfxContext.TransitionResource(
                DrawCommandManager::GetVisibleMeshletBufferGPU(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(s_PageRenderRequestsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PageManagementCountersGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPageViewsGpu, kGraphicsShaderResourceState);
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.FlushResourceBarriers();

            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                Renderer::s_TextureHeap.GetHeapPointer());
            gfxContext.SetDescriptorHeap(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                Renderer::s_SamplerHeap.GetHeapPointer());
            gfxContext.SetDepthStencilTarget(s_PhysicalPagePool.GetDSV());
            gfxContext.SetViewportAndScissor(0, 0, kPhysicalPoolResolution, kPhysicalPoolResolution);
            gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ProgramBinder binder(*s_PhysicalPageDepthPrograms[passIndex], gfxContext);
            binder.SetRootSignature();
            gfxContext.SetPipelineState(s_PhysicalPageDepthPSOs[passIndex]);
            binder["g_VBufferMesh"]["ViewportWidth"].Set(kPageSize);
            binder["g_VBufferMesh"]["ViewportHeight"].Set(kPageSize);
            BindPhysicalPagePassResources(binder, renderRequestIndex);
            SetCommonResources(binder, frame);
            binder.Apply();

            gfxContext.ExecuteIndirect(
                Renderer::GPUDrivenDrawIndirectCommandSignature,
                DrawCommandManager::GetIndirectDispatchMeshGPU(),
                0,
                1);
        }

        uint32_t FindPreviousPageTableBase(uint32_t stableShadowMapId, uint32_t addressType, uint32_t layer)
        {
            const auto previousView = std::find_if(
                s_PreviousViews.begin(),
                s_PreviousViews.end(),
                [=](const VsmShadowView& view)
                {
                    return view.StableShadowMapId == stableShadowMapId && view.AddressType == addressType &&
                        view.Layer == layer;
                });

            return previousView != s_PreviousViews.end() ? previousView->PageTableBase : kInvalidPageTableBase;
        }

        DirectX::XMFLOAT4 PackFloat4(Math::Vector3 value, float w)
        {
            return DirectX::XMFLOAT4(
                static_cast<float>(value.GetX()),
                static_cast<float>(value.GetY()),
                static_cast<float>(value.GetZ()),
                w);
        }

        DirectX::XMFLOAT4 PackFloat4(Math::Vector4 value)
        {
            return DirectX::XMFLOAT4(
                static_cast<float>(value.GetX()),
                static_cast<float>(value.GetY()),
                static_cast<float>(value.GetZ()),
                static_cast<float>(value.GetW()));
        }

        int32_t CheckedInt32(int64_t value)
        {
            ASSERT(value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max(),
                   "VSM address exceeds the signed 32-bit shader address range.");

            return static_cast<int32_t>(std::clamp(value, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                                                   static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
        }

        int32_t FloorToInt32(float value)
        {
            ASSERT(std::isfinite(value), "VSM address must be finite.");
            if (!std::isfinite(value))
                return 0;

            const double flooredValue = std::floor(static_cast<double>(value));
            const double clampedValue = std::clamp(flooredValue, static_cast<double>(std::numeric_limits<int32_t>::min()),
                                                   static_cast<double>(std::numeric_limits<int32_t>::max()));

            ASSERT(flooredValue == clampedValue, "VSM address exceeds the signed 32-bit shader address range.");

            return static_cast<int32_t>(clampedValue);
        }

        DirectionalVsmAddressGpu BuildDirectionalVsmAddressGpu(
            const DirectionalVsmAddressConstants& addressConstants)
        {
            // Math matrices use DirectXMath's transposed storage while Slang consumes
            // explicit mathematical rows for the dot products below.
            const Math::Matrix3 worldToLightRows = Math::Transpose(addressConstants.WorldToLightRotation);

            DirectionalVsmAddressGpu addressData{};
            addressData.WorldToLightRow0 = PackFloat4(worldToLightRows.GetX(), 0.0f);
            addressData.WorldToLightRow1 = PackFloat4(worldToLightRows.GetY(), 0.0f);
            addressData.WorldToLightRow2 = PackFloat4(worldToLightRows.GetZ(), 0.0f);
            addressData.AddressOriginAndInvWorldUnitsPerPage =
                PackFloat4(addressConstants.AddressOriginWS, addressConstants.InvWorldUnitsPerPage);
            addressData.AddressAndWindowOriginPage = DirectX::XMINT4(
                addressConstants.AddressOriginPage.x,
                addressConstants.AddressOriginPage.y,
                addressConstants.WindowOriginPage.x,
                addressConstants.WindowOriginPage.y);
            return addressData;
        }

        DirectX::XMFLOAT4 BuildDirectionalVirtualProjectionRow(
            const DirectX::XMFLOAT4& worldToLightRow,
            const DirectX::XMFLOAT4& addressOriginAndInvWorldUnitsPerPage,
            float clipOrigin)
        {
            const float scale = 2.0f * addressOriginAndInvWorldUnitsPerPage.w / static_cast<float>(kPageTableDim);
            const DirectX::XMFLOAT4 row(
                worldToLightRow.x * scale,
                worldToLightRow.y * scale,
                worldToLightRow.z * scale,
                0.0f);
            const float translation =
                -(row.x * addressOriginAndInvWorldUnitsPerPage.x +
                  row.y * addressOriginAndInvWorldUnitsPerPage.y +
                  row.z * addressOriginAndInvWorldUnitsPerPage.z) +
                clipOrigin;
            return DirectX::XMFLOAT4(row.x, row.y, row.z, translation);
        }

        VsmProjectionGpu BuildVsmProjectionGpu(
            const Math::Matrix4& viewProjMatrix,
            const Math::Matrix4& prevViewProjMatrix,
            Math::Vector3 viewerPosition,
            uint32_t projectionType)
        {
            const Math::Matrix4 viewProjRows = Math::Transpose(viewProjMatrix);
            const Math::Matrix4 prevViewProjRows = Math::Transpose(prevViewProjMatrix);

            VsmProjectionGpu projectionData{};
            projectionData.ViewProjRow0 = PackFloat4(viewProjRows.GetX());
            projectionData.ViewProjRow1 = PackFloat4(viewProjRows.GetY());
            projectionData.ViewProjRow2 = PackFloat4(viewProjRows.GetZ());
            projectionData.ViewProjRow3 = PackFloat4(viewProjRows.GetW());
            projectionData.PrevViewProjRow0 = PackFloat4(prevViewProjRows.GetX());
            projectionData.PrevViewProjRow1 = PackFloat4(prevViewProjRows.GetY());
            projectionData.PrevViewProjRow2 = PackFloat4(prevViewProjRows.GetZ());
            projectionData.PrevViewProjRow3 = PackFloat4(prevViewProjRows.GetW());
            projectionData.ViewerPositionAndProjectionType =
                PackFloat4(viewerPosition, static_cast<float>(projectionType));
            return projectionData;
        }

        VsmProjectionGpu BuildDirectionalVsmProjectionGpu(
            const DirectionalVsmAddressDesc& desc,
            const DirectionalVsmAddressGpu& addressData)
        {
            VsmProjectionGpu projectionData = BuildVsmProjectionGpu(
                desc.ViewProjMatrix,
                desc.PrevViewProjMatrix,
                desc.FocusPositionWS,
                VSM_PROJECTION_TYPE_ORTHOGRAPHIC);
            projectionData.ViewProjRow0 = BuildDirectionalVirtualProjectionRow(
                addressData.WorldToLightRow0,
                addressData.AddressOriginAndInvWorldUnitsPerPage,
                -1.0f);
            projectionData.ViewProjRow1 = BuildDirectionalVirtualProjectionRow(
                addressData.WorldToLightRow1,
                addressData.AddressOriginAndInvWorldUnitsPerPage,
                1.0f);

            // A directional page keeps a stable XY projection while its address generation is unchanged.
            projectionData.PrevViewProjRow0 = projectionData.ViewProjRow0;
            projectionData.PrevViewProjRow1 = projectionData.ViewProjRow1;
            return projectionData;
        }

        float ComputePerspectiveVsmLodScale(const Math::Matrix4& projMatrix)
        {
            const float scaleX = std::fabs(static_cast<float>(projMatrix.GetX().GetX()));
            const float scaleY = std::fabs(static_cast<float>(projMatrix.GetY().GetY()));
            if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f)
                return 0.0f;

            const float virtualResolution = static_cast<float>(kVirtualResolution);
            return std::min(
                2.0f / (scaleX * virtualResolution),
                2.0f / (scaleY * virtualResolution));
        }

        void UploadFrameData(ComputeContext& context)
        {
            ASSERT(!s_Views.empty());
            if (s_Views.empty())
                return;

            context.WriteBuffer(
                s_ShadowViewsGpu,
                0,
                s_Views.data(),
                s_Views.size() * sizeof(VsmShadowView));

            if (!s_DirectionalAddressesGpuData.empty())
            {
                context.WriteBuffer(
                    s_DirectionalAddressesGpu,
                    0,
                    s_DirectionalAddressesGpuData.data(),
                    s_DirectionalAddressesGpuData.size() * sizeof(DirectionalVsmAddressGpu));
            }

            if (!s_ProjectionsGpuData.empty())
            {
                context.WriteBuffer(
                    s_ProjectionsGpu,
                    0,
                    s_ProjectionsGpuData.data(),
                    s_ProjectionsGpuData.size() * sizeof(VsmProjectionGpu));
            }

            context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    } // namespace

    DirectionalVsmAddressConstants BuildDirectionalVsmAddressConstants(const DirectionalVsmAddressDesc& desc)
    {
        DirectionalVsmAddressConstants constants;
        constants.WorldToLightRotation = desc.WorldToLightRotation;
        constants.LightIndex = desc.LightIndex;
        constants.ClipmapLevel = desc.ClipmapLevel;
        constants.AddressGeneration = desc.AddressGeneration;

        const float worldUnitsPerPage = desc.LevelWorldExtent / static_cast<float>(kPageTableDim);
        ASSERT(std::isfinite(worldUnitsPerPage) && worldUnitsPerPage > 0.0f, "VSM world-units-per-page must be positive and finite.");
        if (!std::isfinite(worldUnitsPerPage) || worldUnitsPerPage <= 0.0f)
            return constants;

        constants.InvWorldUnitsPerPage = 1.0f / worldUnitsPerPage;

        const Math::Vector3 focusPositionLS = desc.WorldToLightRotation * desc.FocusPositionWS;
        const float focusPageX = static_cast<float>(focusPositionLS.GetX()) * constants.InvWorldUnitsPerPage;
        const float focusPageY = -static_cast<float>(focusPositionLS.GetY()) * constants.InvWorldUnitsPerPage;

        constants.AddressOriginPage = DirectX::XMINT2(FloorToInt32(focusPageX), FloorToInt32(focusPageY));
        constants.WindowOriginPage =
            DirectX::XMINT2(CheckedInt32(static_cast<int64_t>(constants.AddressOriginPage.x) - static_cast<int64_t>(kPageTableDim / 2u)),
                            CheckedInt32(static_cast<int64_t>(constants.AddressOriginPage.y) - static_cast<int64_t>(kPageTableDim / 2u)));

        // AddressOriginWS is the world-space point whose light-plane XY lies
        // exactly on the AddressOriginPage boundary. Keeping the focus Z makes
        // the relative world position small along the light direction too.
        const Math::Vector3 addressOriginLS(static_cast<float>(constants.AddressOriginPage.x) * worldUnitsPerPage,
                                            -static_cast<float>(constants.AddressOriginPage.y) * worldUnitsPerPage,
                                            static_cast<float>(focusPositionLS.GetZ()));
        constants.AddressOriginWS = Math::Transpose(desc.WorldToLightRotation) * addressOriginLS;

        return constants;
    }

    bool Initialize()
    {
        if (s_Initialized)
            return true;

        ProgramDesc markRequestedPagesDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MarkVsmPages.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        markRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        markRequestedPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        markRequestedPagesDesc.AddRootBufferSRV("g_VsmProjections");
        markRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRequestMask");
        markRequestedPagesDesc.AddRootBufferUAV("g_VsmRequestStatistics");
        s_MarkRequestedPagesProgram = ProgramUtils::GetProgram(
            markRequestedPagesDesc,
            "VSM: Mark Requested Pages");
        if (!s_MarkRequestedPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkRequestedPagesPSO, *s_MarkRequestedPagesProgram);
        s_MarkRequestedPagesPSO.Finalize();

        const std::string pageManagementShaderPath = Renderer::GetModelShaderPath("VsmPageManagement.slang");

        ProgramDesc reuseRequestedPagesDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "reuseRequestedPages");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        reuseRequestedPagesDesc.AddRootBufferSRV("g_VsmPreviousPageTable");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmCurrentPageTable");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        reuseRequestedPagesDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        s_ReuseRequestedPagesProgram = ProgramUtils::GetProgram(reuseRequestedPagesDesc, "VSM: Reuse Requested Pages");
        if (!s_ReuseRequestedPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_ReuseRequestedPagesPSO, *s_ReuseRequestedPagesProgram);
        s_ReuseRequestedPagesPSO.Finalize();

        ProgramDesc buildFreePhysicalPageListDesc =
            ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "buildFreePhysicalPageList");
        buildFreePhysicalPageListDesc.AddRootBufferSRV("g_VsmPhysicalPageUsedMaskSRV");
        buildFreePhysicalPageListDesc.AddRootBufferUAV("g_VsmFreePhysicalPagesUAV");
        buildFreePhysicalPageListDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        s_BuildFreePhysicalPageListProgram =
            ProgramUtils::GetProgram(buildFreePhysicalPageListDesc, "VSM: Build Free Physical Page List");
        if (!s_BuildFreePhysicalPageListProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildFreePhysicalPageListPSO, *s_BuildFreePhysicalPageListProgram);
        s_BuildFreePhysicalPageListPSO.Finalize();

        ProgramDesc allocateNewPagesDesc = ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "allocateNewPages");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmShadowViews");
        allocateNewPagesDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmPageRequestMask");
        allocateNewPagesDesc.AddRootBufferSRV("g_VsmFreePhysicalPagesSRV");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmCurrentPageTable");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPageRenderRequests");
        allocateNewPagesDesc.AddRootBufferUAV("g_VsmPageManagementCounters");
        s_AllocateNewPagesProgram = ProgramUtils::GetProgram(allocateNewPagesDesc, "VSM: Allocate New Pages");
        if (!s_AllocateNewPagesProgram)
            return false;

        ProgramUtils::SetProgram(s_AllocateNewPagesPSO, *s_AllocateNewPagesProgram);
        s_AllocateNewPagesPSO.Finalize();

        ProgramDesc markPhysicalPageRenderedDesc =
            ProgramUtils::MakeComputeDesc(pageManagementShaderPath, "markPhysicalPageRendered");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmPageRenderRequestsSRV");
        markPhysicalPageRenderedDesc.AddRootBufferSRV("g_VsmPageManagementCountersSRV");
        markPhysicalPageRenderedDesc.AddRootBufferUAV("g_VsmPhysicalPageMetadataUAV");
        s_MarkPhysicalPageRenderedProgram =
            ProgramUtils::GetProgram(markPhysicalPageRenderedDesc, "VSM: Mark Physical Page Rendered");
        if (!s_MarkPhysicalPageRenderedProgram)
            return false;

        ProgramUtils::SetProgram(s_MarkPhysicalPageRenderedPSO, *s_MarkPhysicalPageRenderedProgram);
        s_MarkPhysicalPageRenderedPSO.Finalize();

        ProgramDesc buildPhysicalPageViewsDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("BuildVsmPhysicalPageViews.slang"),
            "computeMain");
        buildPhysicalPageViewsDesc.AddRootBufferSRV("g_VsmShadowViews");
        buildPhysicalPageViewsDesc.AddRootBufferSRV("g_DirectionalVsmAddresses");
        buildPhysicalPageViewsDesc.AddRootBufferSRV("g_VsmProjections");
        buildPhysicalPageViewsDesc.AddRootBufferSRV("g_VsmPageRenderRequests");
        buildPhysicalPageViewsDesc.AddRootBufferSRV("g_VsmPageManagementCounters");
        buildPhysicalPageViewsDesc.AddRootBufferUAV("g_VsmPhysicalPageViews");
        s_BuildPhysicalPageViewsProgram =
            ProgramUtils::GetProgram(buildPhysicalPageViewsDesc, "VSM: Build Physical Page Views");
        if (!s_BuildPhysicalPageViewsProgram)
            return false;

        ProgramUtils::SetProgram(s_BuildPhysicalPageViewsPSO, *s_BuildPhysicalPageViewsProgram);
        s_BuildPhysicalPageViewsPSO.Finalize();

        ProgramDesc clearRequestedPhysicalPageDesc = ProgramUtils::MakeGraphicsDesc(
            Renderer::GetModelShaderPath("ClearVsmPhysicalPage.slang"),
            "vertexMain",
            "");
        clearRequestedPhysicalPageDesc.AddRootBufferSRV("g_VsmPageRenderRequests");
        clearRequestedPhysicalPageDesc.AddRootBufferSRV("g_VsmPageManagementCounters");
        s_ClearRequestedPhysicalPageProgram =
            ProgramUtils::GetProgram(clearRequestedPhysicalPageDesc, "VSM: Clear Requested Physical Page");
        if (!s_ClearRequestedPhysicalPageProgram)
            return false;

        D3D12_DEPTH_STENCIL_DESC clearDepthState = DepthStateReadWrite;
        clearDepthState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        s_ClearRequestedPhysicalPagePSO.SetRasterizerState(RasterizerTwoSided);
        s_ClearRequestedPhysicalPagePSO.SetDepthStencilState(clearDepthState);
        s_ClearRequestedPhysicalPagePSO.SetBlendState(BlendDisable);
        s_ClearRequestedPhysicalPagePSO.SetInputLayout(0, nullptr);
        s_ClearRequestedPhysicalPagePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        s_ClearRequestedPhysicalPagePSO.SetDepthTargetFormat(DXGI_FORMAT_D32_FLOAT);
        ProgramUtils::SetProgram(s_ClearRequestedPhysicalPagePSO, *s_ClearRequestedPhysicalPageProgram);
        s_ClearRequestedPhysicalPagePSO.Finalize();

        SamplerDesc physicalPageHZBSampler;
        physicalPageHZBSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        physicalPageHZBSampler.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            const std::string passIndexString = std::to_string(passIndex);
            const std::string instanceCullName = "VSM: Physical Page Instance Cull Pass " + passIndexString;
            const std::string dagCullName = "VSM: Physical Page DAG Cull Pass " + passIndexString;

            ProgramDesc instanceCullDesc = ProgramUtils::MakeComputeDesc(
                Renderer::GetModelShaderPath("InstanceCull.slang"),
                "computeMain",
                ProgramUtils::BindlessMode::ResourceHeap);
            instanceCullDesc.AddDefine("INSTANCE_CULL_PASS_INDEX", passIndexString);
            instanceCullDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            instanceCullDesc.AddStaticSampler("g_HZBSampler", physicalPageHZBSampler);
            AddPhysicalPagePassRootSRVs(instanceCullDesc);
            s_PhysicalPageInstanceCullPrograms[passIndex] =
                ProgramUtils::GetProgram(instanceCullDesc, instanceCullName.c_str());
            if (!s_PhysicalPageInstanceCullPrograms[passIndex])
                return false;

            ProgramUtils::SetProgram(
                s_PhysicalPageInstanceCullPSOs[passIndex],
                *s_PhysicalPageInstanceCullPrograms[passIndex]);
            s_PhysicalPageInstanceCullPSOs[passIndex].Finalize();

            ProgramDesc dagCullDesc = ProgramUtils::MakeComputeDesc(
                Renderer::GetModelShaderPath("DAGCull.slang"),
                "computeMain",
                ProgramUtils::BindlessMode::ResourceHeap);
            dagCullDesc.AddDefine("DAG_CULL_PASS_INDEX", passIndexString);
            dagCullDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            dagCullDesc.AddStaticSampler("g_HZBSampler", physicalPageHZBSampler);
            AddPhysicalPagePassRootSRVs(dagCullDesc);
            dagCullDesc.AddRootBufferUAV("g_TaskQueueStateUAV");
            dagCullDesc.AddRootBufferUAV("g_TaskQueueUAV");
            dagCullDesc.AddRootBufferUAV("g_MeshletBatchUAV");
            dagCullDesc.AddRootBufferUAV("g_CandidateMeshletUAV");
            s_PhysicalPageDAGCullPrograms[passIndex] = ProgramUtils::GetProgram(dagCullDesc, dagCullName.c_str());
            if (!s_PhysicalPageDAGCullPrograms[passIndex])
                return false;

            ProgramUtils::SetProgram(
                s_PhysicalPageDAGCullPSOs[passIndex],
                *s_PhysicalPageDAGCullPrograms[passIndex]);
            s_PhysicalPageDAGCullPSOs[passIndex].Finalize();
        }

        ProgramDesc physicalPageMeshBufferGenDesc = ProgramUtils::MakeComputeDesc(
            Renderer::GetModelShaderPath("MeshBufferGen.slang"),
            "computeMain",
            ProgramUtils::BindlessMode::ResourceHeap);
        s_PhysicalPageMeshBufferGenProgram =
            ProgramUtils::GetProgram(physicalPageMeshBufferGenDesc, "VSM: Physical Page Mesh Buffer Gen");
        if (!s_PhysicalPageMeshBufferGenProgram)
            return false;

        ProgramUtils::SetProgram(s_PhysicalPageMeshBufferGenPSO, *s_PhysicalPageMeshBufferGenProgram);
        s_PhysicalPageMeshBufferGenPSO.Finalize();

        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            const std::string passIndexString = std::to_string(passIndex);
            ProgramDesc depthDesc = ProgramUtils::MakeGraphicsDesc(
                Renderer::GetModelShaderPath("VBufferMesh.slang"),
                "",
                "pixelMain",
                "meshMain",
                ProgramUtils::BindlessMode::ResourceAndSamplerHeap);
            depthDesc.AddDefine("VBUFFER_MESH_PASS_INDEX", passIndexString);
            depthDesc.AddDefine("DEPTH_ONLY");
            depthDesc.AddDefine("VSM_PHYSICAL_PAGE_PASS");
            AddPhysicalPagePassRootSRVs(depthDesc);
            const std::string depthName = "VSM: Physical Page Depth Pass " + passIndexString;
            s_PhysicalPageDepthPrograms[passIndex] = ProgramUtils::GetProgram(
                depthDesc,
                depthName.c_str());
            if (!s_PhysicalPageDepthPrograms[passIndex])
                return false;

            MeshShaderPSO& depthPSO = s_PhysicalPageDepthPSOs[passIndex];
            depthPSO.SetRasterizerState(RasterizerShadowTwoSided);
            depthPSO.SetDepthStencilState(DepthStateReadWrite);
            depthPSO.SetBlendState(BlendDisable);
            depthPSO.SetRenderTargetFormats(0, {}, DXGI_FORMAT_D32_FLOAT);
            ProgramUtils::SetProgram(depthPSO, *s_PhysicalPageDepthPrograms[passIndex]);
            depthPSO.Finalize();
        }

        s_ShadowViewsGpu.Create(
            L"VSM Shadow Views",
            kMaxShadowViews,
            sizeof(VsmShadowView));
        s_DirectionalAddressesGpu.Create(
            L"VSM Directional Addresses",
            kMaxShadowViews,
            sizeof(DirectionalVsmAddressGpu));
        s_ProjectionsGpu.Create(
            L"VSM Projections",
            kMaxShadowViews,
            sizeof(VsmProjectionGpu));
        s_PageRequestMaskGpu.Create(
            L"VSM Page Request Mask",
            kMaxShadowViews * kRequestMaskWordCountPerView,
            sizeof(uint32_t));
        s_RequestStatisticsGpu.Create(
            L"VSM Request Statistics",
            kMaxShadowViews,
            VSM_REQUEST_STATISTICS_STRIDE);
        s_PageTablesGpu[0].Create(L"VSM Page Table 0", kMaxShadowViews * kPagesPerView, sizeof(uint32_t));
        s_PageTablesGpu[1].Create(L"VSM Page Table 1", kMaxShadowViews * kPagesPerView, sizeof(uint32_t));
        s_PageRenderRequestsGpu.Create(
            L"VSM Page Render Requests",
            kPhysicalPageCapacity,
            sizeof(VsmPageRenderRequest));
        s_PhysicalPageMetadataGpu.Create(
            L"VSM Physical Page Metadata",
            kPhysicalPageCapacity,
            sizeof(VsmPhysicalPageMetadata));
        s_FreePhysicalPagesGpu.Create(L"VSM Free Physical Pages", kPhysicalPageCapacity, sizeof(uint32_t));
        s_PhysicalPageUsedMaskGpu.Create(
            L"VSM Physical Page Used Mask",
            kPhysicalPageCapacity / kRequestMaskWordBits,
            sizeof(uint32_t));
        s_PageManagementCountersGpu.Create(L"VSM Page Management Counters", 1, VSM_PAGE_MANAGEMENT_COUNTERS_SIZE);
        s_PhysicalPageViewsGpu.Create(
            L"VSM Physical Page Views",
            kPhysicalPageCapacity,
            sizeof(VsmPhysicalPageView));
        s_PhysicalPagePool.Create(
            L"VSM Physical Page Pool",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_D32_FLOAT);
        s_PhysicalHZBs[0].Create(
            L"VSM Physical HZB 0",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_R32_FLOAT);
        s_PhysicalHZBs[1].Create(
            L"VSM Physical HZB 1",
            kPhysicalPoolResolution,
            kPhysicalPoolResolution,
            DXGI_FORMAT_R32_FLOAT);
        Renderer::SetBindlessResourceDescriptor(SRV_VSM_PHYSICAL_HZB0, s_PhysicalHZBs[0].GetSRV());
        Renderer::SetBindlessResourceDescriptor(SRV_VSM_PHYSICAL_HZB1, s_PhysicalHZBs[1].GetSRV());

        s_Views.reserve(kMaxShadowViews);
        s_PreviousViews.reserve(kMaxShadowViews);
        s_DirectionalAddressesGpuData.reserve(kMaxShadowViews);
        s_ProjectionsGpuData.reserve(kMaxShadowViews);
        s_CurrentPageTableIndex = 0;
        s_CommittedPhysicalHZBIndex = 0;
        s_FrameNumber = 0;
        s_PhysicalPagePoolInitialized = false;
        s_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        s_Views.clear();
        s_PreviousViews.clear();
        s_DirectionalAddressesGpuData.clear();
        s_ProjectionsGpuData.clear();

        s_ShadowViewsGpu.Destroy();
        s_DirectionalAddressesGpu.Destroy();
        s_ProjectionsGpu.Destroy();
        s_PageRequestMaskGpu.Destroy();
        s_RequestStatisticsGpu.Destroy();
        s_PageTablesGpu[0].Destroy();
        s_PageTablesGpu[1].Destroy();
        s_PageRenderRequestsGpu.Destroy();
        s_PhysicalPageMetadataGpu.Destroy();
        s_FreePhysicalPagesGpu.Destroy();
        s_PhysicalPageUsedMaskGpu.Destroy();
        s_PageManagementCountersGpu.Destroy();
        s_PhysicalPageViewsGpu.Destroy();
        s_PhysicalPagePool.Destroy();
        s_PhysicalHZBs[0].Destroy();
        s_PhysicalHZBs[1].Destroy();
        s_MarkRequestedPagesProgram.reset();
        s_ReuseRequestedPagesProgram.reset();
        s_BuildFreePhysicalPageListProgram.reset();
        s_AllocateNewPagesProgram.reset();
        s_MarkPhysicalPageRenderedProgram.reset();
        s_BuildPhysicalPageViewsProgram.reset();
        s_ClearRequestedPhysicalPageProgram.reset();
        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            s_PhysicalPageInstanceCullPrograms[passIndex].reset();
            s_PhysicalPageDAGCullPrograms[passIndex].reset();
            s_PhysicalPageDepthPrograms[passIndex].reset();
        }
        s_PhysicalPageMeshBufferGenProgram.reset();
        s_CurrentPageTableIndex = 0;
        s_CommittedPhysicalHZBIndex = 0;
        s_FrameNumber = 0;
        s_PhysicalPagePoolInitialized = false;
        s_Initialized = false;
    }

    void BeginFrame()
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before BeginFrame.");
        if (!s_Initialized)
            return;

        s_PreviousViews.swap(s_Views);
        s_Views.clear();
        s_DirectionalAddressesGpuData.clear();
        s_ProjectionsGpuData.clear();
        s_CurrentPageTableIndex ^= 1u;
        ++s_FrameNumber;
    }

    uint32_t AddDirectionalView(const DirectionalVsmAddressDesc& desc)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before adding views.");
        ASSERT(s_Views.size() < kMaxShadowViews, "Exceeded the VSM shadow-view capacity.");
        ASSERT(s_DirectionalAddressesGpuData.size() < kMaxShadowViews, "Exceeded the directional VSM address capacity.");
        ASSERT(s_ProjectionsGpuData.size() < kMaxShadowViews, "Exceeded the VSM projection capacity.");
        if (!s_Initialized ||
            s_Views.size() >= kMaxShadowViews ||
            s_DirectionalAddressesGpuData.size() >= kMaxShadowViews ||
            s_ProjectionsGpuData.size() >= kMaxShadowViews)
        {
            return kInvalidViewId;
        }

        const DirectionalVsmAddressConstants addressConstants = BuildDirectionalVsmAddressConstants(desc);
        const DirectionalVsmAddressGpu addressData = BuildDirectionalVsmAddressGpu(addressConstants);
        const uint32_t addressDataIndex = static_cast<uint32_t>(s_DirectionalAddressesGpuData.size());
        const uint32_t projectionDataIndex = static_cast<uint32_t>(s_ProjectionsGpuData.size());
        const uint32_t viewId = static_cast<uint32_t>(s_Views.size());

        VsmShadowView view{};
        view.StableShadowMapId = desc.StableShadowMapId;
        view.AddressGeneration = desc.AddressGeneration;
        view.AddressType = VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP;
        view.AddressDataIndex = addressDataIndex;
        view.RequestMaskWordBase = viewId * kRequestMaskWordCountPerView;
        view.PageTableBase = viewId * kPagesPerView;
        view.PreviousPageTableBase = FindPreviousPageTableBase(
            desc.StableShadowMapId,
            VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP,
            desc.ClipmapLevel);
        view.Layer = desc.ClipmapLevel;
        view.LightIndex = desc.LightIndex;
        view.ProjectionDataIndex = projectionDataIndex;
        view.LodScale = addressConstants.InvWorldUnitsPerPage > 0.0f
            ? 1.0f / (addressConstants.InvWorldUnitsPerPage * static_cast<float>(kPageSize))
            : 0.0f;

        s_Views.push_back(view);
        s_DirectionalAddressesGpuData.push_back(addressData);
        s_ProjectionsGpuData.push_back(BuildDirectionalVsmProjectionGpu(desc, addressData));
        return viewId;
    }

    uint32_t AddLocalView(const LocalVsmViewDesc& desc)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before adding views.");
        ASSERT(s_Views.size() < kMaxShadowViews, "Exceeded the VSM shadow-view capacity.");
        ASSERT(s_ProjectionsGpuData.size() < kMaxShadowViews, "Exceeded the VSM projection capacity.");
        if (!s_Initialized || s_Views.size() >= kMaxShadowViews || s_ProjectionsGpuData.size() >= kMaxShadowViews)
            return kInvalidViewId;

        const uint32_t projectionDataIndex = static_cast<uint32_t>(s_ProjectionsGpuData.size());
        const uint32_t viewId = static_cast<uint32_t>(s_Views.size());

        VsmShadowView view{};
        view.StableShadowMapId = desc.StableShadowMapId;
        view.AddressGeneration = desc.AddressGeneration;
        view.AddressType = VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE;
        view.AddressDataIndex = kInvalidViewId;
        view.RequestMaskWordBase = viewId * kRequestMaskWordCountPerView;
        view.PageTableBase = viewId * kPagesPerView;
        view.PreviousPageTableBase = FindPreviousPageTableBase(
            desc.StableShadowMapId,
            VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE,
            desc.Layer);
        view.Layer = desc.Layer;
        view.LightIndex = desc.LightIndex;
        view.ProjectionDataIndex = projectionDataIndex;
        view.LodScale = ComputePerspectiveVsmLodScale(desc.ProjMatrix);

        s_Views.push_back(view);
        s_ProjectionsGpuData.push_back(BuildVsmProjectionGpu(
            desc.ViewProjMatrix,
            desc.PrevViewProjMatrix,
            desc.ViewerPositionWS,
            VSM_PROJECTION_TYPE_PERSPECTIVE));
        return viewId;
    }

    const VsmShadowView& GetView(uint32_t viewId)
    {
        ASSERT(viewId < s_Views.size(), "Invalid VSM shadow-view index.");
        if (viewId >= s_Views.size())
        {
            static const VsmShadowView invalidView{};
            return invalidView;
        }

        return s_Views[viewId];
    }

    bool IsInitialized()
    {
        return s_Initialized;
    }

    uint32_t GetViewCount()
    {
        return static_cast<uint32_t>(s_Views.size());
    }

    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before marking pages.");
        ASSERT(s_MarkRequestedPagesProgram != nullptr);
        if (!s_Initialized || !s_MarkRequestedPagesProgram || s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Mark Requested Pages", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();
        UploadFrameData(context);

        const size_t requestMaskBytes = s_Views.size() * kRequestMaskWordCountPerView * sizeof(uint32_t);
        const size_t statisticsBytes = s_Views.size() * VSM_REQUEST_STATISTICS_STRIDE;
        context.ClearBufferUAV(s_PageRequestMaskGpu, requestMaskBytes, 0);
        context.ClearBufferUAV(s_RequestStatisticsGpu, statisticsBytes, 0);
        context.InsertUAVBarrier(s_PageRequestMaskGpu);
        context.InsertUAVBarrier(s_RequestStatisticsGpu);

        context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
        ProgramBinder binder(*s_MarkRequestedPagesProgram, context);
        binder.SetRootSignature();
        context.SetPipelineState(s_MarkRequestedPagesPSO);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmProjections", s_ProjectionsGpu);
        binder.SetRootBufferUAV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferUAV("g_VsmRequestStatistics", s_RequestStatisticsGpu);

        const Renderer::ViewConstants& viewConstants = receiverView.GetConstants();
        ProgramVar constants = binder["g_MarkVsmPages"];
        constants["InverseViewProjMatrix"].Set(viewConstants.InverseViewProjMatrix);
        constants["ViewportSize"].Set(
            DirectX::XMUINT2(viewConstants.ViewportWidth, viewConstants.ViewportHeight));

        ProgramVar commonResources = binder["g_CommonResources"];
        commonResources["BindlessResourcesBaseIndex"].Set(Renderer::GetBindlessResourcesBaseOffset());
        commonResources["FrameIndexMod2"].Set(TemporalEffects::GetFrameIndexMod2());

        for (uint32_t viewId = 0; viewId < s_Views.size(); ++viewId)
        {
            constants["ViewId"].Set(viewId);
            binder.Apply();
            context.Dispatch2D(viewConstants.ViewportWidth, viewConstants.ViewportHeight);
        }

        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_RequestStatisticsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void AllocateRequestedPages(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before allocating pages.");
        ASSERT(s_ReuseRequestedPagesProgram != nullptr);
        ASSERT(s_BuildFreePhysicalPageListProgram != nullptr);
        ASSERT(s_AllocateNewPagesProgram != nullptr);
        if (!s_Initialized || !s_ReuseRequestedPagesProgram || !s_BuildFreePhysicalPageListProgram ||
            !s_AllocateNewPagesProgram || s_Views.empty())
        {
            return;
        }

        ScopedTimer timer(L"VSM: Manage Physical Pages", gfxContext);

        // Cached pages must survive between frames. Only initialize the pool once;
        // the future page renderer will clear individual physical pages before writing them.
        if (!s_PhysicalPagePoolInitialized)
        {
            gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfxContext.ClearDepth(s_PhysicalPagePool);
            s_PhysicalPagePoolInitialized = true;
        }

        ComputeContext& context = gfxContext.GetComputeContext();
        StructuredBuffer& currentPageTable = GetCurrentPageTable();
        StructuredBuffer& previousPageTable = GetPreviousPageTable();
        const size_t pageTableBytes = s_Views.size() * kPagesPerView * sizeof(uint32_t);
        const size_t physicalPageUsedMaskBytes = kPhysicalPageCapacity / kRequestMaskWordBits * sizeof(uint32_t);
        context.ClearBufferUAV(currentPageTable, pageTableBytes, kInvalidPhysicalPage);
        context.ClearBufferUAV(s_PhysicalPageUsedMaskGpu, physicalPageUsedMaskBytes, 0);
        context.ClearBufferUAV(s_PageManagementCountersGpu, VSM_PAGE_MANAGEMENT_COUNTERS_SIZE, 0);
        context.InsertUAVBarrier(currentPageTable);
        context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRequestMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(previousPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PhysicalPageUsedMaskGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        constexpr uint32_t kPageManagementThreadCount = 64;
        constexpr uint32_t kRequestWordGroupCount =
            (kRequestMaskWordCountPerView + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;
        constexpr uint32_t kPhysicalPageGroupCount =
            (kPhysicalPageCapacity + kPageManagementThreadCount - 1u) / kPageManagementThreadCount;

        {
            ProgramBinder binder(*s_ReuseRequestedPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_ReuseRequestedPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferSRV("g_VsmPreviousPageTable", previousPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmCurrentPageTable", currentPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageUsedMaskUAV", s_PhysicalPageUsedMaskGpu);
            binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            BindPageManagementConstants(binder);
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        context.InsertUAVBarrier(currentPageTable);
        context.InsertUAVBarrier(s_PhysicalPageMetadataGpu);
        context.InsertUAVBarrier(s_PhysicalPageUsedMaskGpu);
        context.InsertUAVBarrier(s_PageRenderRequestsGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);

        context.TransitionResource(s_PhysicalPageUsedMaskGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_FreePhysicalPagesGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        {
            ProgramBinder binder(*s_BuildFreePhysicalPageListProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_BuildFreePhysicalPageListPSO);

            binder.SetRootBufferSRV("g_VsmPhysicalPageUsedMaskSRV", s_PhysicalPageUsedMaskGpu);
            binder.SetRootBufferUAV("g_VsmFreePhysicalPagesUAV", s_FreePhysicalPagesGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            binder.Apply();

            context.Dispatch(kPhysicalPageGroupCount, 1, 1);
        }

        context.InsertUAVBarrier(s_FreePhysicalPagesGpu);
        context.InsertUAVBarrier(s_PageManagementCountersGpu);
        context.TransitionResource(s_FreePhysicalPagesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        {
            ProgramBinder binder(*s_AllocateNewPagesProgram, context);
            binder.SetRootSignature();
            context.SetPipelineState(s_AllocateNewPagesPSO);

            binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
            binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
            binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
            binder.SetRootBufferSRV("g_VsmFreePhysicalPagesSRV", s_FreePhysicalPagesGpu);
            binder.SetRootBufferUAV("g_VsmCurrentPageTable", currentPageTable);
            binder.SetRootBufferUAV("g_VsmPhysicalPageMetadataUAV", s_PhysicalPageMetadataGpu);
            binder.SetRootBufferUAV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
            binder.SetRootBufferUAV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
            BindPageManagementConstants(binder);
            binder.Apply();

            context.Dispatch(kRequestWordGroupCount, static_cast<uint32_t>(s_Views.size()), 1);
        }

        context.TransitionResource(currentPageTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageMetadataGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.FlushResourceBarriers();
    }

    void BuildPhysicalPageViews(GraphicsContext& gfxContext)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before building physical page views.");
        ASSERT(s_BuildPhysicalPageViewsProgram != nullptr);
        if (!s_Initialized || !s_BuildPhysicalPageViewsProgram || s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Build Physical Page Views", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();

        context.TransitionResource(s_ShadowViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_DirectionalAddressesGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_ProjectionsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageRenderRequestsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PageManagementCountersGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.FlushResourceBarriers();

        ProgramBinder binder(*s_BuildPhysicalPageViewsProgram, context);
        binder.SetRootSignature();
        context.SetPipelineState(s_BuildPhysicalPageViewsPSO);

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmProjections", s_ProjectionsGpu);
        binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
        binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
        binder.SetRootBufferUAV("g_VsmPhysicalPageViews", s_PhysicalPageViewsGpu);
        binder.Apply();

        context.Dispatch1D(kPhysicalPageCapacity);
        context.TransitionResource(s_PhysicalPageViewsGpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.FlushResourceBarriers();
    }

    void ClearRequestedPhysicalPage(GraphicsContext& gfxContext, uint32_t renderRequestIndex)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before clearing a physical page.");
        ASSERT(s_ClearRequestedPhysicalPageProgram != nullptr);
        if (!s_Initialized || !s_ClearRequestedPhysicalPageProgram || s_Views.empty())
            return;

        ScopedTimer timer(L"VSM: Clear Requested Physical Page", gfxContext);

        constexpr D3D12_RESOURCE_STATES kGraphicsShaderResourceState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        gfxContext.TransitionResource(s_PageRenderRequestsGpu, kGraphicsShaderResourceState);
        gfxContext.TransitionResource(s_PageManagementCountersGpu, kGraphicsShaderResourceState);
        gfxContext.TransitionResource(s_PhysicalPagePool, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        gfxContext.FlushResourceBarriers();

        ProgramBinder binder(*s_ClearRequestedPhysicalPageProgram, gfxContext);
        binder.SetRootSignature();
        gfxContext.SetPipelineState(s_ClearRequestedPhysicalPagePSO);
        gfxContext.SetDepthStencilTarget(s_PhysicalPagePool.GetDSV());
        gfxContext.SetViewportAndScissor(0, 0, kPhysicalPoolResolution, kPhysicalPoolResolution);
        gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        binder.SetRootBufferSRV("g_VsmPageRenderRequests", s_PageRenderRequestsGpu);
        binder.SetRootBufferSRV("g_VsmPageManagementCounters", s_PageManagementCountersGpu);
        binder["g_ClearVsmPhysicalPage"]["RenderRequestIndex"].Set(renderRequestIndex);
        binder.Apply();

        gfxContext.Draw(6);
    }

    void RenderRequestedPhysicalPageDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame,
        uint32_t renderRequestIndex)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before rendering a physical page.");
        ASSERT(ArePhysicalPageRenderProgramsReady());
        ASSERT(s_PhysicalPagePoolInitialized, "VSM physical page pool must be initialized before rendering.");
        if (!s_Initialized || !ArePhysicalPageRenderProgramsReady() || !s_PhysicalPagePoolInitialized ||
            s_Views.empty() ||
            DrawCommandManager::GetNumPotentialDrawItems() == 0)
        {
            return;
        }

        ScopedTimer timer(L"VSM: Render Requested Physical Page Depth", gfxContext);
        ComputeContext& context = gfxContext.GetComputeContext();

        for (GpuBuffer& geometryChunk : GeometryStreaming::m_GeometryChunksGPU)
            context.TransitionResource(geometryChunk, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        context.TransitionResource(
            GeometryStreaming::m_GroupDataLocationGPU,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        context.ClearBufferUAV(
            DrawCommandManager::GetTaskQueueStateGPU(),
            DrawCommandManager::GetTaskQueueStateGPU().GetBufferSize(),
            0);
        gfxContext.TransitionResource(DrawCommandManager::GetTaskQueueGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gfxContext.ClearUAV(DrawCommandManager::GetTaskQueueGPU(), 0xffffffffu);
        gfxContext.TransitionResource(DrawCommandManager::GetMeshletBatchGPU(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gfxContext.ClearUAV(DrawCommandManager::GetMeshletBatchGPU(), 0);
        context.InsertUAVBarrier(DrawCommandManager::GetTaskQueueStateGPU());
        context.InsertUAVBarrier(DrawCommandManager::GetTaskQueueGPU());
        context.InsertUAVBarrier(DrawCommandManager::GetMeshletBatchGPU());

        const Renderer::HZBResources hzbResources = GetPhysicalHZBResources();
        for (uint32_t passIndex = 0; passIndex < 2; ++passIndex)
        {
            DispatchPhysicalPageCull(gfxContext, frame, hzbResources, renderRequestIndex, passIndex);
            BuildPhysicalPageDrawCommand(gfxContext, frame, passIndex);
            DrawPhysicalPageDepth(gfxContext, frame, renderRequestIndex, passIndex);
            GeneratePendingPhysicalHZB(gfxContext);
        }

        MarkPhysicalPageRendered(gfxContext, renderRequestIndex);
        CommitPendingPhysicalHZB();
    }

    void BindDebugResources(ProgramBinder& binder)
    {
        ASSERT(s_Initialized, "VirtualShadowMap must be initialized before binding request resources.");
        if (!s_Initialized)
            return;

        binder.SetRootBufferSRV("g_VsmShadowViews", s_ShadowViewsGpu);
        binder.SetRootBufferSRV("g_DirectionalVsmAddresses", s_DirectionalAddressesGpu);
        binder.SetRootBufferSRV("g_VsmPageRequestMask", s_PageRequestMaskGpu);
        binder.SetRootBufferSRV("g_VsmPageTable", GetCurrentPageTable());
        binder.SetRootBufferSRV("g_VsmPhysicalPageMetadata", s_PhysicalPageMetadataGpu);
    }
} // namespace Renderer::VirtualShadowMap

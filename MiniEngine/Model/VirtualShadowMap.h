#pragma once

#include "VirtualShadowMapShared.h"

class GraphicsContext;
class ComputeContext;
class ProgramBinder;

namespace Renderer
{
    struct FrameConstants;
    class RenderView;
}

namespace Renderer::VirtualShadowMap
{
    inline constexpr uint32_t kMaxShadowViews = 256;
    inline constexpr uint32_t kMaxDirectionalClipmaps = 16;
    inline constexpr uint32_t kMaxDirectionalClipmapLevels = 16;
    inline constexpr float kDirectionalClipmapSelectionRadiusScale = 0.25f;

    struct PageStatistics
    {
        uint32_t RequestedPages = 0;
        uint32_t ReusedPages = 0;
        uint32_t NewPages = 0;
        uint32_t OverflowPages = 0;
        uint32_t CoarseMappedPages = 0;
        uint32_t CoarseOverflowPages = 0;
        uint32_t RenderRequests = 0;
        uint32_t FreePagesBeforeAllocation = 0;
        uint32_t RenderBudget = 0;
        uint32_t RenderBacklog = 0;

        bool Ready = false;
        bool PhysicalPoolExhausted = false;
        bool RenderBudgetExceeded = false;
        bool CountersValid = false;
    };

    struct DirectionalVsmClipmapDesc
    {
        Math::Matrix3 WorldToLightRotation = Math::Matrix3(Math::kIdentity);
        Math::Vector3 OriginWS = Math::Vector3(Math::kZero);

        // Every level has the same virtual resolution. Its world extent doubles
        // from FirstLevelExtent for each successive level.
        float FirstLevelExtent = 64.0f;
        uint32_t LevelCount = 1;

        uint32_t LightIndex = 0;
        uint32_t StableShadowMapId = 0;
        uint32_t AddressGeneration = 0;
    };

    struct DirectionalVsmAddressDesc
    {
        Math::Matrix3 WorldToLightRotation = Math::Matrix3(Math::kIdentity);
        Math::Vector3 FocusPositionWS = Math::Vector3(Math::kZero);

        // Full world-space width and height covered by this clipmap level.
        float LevelWorldExtent = 1024.0f;

        uint32_t LightIndex = 0;
        uint32_t StableShadowMapId = 0;
        uint32_t ClipmapLevel = 0;
        uint32_t AddressGeneration = 0;
    };

    // One perspective shadow projection: a spot light, or one point-light cube face/mip.
    struct LocalVsmViewDesc
    {
        Math::Matrix4 ProjMatrix = Math::Matrix4(Math::kIdentity);
        Math::Matrix4 ViewProjMatrix = Math::Matrix4(Math::kIdentity);
        Math::Vector3 ViewerPositionWS = Math::Vector3(Math::kZero);

        uint32_t LightIndex = 0;
        uint32_t StableShadowMapId = 0;
        uint32_t Layer = 0;
        uint32_t AddressGeneration = 0;
    };

    DirectionalVsmAddressConstants BuildDirectionalVsmAddressConstants(const DirectionalVsmAddressDesc& desc);

    bool Initialize();
    void Shutdown();
    void Reset(GraphicsContext& gfxContext);

    void BeginFrame();
    uint32_t AddDirectionalClipmap(const DirectionalVsmClipmapDesc& desc);
    uint32_t AddLocalView(const LocalVsmViewDesc& desc);
    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView);

    // Queues every cached physical page owned by this frame-local view for rerendering.
    // Call after adding the view and before AllocateRequestedPages().
    void MarkViewDirty(uint32_t viewId);
    void MarkClipmapDirty(uint32_t clipmapId);

    void AllocateRequestedPages(GraphicsContext& gfxContext);
    void BuildPhysicalPageViews(GraphicsContext& gfxContext);

    // The batch entry becomes the active path once all selected requests are rendered in the same frame.
    void ClearRequestedPhysicalPages(GraphicsContext& gfxContext);
    void ClearRequestedPhysicalPage(GraphicsContext& gfxContext, uint32_t renderRequestIndex);
    void RenderRequestedPhysicalPagesDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame);
    void RenderRequestedPhysicalPageDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame,
        uint32_t renderRequestIndex);

    void BindSamplingResources(ComputeContext& context, ProgramBinder& binder);

    bool IsInitialized();
    uint32_t GetViewCount();
    const VsmShadowView& GetView(uint32_t viewId);
    const PageStatistics& GetPageStatistics();
} // namespace Renderer::VirtualShadowMap

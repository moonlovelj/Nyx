#pragma once

#include "VirtualShadowMapShared.h"

class GraphicsContext;
class ProgramBinder;

namespace Renderer
{
    struct FrameConstants;
    class RenderView;
}

namespace Renderer::VirtualShadowMap
{
    inline constexpr uint32_t kMaxShadowViews = 256;

    struct DirectionalVsmAddressDesc
    {
        Math::Matrix3 WorldToLightRotation = Math::Matrix3(Math::kIdentity);
        Math::Vector3 FocusPositionWS = Math::Vector3(Math::kZero);
        Math::Matrix4 ViewProjMatrix = Math::Matrix4(Math::kIdentity);

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

    void BeginFrame();
    uint32_t AddDirectionalView(const DirectionalVsmAddressDesc& desc);
    uint32_t AddLocalView(const LocalVsmViewDesc& desc);
    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView);

    // Queues every cached physical page owned by this frame-local view for rerendering.
    // Call after adding the view and before AllocateRequestedPages().
    void MarkViewDirty(uint32_t viewId);

    void AllocateRequestedPages(GraphicsContext& gfxContext);
    void BuildPhysicalPageViews(GraphicsContext& gfxContext);
    void ClearRequestedPhysicalPage(GraphicsContext& gfxContext, uint32_t renderRequestIndex);
    void RenderRequestedPhysicalPageDepth(
        GraphicsContext& gfxContext,
        const Renderer::FrameConstants& frame,
        uint32_t renderRequestIndex);

    void BindDebugResources(ProgramBinder& binder);

    bool IsInitialized();
    uint32_t GetViewCount();
    const VsmShadowView& GetView(uint32_t viewId);
} // namespace Renderer::VirtualShadowMap

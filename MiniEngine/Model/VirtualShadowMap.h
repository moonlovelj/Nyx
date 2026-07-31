#pragma once

#include "VirtualShadowMapShared.h"

class GraphicsContext;
class ProgramBinder;

namespace Renderer
{
    class RenderView;
}

namespace Renderer::VirtualShadowMap
{
    inline constexpr uint32_t kMaxShadowViews = 256;

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

    DirectionalVsmAddressConstants BuildDirectionalVsmAddressConstants(const DirectionalVsmAddressDesc& desc);

    bool Initialize();
    void Shutdown();

    void BeginFrame();
    uint32_t AddDirectionalView(const DirectionalVsmAddressDesc& desc);
    void MarkRequestedPages(GraphicsContext& gfxContext, const Renderer::RenderView& receiverView);
    void AllocateRequestedPages(GraphicsContext& gfxContext);

    void BindPageRequestDebugResources(ProgramBinder& binder);

    bool IsInitialized();
    uint32_t GetViewCount();
    const VsmShadowView& GetView(uint32_t viewId);
} // namespace Renderer::VirtualShadowMap

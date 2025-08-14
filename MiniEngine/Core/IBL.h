#pragma once
#include "CommandContext.h"
#include "TextureManager.h"

namespace IBL
{
    extern DescriptorHandle m_IBLLightingTextures;


    extern const uint32_t g_IBLDiffuseLDMapSize;
    extern const uint32_t g_IBLSpecularLDMapSize;
    extern const uint32_t g_IBLLutSize;

    void InitializeResources(TextureRef IBLHDRI, DescriptorHeap& TextureHeap);

    void Shutdown(void);

    void Precompute(GraphicsContext& gfxContext);
}

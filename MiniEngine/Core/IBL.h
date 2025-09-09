#pragma once
#include "CommandContext.h"
#include "TextureManager.h"

namespace IBL
{
    extern const uint32_t g_IBLCubeMapSize;
    extern const uint32_t g_IBLDiffuseLDMapSize;
    extern const uint32_t g_IBLSpecularLDMapSize;
    extern const uint32_t g_IBLLutSize;

    void InitializeResources(TextureRef IBLHDRI);

    bool IsValid();

    void ChangeIBL(TextureRef IBLHDRI);

    void Shutdown(void);

    void Precompute(GraphicsContext& gfxContext);
}

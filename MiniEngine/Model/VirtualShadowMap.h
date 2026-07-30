#pragma once

#include "VirtualShadowMapShared.h"

class ProgramVar;

namespace Renderer::VirtualShadowMap
{
    struct DirectionalVsmAddressDesc
    {
        Math::Matrix3 WorldToLightRotation = Math::Matrix3(Math::kIdentity);
        Math::Vector3 FocusPositionWS = Math::Vector3(Math::kZero);

        // Full world-space width and height covered by this clipmap level.
        float LevelWorldExtent = 256.0f;

        uint32_t LightIndex = 0;
        uint32_t ClipmapLevel = 0;
        uint32_t AddressGeneration = 0;
    };

    DirectionalVsmAddressConstants BuildDirectionalVsmAddressConstants(const DirectionalVsmAddressDesc& desc);

    // Binding is field-based. Do not raw-copy this structure into a constant buffer because Math::Vector3 uses a
    // SIMD-sized C++ representation.
    void SetDirectionalVsmAddressConstants(const ProgramVar& variable, const DirectionalVsmAddressConstants& addressConstants);
} // namespace Renderer::VirtualShadowMap

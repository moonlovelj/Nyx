#include "VirtualShadowMap.h"

#include "../Core/ProgramBinder.h"
#include "../Core/Utility.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Renderer::VirtualShadowMap
{
    namespace
    {
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

    void SetDirectionalVsmAddressConstants(const ProgramVar& variable, const DirectionalVsmAddressConstants& addressConstants)
    {
        ASSERT(variable.IsValid(), "Directional VSM constant variable is invalid.");
        if (!variable.IsValid())
            return;

        variable["WorldToLightRotation"].Set(addressConstants.WorldToLightRotation);
        variable["InvWorldUnitsPerPage"].Set(addressConstants.InvWorldUnitsPerPage);
        variable["AddressOriginWS"].Set(addressConstants.AddressOriginWS);
        variable["LightIndex"].Set(addressConstants.LightIndex);
        variable["AddressOriginPage"].Set(addressConstants.AddressOriginPage);
        variable["WindowOriginPage"].Set(addressConstants.WindowOriginPage);
        variable["ClipmapLevel"].Set(addressConstants.ClipmapLevel);
        variable["AddressGeneration"].Set(addressConstants.AddressGeneration);
    }
} // namespace Renderer::VirtualShadowMap

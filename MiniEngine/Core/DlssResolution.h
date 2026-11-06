#pragma once

#include "Display.h"
#include "../ThirdParty/DLSS/include/nvsdk_ngx_helpers.h"

// Private to NGX callers. Query on mode/output changes using the capability parameters
// from the initialized device. An empty result falls back to rendering at output size.
namespace DlssBackend
{
    inline PixelSize QueryOptimalRenderSize(NVSDK_NGX_Parameter* capabilities,
        NVSDK_NGX_PerfQuality_Value quality, PixelSize output)
    {
        if (!capabilities || !output.IsValid())
            return {};

        PixelSize optimal = {}, minimum = {}, maximum = {};
        float sharpness = 0.0f;
        const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities, output.width, output.height, quality,
            &optimal.width, &optimal.height, &maximum.width, &maximum.height,
            &minimum.width, &minimum.height, &sharpness);
        if (NVSDK_NGX_FAILED(result) || !optimal.IsValid() ||
            optimal.width > output.width || optimal.height > output.height)
            return {};
        return optimal;
    }
}

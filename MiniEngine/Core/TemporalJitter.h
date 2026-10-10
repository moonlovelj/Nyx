#pragma once

#include "VectorMath.h"

namespace TemporalEffects
{
    // Pixel offsets are positive right/down. Matrix4 uses column-vector composition.
    inline Math::Matrix4 JitterProjection(const Math::Matrix4& projection,
        float jitterX, float jitterY, float width, float height)
    {
        if (width <= 0.0f || height <= 0.0f)
            return projection;

        const Math::Matrix4 translation(
            Math::Vector4(1.0f, 0.0f, 0.0f, 0.0f),
            Math::Vector4(0.0f, 1.0f, 0.0f, 0.0f),
            Math::Vector4(0.0f, 0.0f, 1.0f, 0.0f),
            Math::Vector4(2.0f * jitterX / width, -2.0f * jitterY / height, 0.0f, 1.0f));
        return translation * projection;
    }
}

#ifndef OBJECT_MOTION_SHARED_H
#define OBJECT_MOTION_SHARED_H

#define OBJECT_MOTION_STATIC 0u
#define OBJECT_MOTION_VALID 1u
#define OBJECT_MOTION_INVALID 2u

#ifdef __cplusplus
#include "../../Core/VectorMath.h"
#include <cstdint>
namespace Renderer
{
    struct ObjectMotionData
    {
        Math::Matrix4 WorldToPreviousWorld = Math::Matrix4(Math::kIdentity);
        uint32_t Flags = OBJECT_MOTION_STATIC;
        uint32_t Padding[3] = {};
    };
    static_assert(sizeof(ObjectMotionData) == 80, "ObjectMotionData must match the shader buffer stride.");
}
#else
struct ObjectMotionData
{
    float4x4 WorldToPreviousWorld;
    uint Flags;
    uint3 Padding;
};
#endif

#endif

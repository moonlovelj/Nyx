#pragma once

#include "../Core/Math/BoundingBox.h"
#include <cstdint>

// One object is one Mesh within a ModelInstance. IDs remain stable until scene reload.
struct SceneObjectUpdate
{
    uint32_t ObjectId;
    Math::AxisAlignedBox PreviousBoundsWS;
    Math::AxisAlignedBox CurrentBoundsWS;
};

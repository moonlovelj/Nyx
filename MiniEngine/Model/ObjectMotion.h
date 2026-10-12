#pragma once

#include "Shaders/ObjectMotionShared.h.slang"
#include <cmath>

namespace Renderer
{
    // Advanced only after a scene render is submitted, independently of animation updates.
    class ObjectMotionHistory
    {
    public:
        ObjectMotionData GetMotion(const Math::Matrix4& currentWorld) const
        {
            ObjectMotionData motion;
            motion.Flags = OBJECT_MOTION_INVALID;
            if (!m_HasPreviousWorld || !IsFinite(currentWorld) || !IsFinite(m_PrevWorldMatrix))
                return motion;

            if (DirectX::XMVector4Equal(currentWorld.GetX(), m_PrevWorldMatrix.GetX()) &&
                DirectX::XMVector4Equal(currentWorld.GetY(), m_PrevWorldMatrix.GetY()) &&
                DirectX::XMVector4Equal(currentWorld.GetZ(), m_PrevWorldMatrix.GetZ()) &&
                DirectX::XMVector4Equal(currentWorld.GetW(), m_PrevWorldMatrix.GetW()))
            {
                motion.Flags = OBJECT_MOTION_STATIC;
                return motion;
            }

            const float determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(currentWorld));
            const float basisVolume = static_cast<float>(Math::Length(Math::Vector3(currentWorld.GetX()))) *
                static_cast<float>(Math::Length(Math::Vector3(currentWorld.GetY()))) *
                static_cast<float>(Math::Length(Math::Vector3(currentWorld.GetZ())));
            if (!std::isfinite(determinant) || !std::isfinite(basisVolume) || basisVolume <= 0.0f ||
                std::fabs(determinant) <= basisVolume * 1e-6f)
                return motion;

            motion.WorldToPreviousWorld = m_PrevWorldMatrix * Math::Invert(currentWorld);
            if (IsFinite(motion.WorldToPreviousWorld))
                motion.Flags = OBJECT_MOTION_VALID;
            return motion;
        }

        void CommitRenderedWorld(const Math::Matrix4& world)
        {
            m_PrevWorldMatrix = world;
            m_HasPreviousWorld = IsFinite(world);
        }

    private:
        static bool IsFinite(const Math::Matrix4& matrix)
        {
            const DirectX::XMMATRIX value = matrix;
            return !DirectX::XMMatrixIsNaN(value) && !DirectX::XMMatrixIsInfinite(value);
        }

        Math::Matrix4 m_PrevWorldMatrix = Math::Matrix4(Math::kIdentity);
        bool m_HasPreviousWorld = false;
    };
}

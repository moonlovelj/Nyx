#pragma once

namespace Math { class Camera; class Matrix4; }
class GraphicsContext;
class BoolVar;

namespace XeGTAO
{
    void Initialize( void );
    void Shutdown( void );
    void Render( GraphicsContext& Context, const Math::Camera& camera );
    void Render( GraphicsContext& Context, const Math::Matrix4& renderProjection );

    extern BoolVar Enable;
}


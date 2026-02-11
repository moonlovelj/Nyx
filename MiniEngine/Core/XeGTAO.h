#pragma once

namespace Math { class Camera; }
class GraphicsContext;
class BoolVar;

namespace XeGTAO
{
    void Initialize( void );
    void Shutdown( void );
    void Render( GraphicsContext& Context, const Math::Camera& camera );

    extern BoolVar Enable;
}


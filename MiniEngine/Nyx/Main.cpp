#include "pch.h"
#include "GameCore.h"
#include "GraphicsCore.h"
#include "SystemTime.h"
#include "CommandContext.h"
#include "BufferManager.h"

using namespace GameCore;
using namespace Graphics;

class Nyx : public GameCore::IGameApp
{
public:
    void Startup(void) override;
    void Cleanup(void) override;
    void Update(float deltaT) override;
    void RenderScene(void) override;
};

CREATE_APPLICATION(Nyx)

void Nyx::Startup(void)
{
}

void Nyx::Cleanup(void)
{
}

void Nyx::Update(float /*deltaT*/)
{
    ScopedTimer _prof(L"Update State");
}

void Nyx::RenderScene(void)
{
    GraphicsContext& context = GraphicsContext::Begin(L"Scene Render");
    context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    context.ClearColor(g_SceneColorBuffer);
    context.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    context.SetViewportAndScissor(
        0,
        0,
        g_SceneColorBuffer.GetWidth(),
        g_SceneColorBuffer.GetHeight());
    context.Finish();
}

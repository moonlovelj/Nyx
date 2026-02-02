#pragma once
#include "pch.h"
class GraphicsContext;
struct ID3D12DescriptorHeap;

namespace ImGuiManager
{
    void Initialize(HWND hwnd);
    void Shutdown();
    void NewFrame();
    void Render(GraphicsContext& context);
    ID3D12DescriptorHeap* GetSrvDescriptorHeap();
    bool IsInitialized();
    void NotifyInputCaptured(bool mouse, bool keyboard, bool mouseButtons);
    bool WantsCaptureMouse();
    bool WantsCaptureKeyboard();
    bool WantsCaptureInput();
    bool IsAltHeld();
}

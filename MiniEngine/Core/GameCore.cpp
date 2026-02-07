//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//

#include "pch.h"
#include "GameCore.h"
#include "GraphicsCore.h"
#include "SystemTime.h"
#include "GameInput.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "PostEffects.h"
#include "Display.h"
#include "ImGuiManager.h"
#include "Util/CommandLineArg.h"
#include <shellapi.h>
#include "imgui_impl_win32.h"

#pragma comment(lib, "runtimeobject.lib") 

// Forward declaration per imgui_impl_win32.h guidance.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace GameCore
{
    using namespace Graphics;

    bool gIsSupending = false;
    static bool s_ExitRequested = false;
    extern HWND g_hWnd;

    static void RequestExit()
    {
        s_ExitRequested = true;
    }

    void InitializeApplication( IGameApp& game )
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        CommandLineArgs::Initialize(argc, argv);

        Graphics::Initialize(game.RequiresRaytracingSupport());
        SystemTime::Initialize();
        GameInput::Initialize();
        EngineTuning::Initialize();
        ImGuiManager::Initialize(g_hWnd);

        game.Startup();
    }

    void TerminateApplication( IGameApp& game )
    {
        g_CommandManager.IdleGPU();

        game.Cleanup();

        ImGuiManager::Shutdown();
        GameInput::Shutdown();
    }

    bool UpdateApplication( IGameApp& game )
    {
        float DeltaTime = Graphics::GetFrameTime();
    
        GameInput::Update(DeltaTime);
        ImGuiManager::NewFrame();
        EngineProfiling::Update();
        EngineTuning::Update(DeltaTime);
        
        game.Update(DeltaTime);
        game.RenderScene();

        PostEffects::Render();

        GraphicsContext& UiContext = GraphicsContext::Begin(L"Render UI");
        UiContext.TransitionResource(g_OverlayBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        UiContext.ClearColor(g_OverlayBuffer);
        UiContext.SetRenderTarget(g_OverlayBuffer.GetRTV());
        UiContext.SetViewportAndScissor(0, 0, g_OverlayBuffer.GetWidth(), g_OverlayBuffer.GetHeight());
        game.RenderUI(UiContext);
        UiContext.SetRenderTarget(g_OverlayBuffer.GetRTV());
        UiContext.SetViewportAndScissor(0, 0, g_OverlayBuffer.GetWidth(), g_OverlayBuffer.GetHeight());
        EngineTuning::Display( UiContext, 10.0f, 40.0f, 1900.0f, 1040.0f );
        ImGuiManager::Render(UiContext);

        UiContext.Finish();

        Display::Present();

        return !game.IsDone();
    }

    // Default implementation to be overridden by the application
    bool IGameApp::IsDone( void )
    {
        return s_ExitRequested;
    }

    HWND g_hWnd = nullptr;

    LRESULT CALLBACK WndProc( HWND, UINT, WPARAM, LPARAM );

    static bool IsMouseMessage(UINT message)
    {
        switch (message)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return true;
        default:
            return false;
        }
    }

    static bool IsMouseButtonMessage(UINT message)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
        }
    }

    static bool IsKeyboardMessage(UINT message)
    {
        switch (message)
        {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
        default:
            return false;
        }
    }

    int RunApplication( IGameApp&& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow )
    {
        if (!XMVerifyCPUSupport())
            return 1;

        s_ExitRequested = false;

        Microsoft::WRL::Wrappers::RoInitializeWrapper InitializeWinRT(RO_INIT_MULTITHREADED);
        ASSERT_SUCCEEDED(InitializeWinRT);

        // Register class
        WNDCLASSEX wcex;
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInst;
        wcex.hIcon = LoadIcon(hInst, IDI_APPLICATION);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = className;
        wcex.hIconSm = LoadIcon(hInst, IDI_APPLICATION);
        ASSERT(0 != RegisterClassEx(&wcex), "Unable to register a window");

        // Create window
        RECT rc = { 0, 0, (LONG)g_DisplayWidth, (LONG)g_DisplayHeight };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        g_hWnd = CreateWindow(className, className, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);

        ASSERT(g_hWnd != 0);

        InitializeApplication(app);

        ShowWindow( g_hWnd, nCmdShow/*SW_SHOWDEFAULT*/ );

        do
        {
            MSG msg = {};
            bool done = false;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);

                if (msg.message == WM_QUIT)
                    done = true;
            }

            if (done)
                break;
        }
        while (UpdateApplication(app));	// Returns false to quit loop

        TerminateApplication(app);
        Graphics::Shutdown();
        return 0;
    }

    //--------------------------------------------------------------------------------------
    // Called every time the application receives a message
    //--------------------------------------------------------------------------------------
    LRESULT CALLBACK WndProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
    {
        if (IsMouseButtonMessage(message))
            ImGuiManager::NotifyInputCaptured(false, false, true);

        if (message == WM_SYSKEYDOWN)
        {
            if (wParam == VK_F4)
            {
                RequestExit();
                DestroyWindow(hWnd);
                return 0;
            }
            if (wParam == VK_ESCAPE)
            {
                RequestExit();
                DestroyWindow(hWnd);
                return 0;
            }
            if (wParam == VK_F10)
                return 0;
        }

        if (message == WM_SYSKEYUP && wParam == VK_F10)
            return 0;

        if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
        {
            RequestExit();
            DestroyWindow(hWnd);
            return 0;
        }

        if (message == WM_SYSCHAR || message == WM_SYSDEADCHAR)
            return 0;

        if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
        {
            if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU)
            {
                ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
                ImGuiManager::NotifyInputCaptured(IsMouseMessage(message), IsKeyboardMessage(message), IsMouseButtonMessage(message));
                return 0;
            }
        }

        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU)
            return 0;

        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        {
            ImGuiManager::NotifyInputCaptured(IsMouseMessage(message), IsKeyboardMessage(message), IsMouseButtonMessage(message));
            return true;
        }

        switch( message )
        {
        case WM_CLOSE:
            RequestExit();
            DestroyWindow(hWnd);
            return 0;

        case WM_SIZE:
            Display::Resize((UINT)(UINT64)lParam & 0xFFFF, (UINT)(UINT64)lParam >> 16);
            break;

        case WM_DESTROY:
            RequestExit();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc( hWnd, message, wParam, lParam );
        }

        return 0;
    }

}

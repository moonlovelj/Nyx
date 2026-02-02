#include "pch.h"
#include "ImGuiManager.h"
#include "CommandContext.h"
#include "GraphicsCore.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

namespace
{
    struct DescriptorHeapAllocator
    {
        ID3D12DescriptorHeap* heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = {};
        UINT descriptorSize = 0;
        std::vector<int> freeList;

        void Create(ID3D12Device* device, ID3D12DescriptorHeap* newHeap)
        {
            heap = newHeap;
            D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
            cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
            gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
            descriptorSize = device->GetDescriptorHandleIncrementSize(desc.Type);
            freeList.reserve((int)desc.NumDescriptors);
            for (int i = (int)desc.NumDescriptors - 1; i >= 0; --i)
                freeList.push_back(i);
        }

        void Destroy()
        {
            heap = nullptr;
            freeList.clear();
            cpuStart.ptr = 0;
            gpuStart.ptr = 0;
            descriptorSize = 0;
        }

        void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            ASSERT(!freeList.empty());
            int index = freeList.back();
            freeList.pop_back();
            outCpu->ptr = cpuStart.ptr + (UINT64)index * descriptorSize;
            outGpu->ptr = gpuStart.ptr + (UINT64)index * descriptorSize;
        }

        void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            int index = (int)((cpuHandle.ptr - cpuStart.ptr) / descriptorSize);
            freeList.push_back(index);
        }
    };

    static bool s_Initialized = false;
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> s_SrvHeap;
    static DescriptorHeapAllocator s_SrvHeapAlloc;
    static const UINT kSrvHeapSize = 512;
    static const UINT kFramesInFlight = 3;
    static HWND s_Hwnd = nullptr;
    static bool s_CaptureMouseFromWndProc = false;
    static bool s_CaptureKeyboardFromWndProc = false;
    static bool s_MouseButtonsFromWndProc = false;
    static bool s_LastMouseButtons[5] = { false, false, false, false, false };
    static bool s_AltHeld = false;
}

namespace ImGuiManager
{
    void Initialize(HWND hwnd)
    {
        if (s_Initialized)
            return;

        s_Hwnd = hwnd;
        ImGui_ImplWin32_EnableDpiAwareness();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(hwnd);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kSrvHeapSize;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ASSERT_SUCCEEDED(Graphics::g_Device->CreateDescriptorHeap(&heapDesc, MY_IID_PPV_ARGS(&s_SrvHeap)));

        s_SrvHeapAlloc.Create(Graphics::g_Device, s_SrvHeap.Get());

        ImGui_ImplDX12_InitInfo initInfo = {};
        initInfo.Device = Graphics::g_Device;
        initInfo.CommandQueue = Graphics::g_CommandManager.GetCommandQueue();
        initInfo.NumFramesInFlight = kFramesInFlight;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = s_SrvHeap.Get();
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            s_SrvHeapAlloc.Alloc(outCpu, outGpu);
        };
        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
        {
            s_SrvHeapAlloc.Free(cpuHandle, gpuHandle);
        };
        ImGui_ImplDX12_Init(&initInfo);

        s_Initialized = true;
    }

    void Shutdown()
    {
        if (!s_Initialized)
            return;

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        s_SrvHeapAlloc.Destroy();
        s_SrvHeap.Reset();
        s_Hwnd = nullptr;
        s_Initialized = false;
    }

    void NewFrame()
    {
        if (!s_Initialized)
            return;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        s_AltHeld = io.KeyAlt || ((::GetKeyState(VK_MENU) & 0x8000) != 0);
        io.MouseDrawCursor = s_AltHeld;

        if (s_AltHeld && s_Hwnd != nullptr)
        {
            POINT pos;
            if (::GetCursorPos(&pos) && ::ScreenToClient(s_Hwnd, &pos))
                io.AddMousePosEvent((float)pos.x, (float)pos.y);
        }
        if (!s_MouseButtonsFromWndProc)
        {
            bool buttons[5] =
            {
                (::GetKeyState(VK_LBUTTON) & 0x8000) != 0,
                (::GetKeyState(VK_RBUTTON) & 0x8000) != 0,
                (::GetKeyState(VK_MBUTTON) & 0x8000) != 0,
                (::GetKeyState(VK_XBUTTON1) & 0x8000) != 0,
                (::GetKeyState(VK_XBUTTON2) & 0x8000) != 0,
            };

            for (int i = 0; i < 5; ++i)
            {
                if (buttons[i] != s_LastMouseButtons[i])
                    io.AddMouseButtonEvent(i, buttons[i]);
                s_LastMouseButtons[i] = buttons[i];
            }
        }
        else
        {
            s_LastMouseButtons[0] = (::GetKeyState(VK_LBUTTON) & 0x8000) != 0;
            s_LastMouseButtons[1] = (::GetKeyState(VK_RBUTTON) & 0x8000) != 0;
            s_LastMouseButtons[2] = (::GetKeyState(VK_MBUTTON) & 0x8000) != 0;
            s_LastMouseButtons[3] = (::GetKeyState(VK_XBUTTON1) & 0x8000) != 0;
            s_LastMouseButtons[4] = (::GetKeyState(VK_XBUTTON2) & 0x8000) != 0;
        }

        ImGui::NewFrame();

        s_CaptureMouseFromWndProc = false;
        s_CaptureKeyboardFromWndProc = false;
        s_MouseButtonsFromWndProc = false;
    }

    void Render(GraphicsContext& context)
    {
        if (!s_Initialized)
            return;

        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { s_SrvHeap.Get() };
        context.GetCommandList()->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), context.GetCommandList());
    }

    ID3D12DescriptorHeap* GetSrvDescriptorHeap()
    {
        return s_SrvHeap.Get();
    }

    bool IsInitialized()
    {
        return s_Initialized;
    }

    void NotifyInputCaptured(bool mouse, bool keyboard, bool mouseButtons)
    {
        if (!s_Initialized)
            return;

        s_CaptureMouseFromWndProc |= mouse;
        s_CaptureKeyboardFromWndProc |= keyboard;
        s_MouseButtonsFromWndProc |= mouseButtons;
    }

    bool WantsCaptureMouse()
    {
        if (!s_Initialized)
            return false;

        ImGuiIO& io = ImGui::GetIO();
        return s_AltHeld || s_CaptureMouseFromWndProc || io.WantCaptureMouse;
    }

    bool WantsCaptureKeyboard()
    {
        if (!s_Initialized)
            return false;

        ImGuiIO& io = ImGui::GetIO();
        return s_AltHeld || s_CaptureKeyboardFromWndProc || io.WantCaptureKeyboard;
    }

    bool WantsCaptureInput()
    {
        return WantsCaptureMouse() || WantsCaptureKeyboard();
    }

    bool IsAltHeld()
    {
        if (!s_Initialized)
            return false;

        return s_AltHeld;
    }
}

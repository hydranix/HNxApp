// HNx Packer: the graphical front end.
//
// Platform layer only -- window, D3D11 device, ImGui wiring, message pump. The
// application itself lives in app.cpp.
//
// D3D11 rather than a cross-platform renderer because it ships with Windows, so
// the GUI inherits the property that defines this whole project: one executable,
// nothing to install.
#include "app.hpp"
#include "hnx_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

// Declared by the ImGui Win32 backend; not in its public header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

void releaseRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swapChain, &g_device, &got, &g_context);

    // A machine with no usable GPU -- a bare VM, an RDP session with hardware
    // acceleration off -- still gets a working GUI through the software
    // rasteriser. Slower, but a packaging tool is not a game.
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_device, &got, &g_context);

    if (FAILED(hr)) return false;

    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    releaseRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resizeWidth = LOWORD(lp);
            g_resizeHeight = HIWORD(lp);
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
            return ::DefWindowProcW(hwnd, msg, wp, lp);

        case WM_CLOSE:
            // A build in flight owns worker threads and a half-written output
            // file; closing underneath it would leave both behind.
            if (hnx::gui::confirmClose(hwnd)) ::DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// Follows the system light/dark setting for the title bar, so the window frame
// does not clash with the dark ImGui theme.
void applyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                            sizeof(dark));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HNxPackerWindow";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"HNx Packer " HNX_VERSION_WSTRING,
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1280, 820, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, inst);
        ::MessageBoxW(nullptr,
                      L"Could not create a Direct3D device, so the interface cannot "
                      L"start.\n\nUse hnxpack.exe from a command prompt instead.",
                      L"HNx Packer", MB_OK | MB_ICONERROR);
        return 1;
    }

    applyDarkTitleBar(hwnd);
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Settings are not persisted: the window layout is fixed, so an imgui.ini
    // beside the executable would be litter with nothing useful in it.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    hnx::gui::applyStyle();

    // Scale for the monitor the window opened on. The manifest declares
    // PerMonitorV2, so without this the interface is unusably small on a high-DPI
    // display.
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (dpi > 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(dpi);
        io.FontGlobalScale = dpi;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    hnx::gui::App app;
    app.setWindow(hwnd);

    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // Idle cheaply while minimised.
        //
        // Deliberately IsIconic rather than the DXGI occlusion status the ImGui
        // example uses. On a machine with no GPU -- this project was developed on
        // one, a QEMU guest running the Microsoft Basic Render Driver -- Present
        // returns DXGI_STATUS_OCCLUDED on every frame even while the window is
        // plainly visible. Trusting that flag stops rendering after the first
        // frame and leaves a permanently blank window.
        //
        // IsIconic answers the question actually being asked, and cannot be
        // wrong about it.
        if (::IsIconic(hwnd)) {
            ::Sleep(16);
            continue;
        }

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            releaseRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                       DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            createRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.draw();

        ImGui::Render();
        constexpr float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // VSync on: this is a form, not a game, and spinning at hundreds of frames
        // per second to render static text would be rude on a laptop -- and on a
        // software renderer it is the difference between idle and a busy core.
        //
        // The return value is deliberately not used to gate rendering; see the
        // IsIconic note above.
        g_swapChain->Present(1, 0);
    }

    app.shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}

#include <windows.h>

#include <cstdio>

#include "util/Dpi.h"

namespace {

constexpr wchar_t kWindowClass[] = L"CapturaClipA2.Scaffold";

// Logical (96-DPI) size of the placeholder window.
constexpr int kLogicalWidth = 480;
constexpr int kLogicalHeight = 320;

// Phase 0 scaffold: surface the DPI state in the title bar so the manifest can
// be verified by eye, including while dragging across monitors.
void UpdateTitle(HWND hwnd) noexcept {
    const UINT dpi = ccl::dpi::ForWindow(hwnd);
    const int scale = ::MulDiv(static_cast<int>(dpi), 100, ccl::dpi::kDefaultDpi);

    wchar_t title[128];
    ::swprintf_s(title, L"CapturaClipA2  |  DPI %u (%d%%)  |  Per-Monitor V2: %s",
                 dpi, scale, ccl::dpi::IsPerMonitorV2() ? L"yes" : L"NO");
    ::SetWindowTextW(hwnd, title);
}

void ResizeToLogicalSize(HWND hwnd) noexcept {
    const UINT dpi = ccl::dpi::ForWindow(hwnd);

    RECT desired{0, 0, ccl::dpi::Scale(kLogicalWidth, dpi),
                 ccl::dpi::Scale(kLogicalHeight, dpi)};
    ::AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

    ::SetWindowPos(hwnd, nullptr, 0, 0, desired.right - desired.left,
                   desired.bottom - desired.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            ResizeToLogicalSize(hwnd);
            UpdateTitle(hwnd);
            return 0;

        case WM_DPICHANGED: {
            // Per-Monitor V2 hands us the rect the window should move to.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateTitle(hwnd);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC hdc = ::BeginPaint(hwnd, &ps);
            ::FillRect(hdc, &ps.rcPaint,
                       reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            ::EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ReportFatal(const wchar_t* what) noexcept {
    ::MessageBoxW(nullptr, what, L"CapturaClipA2", MB_ICONERROR | MB_OK);
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR,
                      _In_ int showCommand) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&wc) == 0) {
        ReportFatal(L"Failed to register the window class.");
        return 1;
    }

    const HWND hwnd = ::CreateWindowExW(
        0, kWindowClass, L"CapturaClipA2", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance,
        nullptr);

    if (hwnd == nullptr) {
        ReportFatal(L"Failed to create the window.");
        return 1;
    }

    ::ShowWindow(hwnd, showCommand);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

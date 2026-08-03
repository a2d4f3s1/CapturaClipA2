#include "ui/ColorPreview.h"

#include <cstdio>

#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"CapturaClipA2ColorPreview";

// Pixels of screen shown across the magnifier, and how many screen pixels each
// of them becomes. Odd, so that there is a single middle pixel to mark.
constexpr int kSamplePixels = 15;
constexpr int kZoom = 9;

// Height of the strip under the magnified view that carries the value.
constexpr int kCaptionHeight = 22;

// How far the window sits from the cursor. It has to clear the area being read,
// or the magnifier would end up showing itself.
constexpr int kCursorOffset = 28;

}  // namespace

ColorPreview::~ColorPreview() {
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
    }
}

LRESULT CALLBACK ColorPreview::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam) {
    auto* preview =
        reinterpret_cast<ColorPreview*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            if (preview != nullptr) {
                preview->Paint(dc);
            }
            ::EndPaint(hwnd, &paint);
            return 0;
        }

        // Never takes the pointer: it sits under the cursor while the cursor is
        // busy sampling.
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_NCDESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ColorPreview::EnsureWindow() noexcept {
    if (hwnd_ != nullptr) {
        return true;
    }

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ColorPreview::WndProc;
    wc.hInstance = instance;
    wc.hCursor = nullptr;
    wc.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_EX_NOACTIVATE so that showing it does not take focus away from the
    // window doing the sampling, and WS_EX_TRANSPARENT so it never intercepts
    // the mouse.
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance,
        this);
    return hwnd_ != nullptr;
}

void ColorPreview::Paint(HDC dc) noexcept {
    RECT client{};
    ::GetClientRect(hwnd_, &client);

    const int view = ccl::dpi::Scale(kSamplePixels * kZoom, scale_);
    const int caption = ccl::dpi::Scale(kCaptionHeight, scale_);

    // Read a small patch around the cursor and blow it up. Nearest neighbour,
    // because the point is to see the individual pixels.
    const HDC screen = ::GetDC(nullptr);
    ::SetStretchBltMode(dc, COLORONCOLOR);
    ::StretchBlt(dc, 0, 0, view, view, screen,
                 sample_.x - kSamplePixels / 2, sample_.y - kSamplePixels / 2,
                 kSamplePixels, kSamplePixels, SRCCOPY);
    ::ReleaseDC(nullptr, screen);

    // The pixel that would actually be taken. Drawn as an outline so it does
    // not hide the colour it is pointing at.
    const int cell = view / kSamplePixels;
    const int middle = cell * (kSamplePixels / 2);
    RECT centre{middle, middle, middle + cell, middle + cell};
    ::FrameRect(dc, &centre,
                static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    ::InflateRect(&centre, 1, 1);
    ::FrameRect(dc, &centre,
                static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));

    // The value underneath, on a swatch of the colour itself: reading the hex
    // is slower than seeing it, and both are useful.
    RECT strip{0, view, client.right, view + caption};
    const HBRUSH fill = ::CreateSolidBrush(color_);
    ::FillRect(dc, &strip, fill);
    ::DeleteObject(fill);

    wchar_t text[32];
    ::swprintf_s(text, L"#%02X%02X%02X", GetRValue(color_), GetGValue(color_),
                 GetBValue(color_));

    // Black or white, whichever the swatch will not swallow.
    const int brightness = (GetRValue(color_) * 299 + GetGValue(color_) * 587 +
                            GetBValue(color_) * 114) /
                           1000;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, brightness > 140 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    ::DrawTextW(dc, text, -1, &strip,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ::FrameRect(dc, &client,
                static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
}

void ColorPreview::Update(POINT screen) noexcept {
    if (!EnsureWindow()) {
        return;
    }

    scale_ = ccl::dpi::ForPoint(screen);
    sample_ = screen;

    const HDC screenDc = ::GetDC(nullptr);
    color_ = ::GetPixel(screenDc, screen.x, screen.y);
    ::ReleaseDC(nullptr, screenDc);

    const int view = ccl::dpi::Scale(kSamplePixels * kZoom, scale_);
    const int caption = ccl::dpi::Scale(kCaptionHeight, scale_);
    const int offset = ccl::dpi::Scale(kCursorOffset, scale_);

    // Below and right of the cursor by default, flipped when that would put it
    // off the monitor.
    RECT work{};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(::MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST),
                          &info)) {
        work = info.rcWork;
    }

    int x = screen.x + offset;
    int y = screen.y + offset;
    if (work.right > work.left) {
        if (x + view > work.right) {
            x = screen.x - offset - view;
        }
        if (y + view + caption > work.bottom) {
            y = screen.y - offset - view - caption;
        }
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, view, view + caption,
                   SWP_NOACTIVATE | (visible_ ? 0u : SWP_SHOWWINDOW));
    visible_ = true;

    // Repainted whole: the magnified patch changes with every movement, so
    // there is nothing worth preserving.
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    ::UpdateWindow(hwnd_);
}

void ColorPreview::Hide() noexcept {
    if (hwnd_ != nullptr && visible_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
}

}  // namespace ccl::ui

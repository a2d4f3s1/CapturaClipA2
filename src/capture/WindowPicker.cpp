#include "capture/WindowPicker.h"

#include <dwmapi.h>

namespace ccl::capture {
namespace {

bool IsCloaked(HWND window) noexcept {
    // Windows on another virtual desktop are still "visible" as far as
    // IsWindowVisible is concerned; DWM is what knows they are hidden.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                          sizeof(cloaked)))) {
        return cloaked != FALSE;
    }
    return false;
}

RECT FrameBounds(HWND window) noexcept {
    RECT frame{};
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                          &frame, sizeof(frame)))) {
        return frame;
    }
    ::GetWindowRect(window, &frame);
    return frame;
}

RECT ClientBounds(HWND window) noexcept {
    RECT client{};
    if (!::GetClientRect(window, &client)) {
        return RECT{};
    }

    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    ::ClientToScreen(window, &topLeft);
    ::ClientToScreen(window, &bottomRight);
    return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

BOOL CALLBACK CollectWindow(HWND window, LPARAM data) {
    if (!::IsWindowVisible(window) || ::IsIconic(window) || IsCloaked(window)) {
        return TRUE;
    }

    const RECT frame = FrameBounds(window);
    if (frame.right <= frame.left || frame.bottom <= frame.top) {
        return TRUE;
    }

    WindowInfo info;
    info.handle = window;
    info.frame = frame;
    info.client = ClientBounds(window);

    wchar_t title[256];
    const int length = ::GetWindowTextW(window, title, ARRAYSIZE(title));
    if (length > 0) {
        info.title.assign(title, static_cast<size_t>(length));
    }

    reinterpret_cast<std::vector<WindowInfo>*>(data)->push_back(std::move(info));
    return TRUE;
}

}  // namespace

void WindowList::Capture() noexcept {
    windows_.clear();
    // EnumWindows walks top-level windows in Z order, front to back.
    ::EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&windows_));
}

const WindowInfo* WindowList::Hit(POINT screenPoint) const noexcept {
    for (const WindowInfo& window : windows_) {
        if (::PtInRect(&window.frame, screenPoint)) {
            return &window;
        }
    }
    return nullptr;
}

}  // namespace ccl::capture

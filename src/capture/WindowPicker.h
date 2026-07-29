#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ccl::capture {

struct WindowInfo {
    HWND handle = nullptr;
    // Frame as the user sees it. Taken from DWM rather than GetWindowRect,
    // which reports the drop shadow as part of the window.
    RECT frame{};
    // Client area in screen coordinates: the window without its title bar and
    // borders.
    RECT client{};
    std::wstring title;
};

// The visible top-level windows, recorded in Z order.
//
// This has to be captured before the selection overlay is shown: once the
// overlay covers the screen it becomes the topmost window everywhere, so
// asking the system what is under the cursor would only ever answer "the
// overlay".
class WindowList {
public:
    void Capture() noexcept;

    // Topmost recorded window containing the point, or nullptr.
    const WindowInfo* Hit(POINT screenPoint) const noexcept;

private:
    std::vector<WindowInfo> windows_;
};

}  // namespace ccl::capture

#pragma once

#include <windows.h>

namespace ccl::dpi {

// DPI that Win32 logical units are defined against.
inline constexpr UINT kDefaultDpi = USER_DEFAULT_SCREEN_DPI;

// DPI of the monitor the window currently lives on. Falls back to kDefaultDpi.
UINT ForWindow(HWND hwnd) noexcept;

// DPI of the monitor containing a point in virtual-desktop coordinates.
UINT ForPoint(POINT pt) noexcept;

// True when the process actually runs as Per-Monitor V2, i.e. the manifest took
// effect. Capture accuracy on mixed-DPI setups depends on this being true.
bool IsPerMonitorV2() noexcept;

// 96-DPI logical units -> physical pixels.
inline int Scale(int value, UINT dpi) noexcept {
    return ::MulDiv(value, static_cast<int>(dpi), static_cast<int>(kDefaultDpi));
}

// Physical pixels -> 96-DPI logical units.
inline int Unscale(int value, UINT dpi) noexcept {
    return ::MulDiv(value, static_cast<int>(kDefaultDpi), static_cast<int>(dpi));
}

}  // namespace ccl::dpi

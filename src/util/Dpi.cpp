#include "util/Dpi.h"

#include <shellscalingapi.h>

namespace ccl::dpi {

UINT ForWindow(HWND hwnd) noexcept {
    const UINT dpi = ::GetDpiForWindow(hwnd);
    return dpi != 0 ? dpi : kDefaultDpi;
}

UINT ForPoint(POINT pt) noexcept {
    const HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return kDefaultDpi;
    }

    UINT dpiX = kDefaultDpi;
    UINT dpiY = kDefaultDpi;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return kDefaultDpi;
    }
    return dpiX;
}

bool IsPerMonitorV2() noexcept {
    // V1 and V2 both report DPI_AWARENESS_PER_MONITOR_AWARE, so comparing the
    // awareness value is not enough to tell them apart -- compare the context.
    const DPI_AWARENESS_CONTEXT context = ::GetThreadDpiAwarenessContext();
    return ::AreDpiAwarenessContextsEqual(
               context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

}  // namespace ccl::dpi

#include "capture/ScreenSnapshot.h"

#include <algorithm>
#include <utility>

#include "util/Timing.h"

namespace ccl::capture {

VirtualScreen GetVirtualScreenBounds() noexcept {
    VirtualScreen bounds;
    bounds.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return bounds;
}

ScreenSnapshot::~ScreenSnapshot() { Reset(); }

ScreenSnapshot::ScreenSnapshot(ScreenSnapshot&& other) noexcept
    : bitmap_(std::exchange(other.bitmap_, nullptr)),
      bounds_(std::exchange(other.bounds_, VirtualScreen{})) {}

ScreenSnapshot& ScreenSnapshot::operator=(ScreenSnapshot&& other) noexcept {
    if (this != &other) {
        Reset();
        bitmap_ = std::exchange(other.bitmap_, nullptr);
        bounds_ = std::exchange(other.bounds_, VirtualScreen{});
    }
    return *this;
}

void ScreenSnapshot::Reset() noexcept {
    if (bitmap_ != nullptr) {
        ::DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    bounds_ = VirtualScreen{};
}

bool ScreenSnapshot::Take(UINT delayMs) noexcept {
    Reset();
    ccl::timing::Stopwatch watch;

    if (delayMs > 0) {
        ::Sleep(delayMs);
        watch.Lap(L"  preparation delay");
    }

    bounds_ = GetVirtualScreenBounds();
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return false;
    }

    const HDC screenDc = ::GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }

    const HBITMAP bitmap =
        ::CreateCompatibleBitmap(screenDc, bounds_.width, bounds_.height);
    if (bitmap == nullptr) {
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }
    watch.Lap(L"  snapshot alloc");

    const HDC memoryDc = ::CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        ::DeleteObject(bitmap);
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    const HGDIOBJ previous = ::SelectObject(memoryDc, bitmap);

    // One read for the whole virtual desktop. Reading monitor by monitor was
    // measured as slower despite covering a third less area -- the per-call
    // cost of pulling from the desktop context outweighs the area saved -- and
    // giving each monitor its own device context was slower still.
    //
    // The screen DC spans the whole virtual desktop, so the source origin is
    // the virtual origin rather than (0, 0).
    const BOOL copied = ::BitBlt(memoryDc, 0, 0, bounds_.width, bounds_.height,
                                 screenDc, bounds_.left, bounds_.top, SRCCOPY);
    watch.Lap(L"  screen blit");

    ::SelectObject(memoryDc, previous);
    ::DeleteDC(memoryDc);
    ::ReleaseDC(nullptr, screenDc);

    if (!copied) {
        ::DeleteObject(bitmap);
        return false;
    }
    bitmap_ = bitmap;
    return true;
}

DibBuffer ScreenSnapshot::ExtractRegion(const RECT& area) const noexcept {
    DibBuffer result;
    if (!IsValid()) {
        return result;
    }

    const LONG left = std::clamp(area.left, 0L, static_cast<LONG>(bounds_.width));
    const LONG top = std::clamp(area.top, 0L, static_cast<LONG>(bounds_.height));
    const LONG right =
        std::clamp(area.right, left, static_cast<LONG>(bounds_.width));
    const LONG bottom =
        std::clamp(area.bottom, top, static_cast<LONG>(bounds_.height));

    const int width = static_cast<int>(right - left);
    const int height = static_cast<int>(bottom - top);
    if (width <= 0 || height <= 0 || !result.Create(width, height)) {
        return result;
    }

    const HDC screenDc = ::GetDC(nullptr);
    if (screenDc == nullptr) {
        result.Reset();
        return result;
    }

    const HDC sourceDc = ::CreateCompatibleDC(screenDc);
    const HDC targetDc = ::CreateCompatibleDC(screenDc);
    if (sourceDc == nullptr || targetDc == nullptr) {
        if (sourceDc != nullptr) {
            ::DeleteDC(sourceDc);
        }
        if (targetDc != nullptr) {
            ::DeleteDC(targetDc);
        }
        ::ReleaseDC(nullptr, screenDc);
        result.Reset();
        return result;
    }

    const HGDIOBJ previousSource = ::SelectObject(sourceDc, bitmap_);
    const HGDIOBJ previousTarget = ::SelectObject(targetDc, result.Handle());

    const BOOL copied = ::BitBlt(targetDc, 0, 0, width, height, sourceDc, left,
                                 top, SRCCOPY);

    ::SelectObject(targetDc, previousTarget);
    ::SelectObject(sourceDc, previousSource);
    ::DeleteDC(targetDc);
    ::DeleteDC(sourceDc);
    ::ReleaseDC(nullptr, screenDc);

    if (!copied) {
        result.Reset();
    }
    return result;
}

}  // namespace ccl::capture

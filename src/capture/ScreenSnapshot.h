#pragma once

#include <windows.h>

#include "capture/DibBuffer.h"

namespace ccl::capture {

// Bounds of the virtual desktop in physical pixels. The origin can be negative
// when a secondary monitor sits to the left of or above the primary one.
struct VirtualScreen {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

VirtualScreen GetVirtualScreenBounds() noexcept;

// A frozen copy of the whole desktop, held as a device-dependent bitmap.
//
// Deliberately not a DIB section: reading every pixel of a multi-monitor
// desktop into system memory is a GPU-to-CPU transfer that costs hundreds of
// milliseconds. Kept device-dependent, both the grab and the overlay's redraws
// stay on the GPU, and only the region the user actually selects is ever read
// back.
class ScreenSnapshot {
public:
    ScreenSnapshot() = default;
    ~ScreenSnapshot();

    ScreenSnapshot(ScreenSnapshot&& other) noexcept;
    ScreenSnapshot& operator=(ScreenSnapshot&& other) noexcept;

    ScreenSnapshot(const ScreenSnapshot&) = delete;
    ScreenSnapshot& operator=(const ScreenSnapshot&) = delete;

    // delayMs is the "capture preparation time": a deliberate pause before the
    // grab so that a menu fading out, or an animation still settling, does not
    // end up in the picture.
    bool Take(UINT delayMs) noexcept;
    void Reset() noexcept;

    bool IsValid() const noexcept { return bitmap_ != nullptr; }
    HBITMAP Handle() const noexcept { return bitmap_; }
    int Width() const noexcept { return bounds_.width; }
    int Height() const noexcept { return bounds_.height; }
    const VirtualScreen& Bounds() const noexcept { return bounds_; }

    // Reads one region back into system memory. This is the only place that
    // pays the GPU-to-CPU cost, and only for the pixels that were selected.
    DibBuffer ExtractRegion(const RECT& area) const noexcept;

private:
    HBITMAP bitmap_ = nullptr;
    VirtualScreen bounds_{};
};

}  // namespace ccl::capture

#pragma once

#include <windows.h>

#include <string>

#include "capture/ScreenSnapshot.h"
#include "capture/WindowPicker.h"

namespace ccl::overlay {

struct SelectionResult {
    bool accepted = false;
    // Selected area in snapshot coordinates, i.e. relative to the top-left of
    // the virtual desktop rather than to the primary monitor.
    RECT area{};
    // Timestamp of the mouse release, used to measure the second half of the
    // latency budget: release -> capture window on screen.
    LONGLONG releasedAt = 0;
    // Title of the window the capture came from, used by the %t placeholder.
    std::wstring title;
};

// Runs the region selection modally on top of a frozen snapshot of the screen.
//
// Freezing the screen first is what makes the interaction feel instant: every
// frame after this point is drawn from memory, so the rubber band tracks the
// mouse without waiting on the desktop compositor, and nothing needs to be
// hidden and redrawn before the actual grab.
// Clicking without dragging selects a whole window instead of a region; the
// window list must therefore have been captured before the overlay was shown.
SelectionResult RunSelection(const ccl::capture::ScreenSnapshot& snapshot,
                             const ccl::capture::WindowList& windows,
                             LONGLONG launchStart) noexcept;

}  // namespace ccl::overlay

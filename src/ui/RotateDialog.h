#pragma once

#include <windows.h>

#include <functional>
#include <optional>

namespace ccl::ui {

// Asks how far to turn the picture.
//
// `onAngleChanged` is called every time the angle moves, so the caller can show
// the result on the picture itself at its own size rather than in a thumbnail.
// It is called with zero on the way out, whichever way the dialog ends, so the
// caller is not left having to undo the preview.
//
// Returns the angle in degrees, or nothing when cancelled or when the angle was
// left at zero: turning by nothing would still resample the whole picture,
// which costs quality and changes nothing.
std::optional<float> ShowRotateDialog(
    HWND owner, const std::function<void(float)>& onAngleChanged) noexcept;

}  // namespace ccl::ui

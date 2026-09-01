#pragma once

#include <windows.h>

#include <functional>
#include <optional>

#include "io/ImageOps.h"

namespace ccl::ui {

// Asks where to put the picture from the clipboard against the one open.
//
// `onChanged` is called every time any of the values moves, so the caller can
// show the result on the picture itself rather than in a thumbnail -- the join
// puts the two side by side, so a preview small enough to fit in the dialog
// would be too small to judge. It is called once more on the way out, with the
// values the dialog started from, so the caller is not left holding a preview
// it has to remember to take down.
//
// Returns where to place it, or nothing when cancelled.
std::optional<ccl::io::ConcatPlacement> ShowConcatDialog(
    HWND owner, const ccl::io::ConcatPlacement& initial,
    const std::function<void(const ccl::io::ConcatPlacement&)>&
        onChanged) noexcept;

}  // namespace ccl::ui

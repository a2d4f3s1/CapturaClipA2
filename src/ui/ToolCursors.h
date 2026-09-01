#pragma once

#include <windows.h>

namespace ccl::ui {

// Which of the four selecting tools a cursor stands for.
//
// The four are told apart by two things at once, so that neither has to be
// read on its own: the shape says how the area is drawn -- a box for the one
// dragged out by its corners, a lopsided loop for the one drawn by hand -- and
// whether it is filled says what is being picked, an area of the picture or
// the pieces that were drawn on it.
enum class SelectCursor {
    Rect,         // W
    Lasso,        // L
    ObjectRect,   // Ctrl+W
    ObjectLasso,  // Ctrl+L
    Count,
};

// The eight cursors the selecting tools use: each of the four in its own
// colour, and again in red for while Alt is held.
//
// Built in code rather than kept in the resources. The resource script carries
// only what the shell reads out of the file for itself -- the application icon
// and the version -- and a cursor is not one of those. Drawing them here also
// means they can follow the system's cursor size, which a fixed-size resource
// could not.
//
// Built once and kept: swapping a cursor happens on every mouse move, and is
// not the place to be drawing anything.
class ToolCursors {
public:
    ~ToolCursors() noexcept { Destroy(); }

    // Draws all eight. Does nothing if they are already there, so it is safe to
    // call from wherever the first one is wanted.
    void Build() noexcept;

    // `removing` is Alt being held, which takes pieces back out of what is
    // picked; it is drawn in the same red the outline of a subtraction uses.
    // Returns nullptr if building failed, which leaves the caller to fall back
    // on a system cursor rather than showing nothing at all.
    HCURSOR Get(SelectCursor which, bool removing) const noexcept;

    void Destroy() noexcept;

private:
    static constexpr size_t kCount = static_cast<size_t>(SelectCursor::Count) * 2;

    HCURSOR cursors_[kCount]{};
    bool built_ = false;
};

}  // namespace ccl::ui

#pragma once

namespace ccl::tool {

// Which tool the left button belongs to.
//
// On its own, with nothing included, because the undo history holds one of
// these per step -- so that stepping back over an edit puts back the tool it
// was made with, and the marks it left can be seen for what they are. Kept out
// of ToolState.h for that: the history has no business with brush sizes and
// colours.
enum class Tool {
    View,
    Pen,
    Eraser,
    Eyedropper,
    Text,
    Select,
    // Selects by hand rather than by corner. A tool of its own rather than a
    // mode of the one above, so that which of the two is in use is visible
    // without looking for it -- the two share the area they select.
    Lasso,
};

}  // namespace ccl::tool

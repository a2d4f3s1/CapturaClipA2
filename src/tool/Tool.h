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
};

}  // namespace ccl::tool

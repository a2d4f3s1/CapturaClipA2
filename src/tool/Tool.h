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
    // The two above pick out an area of the picture. These two pick out the
    // things that were drawn on it -- a line, a patch of paint, a piece of
    // text -- so that they can be moved, turned, or sent behind one another
    // after the fact.
    //
    // Tools of their own for the same reason the lasso is one: what the left
    // button is about to do differs, and which of the two kinds of selecting
    // is in use should be visible without having to try it.
    ObjectSelect,
    ObjectLasso,
};

}  // namespace ccl::tool

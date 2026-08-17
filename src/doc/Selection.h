#pragma once

#include <vector>

namespace ccl::doc {

// How a piece combines with what is already selected.
enum class SelectionOp {
    Replace,
    Add,
    Subtract,
};

struct SelectionPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// One piece of a selection, in image coordinates at 100% zoom.
//
// A rectangle keeps its corners instead of being folded into a four-point
// polygon. Whether a selection is a single rectangle decides what can be done
// with it -- cropping produces a rectangle and nothing else -- and asking that
// of a polygon means comparing floating point corners.
//
// Deliberately no Direct2D types here: the rest of doc/ is plain floats too,
// and keeping it that way is what lets the eraser and the hit tests answer
// without a render target in hand.
struct SelectionShape {
    SelectionOp op = SelectionOp::Replace;
    bool lasso = false;

    // When lasso is false.
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    // When lasso is true: a closed polygon, the closing edge implied.
    std::vector<SelectionPoint> points;

    bool ContainsPoint(float x, float y) const noexcept;
};

using SelectionShapes = std::vector<SelectionShape>;

// True when the point falls inside, with the pieces folded in the order they
// were laid down -- the same combining the geometry does.
//
// Kept on the CPU because the eraser asks this for every movement while it is
// being dragged, and building a geometry each time to answer would be work out
// of all proportion to the question.
bool SelectionContains(const SelectionShapes& shapes, float x,
                       float y) noexcept;

// True when the point is within `distance` of an edge of any of the pieces,
// which is how something drawn along the edge is picked out.
//
// The pieces are taken one by one rather than folded first: an edge that the
// folding removed -- a piece taken away from somewhere nothing was selected --
// would count here although nothing was drawn along it. That costs a stray hit
// in a corner case, against having to fold a shape to answer at all.
bool SelectionNearEdge(const SelectionShapes& shapes, float x, float y,
                       float distance) noexcept;

// True when the selection is exactly one rectangle, which is what the
// operations that can only produce a rectangle are offered for.
bool IsSingleRect(const SelectionShapes& shapes) noexcept;

}  // namespace ccl::doc

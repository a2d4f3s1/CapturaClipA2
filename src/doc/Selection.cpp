#include "doc/Selection.h"

namespace ccl::doc {

bool SelectionShape::ContainsPoint(float x, float y) const noexcept {
    if (!lasso) {
        return x >= left && x < right && y >= top && y < bottom;
    }

    // Crossing count. A ray is cast to the right and the edges it passes
    // through are counted; an odd number means the point is inside. The
    // closing edge is included by pairing the last point with the first.
    if (points.size() < 3) {
        return false;
    }

    bool inside = false;
    size_t previous = points.size() - 1;
    for (size_t i = 0; i < points.size(); ++i) {
        const SelectionPoint& a = points[previous];
        const SelectionPoint& b = points[i];
        previous = i;

        // Only edges that straddle the ray's height can be crossed. The
        // comparison is deliberately asymmetric so that a point exactly level
        // with a shared corner is counted once rather than twice.
        if ((b.y > y) == (a.y > y)) {
            continue;
        }
        const float crossing = (a.x - b.x) * (y - b.y) / (a.y - b.y) + b.x;
        if (x < crossing) {
            inside = !inside;
        }
    }
    return inside;
}

bool SelectionContains(const SelectionShapes& shapes, float x,
                       float y) noexcept {
    bool inside = false;
    for (const SelectionShape& shape : shapes) {
        const bool here = shape.ContainsPoint(x, y);
        switch (shape.op) {
            case SelectionOp::Replace:
                inside = here;
                break;
            case SelectionOp::Add:
                inside = inside || here;
                break;
            case SelectionOp::Subtract:
                inside = inside && !here;
                break;
        }
    }
    return inside;
}

bool IsSingleRect(const SelectionShapes& shapes) noexcept {
    return shapes.size() == 1 && !shapes.front().lasso;
}

}  // namespace ccl::doc

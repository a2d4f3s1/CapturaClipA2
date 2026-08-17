#include "doc/Selection.h"

#include <algorithm>

namespace ccl::doc {
namespace {

float DistanceToSegmentSquared(float px, float py, float ax, float ay, float bx,
                               float by) noexcept {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;

    float t = 0.0f;
    if (lengthSquared > 0.0f) {
        t = ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
        t = std::clamp(t, 0.0f, 1.0f);
    }

    const float nx = px - (ax + t * dx);
    const float ny = py - (ay + t * dy);
    return nx * nx + ny * ny;
}

}  // namespace

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

bool SelectionNearEdge(const SelectionShapes& shapes, float x, float y,
                       float distance) noexcept {
    const float limit = distance * distance;

    for (const SelectionShape& shape : shapes) {
        if (!shape.lasso) {
            const float left = (std::min)(shape.left, shape.right);
            const float right = (std::max)(shape.left, shape.right);
            const float top = (std::min)(shape.top, shape.bottom);
            const float bottom = (std::max)(shape.top, shape.bottom);

            const SelectionPoint corners[4] = {
                {left, top}, {right, top}, {right, bottom}, {left, bottom}};
            for (int i = 0; i < 4; ++i) {
                const SelectionPoint& a = corners[i];
                const SelectionPoint& b = corners[(i + 1) % 4];
                if (DistanceToSegmentSquared(x, y, a.x, a.y, b.x, b.y) <=
                    limit) {
                    return true;
                }
            }
            continue;
        }

        if (shape.points.size() < 2) {
            continue;
        }
        size_t previous = shape.points.size() - 1;
        for (size_t i = 0; i < shape.points.size(); ++i) {
            const SelectionPoint& a = shape.points[previous];
            const SelectionPoint& b = shape.points[i];
            previous = i;
            if (DistanceToSegmentSquared(x, y, a.x, a.y, b.x, b.y) <= limit) {
                return true;
            }
        }
    }
    return false;
}

bool IsSingleRect(const SelectionShapes& shapes) noexcept {
    return shapes.size() == 1 && !shapes.front().lasso;
}

}  // namespace ccl::doc

#pragma once

#include <vector>

namespace ccl::doc {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct StrokePoint {
    float x = 0.0f;
    float y = 0.0f;
    // The width the line actually has here, in image pixels.
    //
    // Stored resolved rather than as a pressure to be scaled later, because the
    // width comes from different places depending on how the line was drawn --
    // pen pressure while drawing freehand, the brush size at each end of a
    // straight line -- and the renderer should not have to care which.
    float width = 3.0f;
};

// A single drawn line, in image coordinates at 100% zoom.
struct Stroke {
    std::vector<StrokePoint> points;
    Color color;
    bool antialias = true;

    // True when the width changes along the stroke, which means it has to be
    // drawn segment by segment rather than as a single path.
    bool HasVariableWidth() const noexcept {
        if (points.size() < 2) {
            return false;
        }
        const float first = points.front().width;
        for (const StrokePoint& point : points) {
            if (point.width != first) {
                return true;
            }
        }
        return false;
    }
};

// Annotations are kept as objects instead of being burned into the image.
//
// This is the structural difference the whole tool hinges on: erasing a
// stroke, re-editing a piece of text, and changing colour or width after the
// fact are all impossible once drawing has been flattened into pixels.
//
// Geometry-changing operations (crop, rotate, concatenate) will flatten the
// annotations first, because their meaning is undefined otherwise.
enum class AnnotationKind {
    Stroke,
    Text,
};

struct Annotation {
    AnnotationKind kind = AnnotationKind::Stroke;
    Stroke stroke;
};

using AnnotationList = std::vector<Annotation>;

}  // namespace ccl::doc

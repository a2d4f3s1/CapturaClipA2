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
    // 0..1. Always 1 for mouse input; kept per-point so that pen pressure can
    // be added later without rebuilding the stroke representation or the
    // rendering that depends on it.
    float pressure = 1.0f;
};

// A single drawn line, in image coordinates at 100% zoom.
struct Stroke {
    std::vector<StrokePoint> points;
    Color color;
    float width = 3.0f;
    bool antialias = true;
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

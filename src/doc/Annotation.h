#pragma once

#include <vector>

namespace ccl::doc {

// Annotations are kept as objects instead of being burned into the image.
//
// This is the structural difference that the whole tool hinges on: erasing a
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
};

using AnnotationList = std::vector<Annotation>;

}  // namespace ccl::doc

#pragma once

#include <string>
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

// Styling applied to a stretch of characters, so that part of a line can be
// coloured or emphasised without splitting it into separate annotations.
struct TextRun {
    unsigned int start = 0;
    unsigned int length = 0;
    Color color;
    float fontSize = 0.0f;       // 0 means the annotation's own size
    std::wstring fontFamily;     // empty means the annotation's own font
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;

    bool SameStyle(const TextRun& other) const noexcept {
        return color.r == other.color.r && color.g == other.color.g &&
               color.b == other.color.b && fontSize == other.fontSize &&
               fontFamily == other.fontFamily && bold == other.bold &&
               italic == other.italic && underline == other.underline &&
               strikethrough == other.strikethrough;
    }
};

// A piece of text placed on the image, kept as text rather than as pixels so
// that the wording, size and colour stay editable after the fact.
struct TextAnnotation {
    std::wstring text;
    // Top-left corner, in image coordinates at 100% zoom.
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 30.0f;  // image pixels
    std::wstring fontFamily = L"Meiryo";
    Color color;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;

    // Per-character styling. When empty the fields above apply to the whole
    // string; when present they override it for the ranges they cover.
    std::vector<TextRun> runs;

    // Width the text wraps at, in image pixels. Zero means no wrapping. Stored
    // so the drawn result breaks in the same places it did while being typed.
    float wrapWidth = 0.0f;

    // A drop shadow or an outline keeps text legible over a busy screenshot,
    // which is most of what gets captured.
    bool shadow = true;
    bool outline = false;
    Color outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
};

enum class EffectKind {
    Mosaic,
    Blur,
};

// An area of the image obscured to hide what it shows.
//
// Kept as an annotation rather than painted into the image so that it can be
// undone and moved like anything else, and so the original pixels survive
// until the capture is saved.
struct EffectAnnotation {
    EffectKind kind = EffectKind::Mosaic;
    // Image coordinates at 100% zoom.
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    // Mosaic block size, or blur radius, in image pixels.
    float strength = 12.0f;
    // Identifies the processed pixels in the renderer's cache. Reprocessing
    // the area on every frame would be wasteful; the result never changes.
    unsigned int id = 0;
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
    Effect,
};

struct Annotation {
    AnnotationKind kind = AnnotationKind::Stroke;
    Stroke stroke;
    TextAnnotation text;
    EffectAnnotation effect;
};

using AnnotationList = std::vector<Annotation>;

}  // namespace ccl::doc

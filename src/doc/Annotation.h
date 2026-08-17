#pragma once

#include <array>
#include <string>
#include <vector>

#include "doc/Selection.h"

namespace ccl::doc {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// The colours reachable with Shift+1..8, which are also the palette's fixed top
// row. Editable in the settings, so this is only where they start.
using QuickColors = std::array<Color, 8>;

inline constexpr QuickColors kDefaultQuickColors = {{
    {1.00f, 0.20f, 0.20f, 1.0f},  // red
    {0.20f, 0.85f, 0.30f, 1.0f},  // green
    {0.25f, 0.55f, 1.00f, 1.0f},  // blue
    {1.00f, 0.85f, 0.15f, 1.0f},  // yellow
    {1.00f, 0.40f, 0.85f, 1.0f},  // magenta
    {0.20f, 0.85f, 0.90f, 1.0f},  // cyan
    {1.00f, 1.00f, 1.00f, 1.0f},  // white
    {0.05f, 0.05f, 0.05f, 1.0f},  // black
}};

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

// A highlighter is only a highlighter if what is under it still reads, so its
// strokes go down at a fixed transparency rather than a chosen one.
inline constexpr float kHighlighterOpacity = 0.4f;

// A single drawn line, in image coordinates at 100% zoom.
struct Stroke {
    std::vector<StrokePoint> points;
    Color color;
    bool antialias = true;
    // Laid down as a translucent wash rather than an opaque line, and as one
    // piece: a highlighter that darkened where the stroke crossed itself would
    // not look like a highlighter.
    bool highlighter = false;

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
};

// An area of the image painted in, or drawn round, in a flat colour.
//
// The two are one thing rather than two: same shape, same colour, same rules
// about when it goes away. Only the last step differs -- whether the shape is
// filled or its edge is traced.
//
// Kept apart from the obscuring effects rather than made another kind of one.
// An effect exists to hold on to the pixels it covers, taken as they were the
// moment it was placed; paint has nothing underneath it to hold on to, and
// folding it in would mean writing "except for these" through every part of
// that machinery.
struct AreaAnnotation {
    // The shape as it was when the paint went down. It stays put afterwards,
    // whatever happens to the selection it came from.
    SelectionShapes shape;
    Color color;
    // Resolved when the paint goes down rather than looked up later, in the
    // same way a stroke resolves pressure into a width: what draws it should
    // not have to know which menu entry it came from.
    float opacity = 1.0f;
    bool antialias = true;
    // Zero fills the shape. Anything else traces its edge at that width, in
    // image pixels.
    float width = 0.0f;
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
    Area,
};

struct Annotation {
    // Identifies this annotation for as long as it exists, including across
    // undo and redo, since those restore copies of the list.
    //
    // The renderer keeps worked-out results against it -- the path a stroke
    // traces, the laid-out glyphs of a piece of text, the obscured pixels of
    // an effect -- none of which change once the annotation is placed, and all
    // of which cost real time to produce again. Keyed on a position in the
    // list instead, erasing one annotation would hand the next one's cached
    // result to its neighbour.
    unsigned int id = 0;
    AnnotationKind kind = AnnotationKind::Stroke;
    Stroke stroke;
    TextAnnotation text;
    EffectAnnotation effect;
    AreaAnnotation area;
};

// Handed out in order and never reused, so a cached result can never be taken
// for one belonging to a different annotation.
unsigned int NextAnnotationId() noexcept;

using AnnotationList = std::vector<Annotation>;

}  // namespace ccl::doc

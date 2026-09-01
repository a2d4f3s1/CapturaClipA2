#include "io/ImageOps.h"

#include <algorithm>
#include <cstring>

namespace ccl::io {
namespace {

using ccl::capture::DibBuffer;

const unsigned char* RowOf(const DibBuffer& image, int y) noexcept {
    return static_cast<const unsigned char*>(image.Pixels()) +
           static_cast<size_t>(y) * image.Stride();
}

unsigned char* RowOf(DibBuffer& image, int y) noexcept {
    return static_cast<unsigned char*>(image.Pixels()) +
           static_cast<size_t>(y) * image.Stride();
}

// One BGRA pixel packed the way the buffers store it. Written as a word so the
// inner loops of the rotations move four bytes at a time.
unsigned int Pack(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) -> unsigned int {
        const float scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
        return static_cast<unsigned int>(scaled + 0.5f);
    };
    return (channel(color.a) << 24) | (channel(color.r) << 16) |
           (channel(color.g) << 8) | channel(color.b);
}

void Fill(DibBuffer& image, unsigned int value) noexcept {
    for (int y = 0; y < image.Height(); ++y) {
        auto* row = reinterpret_cast<unsigned int*>(RowOf(image, y));
        std::fill_n(row, image.Width(), value);
    }
}

// Copies `source` into `target` with its top-left corner at (x, y). The caller
// guarantees it fits.
void Blit(DibBuffer& target, const DibBuffer& source, int x, int y) noexcept {
    for (int row = 0; row < source.Height(); ++row) {
        std::memcpy(RowOf(target, y + row) + static_cast<size_t>(x) * 4u,
                    RowOf(source, row),
                    static_cast<size_t>(source.Width()) * 4u);
    }
}

}  // namespace

DibBuffer RotateLeft(const DibBuffer& source) noexcept {
    DibBuffer result;
    if (!source.IsValid() ||
        !result.Create(source.Height(), source.Width())) {
        return result;
    }

    const int width = source.Width();
    const int height = source.Height();

    // Turning the picture anticlockwise sends its rightmost column to the top,
    // so target row r is source column (width - 1 - r) read downwards.
    for (int row = 0; row < width; ++row) {
        auto* target = reinterpret_cast<unsigned int*>(RowOf(result, row));
        const int column = width - 1 - row;
        for (int y = 0; y < height; ++y) {
            target[y] =
                reinterpret_cast<const unsigned int*>(RowOf(source, y))[column];
        }
    }
    return result;
}

DibBuffer RotateRight(const DibBuffer& source) noexcept {
    DibBuffer result;
    if (!source.IsValid() ||
        !result.Create(source.Height(), source.Width())) {
        return result;
    }

    const int width = source.Width();
    const int height = source.Height();

    // Clockwise: the leftmost column becomes the top row, read upwards.
    for (int row = 0; row < width; ++row) {
        auto* target = reinterpret_cast<unsigned int*>(RowOf(result, row));
        for (int y = 0; y < height; ++y) {
            target[y] = reinterpret_cast<const unsigned int*>(
                RowOf(source, height - 1 - y))[row];
        }
    }
    return result;
}

DibBuffer Rotate180(const DibBuffer& source) noexcept {
    DibBuffer result;
    if (!source.IsValid() ||
        !result.Create(source.Width(), source.Height())) {
        return result;
    }

    const int width = source.Width();
    const int height = source.Height();

    for (int y = 0; y < height; ++y) {
        auto* target = reinterpret_cast<unsigned int*>(RowOf(result, y));
        const auto* row = reinterpret_cast<const unsigned int*>(
            RowOf(source, height - 1 - y));
        for (int x = 0; x < width; ++x) {
            target[x] = row[width - 1 - x];
        }
    }
    return result;
}

DibBuffer FlipHorizontal(const DibBuffer& source) noexcept {
    DibBuffer result;
    if (!source.IsValid() ||
        !result.Create(source.Width(), source.Height())) {
        return result;
    }

    const int width = source.Width();
    for (int y = 0; y < source.Height(); ++y) {
        auto* target = reinterpret_cast<unsigned int*>(RowOf(result, y));
        const auto* row = reinterpret_cast<const unsigned int*>(RowOf(source, y));
        for (int x = 0; x < width; ++x) {
            target[x] = row[width - 1 - x];
        }
    }
    return result;
}

DibBuffer FlipVertical(const DibBuffer& source) noexcept {
    DibBuffer result;
    if (!source.IsValid() ||
        !result.Create(source.Width(), source.Height())) {
        return result;
    }

    const int height = source.Height();
    for (int y = 0; y < height; ++y) {
        std::memcpy(RowOf(result, y), RowOf(source, height - 1 - y),
                    static_cast<size_t>(source.Width()) * 4u);
    }
    return result;
}

ConcatLayout PlanConcat(int sourceWidth, int sourceHeight, int additionWidth,
                        int additionHeight,
                        const ConcatPlacement& placement) noexcept {
    // Where the added picture would sit if the one already open had its corner
    // at the origin. The side decides which way it goes and the gap how far;
    // the nudge is free to take it anywhere from there, including back over
    // the top of what it was placed beside.
    int x = 0;
    int y = 0;
    switch (placement.side) {
        case ConcatSide::Right: x = sourceWidth + placement.margin; break;
        case ConcatSide::Left: x = -(additionWidth + placement.margin); break;
        case ConcatSide::Bottom: y = sourceHeight + placement.margin; break;
        case ConcatSide::Top: y = -(additionHeight + placement.margin); break;
    }
    x += placement.offsetX;
    y += placement.offsetY;

    // The box that covers both. Taken rather than assumed, since the added
    // picture may now reach above or to the left of where the other starts.
    const int left = std::min(0, x);
    const int top = std::min(0, y);
    const int right = std::max(sourceWidth, x + additionWidth);
    const int bottom = std::max(sourceHeight, y + additionHeight);

    ConcatLayout layout;
    layout.width = right - left;
    layout.height = bottom - top;
    // Shifted so that nothing sits at a negative coordinate: the result has to
    // start somewhere, and that somewhere is whichever picture reaches furthest
    // back.
    layout.sourceX = -left;
    layout.sourceY = -top;
    layout.additionX = x - left;
    layout.additionY = y - top;
    return layout;
}

DibBuffer Concatenate(const DibBuffer& source, const DibBuffer& addition,
                      const ConcatPlacement& placement,
                      const ccl::doc::Color& fill) noexcept {
    DibBuffer result;
    if (!source.IsValid() || !addition.IsValid()) {
        return result;
    }

    const ConcatLayout layout =
        PlanConcat(source.Width(), source.Height(), addition.Width(),
                   addition.Height(), placement);
    if (layout.width <= 0 || layout.height <= 0 ||
        !result.Create(layout.width, layout.height)) {
        return result;
    }

    // Only the space neither picture reaches is ever seen, but filling
    // everything is simpler than working out which part that is, and costs one
    // pass.
    Fill(result, Pack(fill));

    // The one that is meant to be underneath goes down first. Nothing is
    // blended: where they overlap, the second one covers the first outright,
    // which is what "on top" is being asked for.
    if (placement.additionOnTop) {
        Blit(result, source, layout.sourceX, layout.sourceY);
        Blit(result, addition, layout.additionX, layout.additionY);
    } else {
        Blit(result, addition, layout.additionX, layout.additionY);
        Blit(result, source, layout.sourceX, layout.sourceY);
    }
    return result;
}

}  // namespace ccl::io

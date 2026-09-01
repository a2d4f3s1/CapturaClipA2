#pragma once

#include "capture/DibBuffer.h"
#include "doc/Annotation.h"

namespace ccl::io {

// Reshaping operations on a picture. All of them build a new buffer and leave
// the source alone; an invalid result means it could not be allocated.
//
// These work on pixels only. Annotations are vector data placed against the old
// shape, so the caller flattens them in before transforming.

// Quarter turns, named for the direction the picture appears to move.
ccl::capture::DibBuffer RotateLeft(const ccl::capture::DibBuffer& source) noexcept;
ccl::capture::DibBuffer RotateRight(const ccl::capture::DibBuffer& source) noexcept;
ccl::capture::DibBuffer Rotate180(const ccl::capture::DibBuffer& source) noexcept;

ccl::capture::DibBuffer FlipHorizontal(
    const ccl::capture::DibBuffer& source) noexcept;
ccl::capture::DibBuffer FlipVertical(
    const ccl::capture::DibBuffer& source) noexcept;

enum class ConcatSide { Top, Bottom, Left, Right };

// How the added picture is placed against the one already open.
struct ConcatPlacement {
    ConcatSide side = ConcatSide::Right;
    // Gap left between the two along the joining direction.
    int margin = 0;
    // Nudge from where the side and the gap put it. Either may be negative,
    // and the two are free to end up overlapping -- which is what makes this a
    // placement rather than a join.
    int offsetX = 0;
    int offsetY = 0;
    // Which one is drawn second where they overlap. No blending either way:
    // whichever goes second covers the other outright.
    bool additionOnTop = true;
};

// Places `addition` against `source` and returns the two together.
//
// The result is the box that covers both, so nudging the added picture past an
// edge grows the picture rather than cutting it off. Wherever neither reaches,
// `fill` shows through.
//
// With no gap and no nudge this is the plain join it has always been: the two
// meet along the given side, aligned at the corner they share.
ccl::capture::DibBuffer Concatenate(const ccl::capture::DibBuffer& source,
                                    const ccl::capture::DibBuffer& addition,
                                    const ConcatPlacement& placement,
                                    const ccl::doc::Color& fill) noexcept;

// Where the added picture lands and how big the result comes out, without
// doing the work. Lets a caller show the outcome, or refuse it for being
// unreasonably large, before committing to building it.
struct ConcatLayout {
    int width = 0;
    int height = 0;
    // Top-left corners within the result.
    int sourceX = 0;
    int sourceY = 0;
    int additionX = 0;
    int additionY = 0;
};

ConcatLayout PlanConcat(int sourceWidth, int sourceHeight, int additionWidth,
                        int additionHeight,
                        const ConcatPlacement& placement) noexcept;

}  // namespace ccl::io

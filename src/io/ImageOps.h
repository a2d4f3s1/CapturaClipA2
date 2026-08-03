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

enum class ConcatSide { Right, Bottom };

// Places `addition` alongside `source`. When the two differ across the joining
// edge the result is as large as the wider (or taller) of them, and the space
// left over is filled with `fill`.
ccl::capture::DibBuffer Concatenate(const ccl::capture::DibBuffer& source,
                                    const ccl::capture::DibBuffer& addition,
                                    ConcatSide side,
                                    const ccl::doc::Color& fill) noexcept;

}  // namespace ccl::io

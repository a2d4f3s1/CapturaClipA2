#pragma once

#include <windows.h>

namespace ccl::capture {

// A 32-bit top-down BGRA DIB section.
//
// Owning the GDI object directly means the screen grab lands in memory that
// Direct2D can consume as-is, with no intermediate copy on the launch path.
class DibBuffer {
public:
    DibBuffer() = default;
    ~DibBuffer();

    DibBuffer(DibBuffer&& other) noexcept;
    DibBuffer& operator=(DibBuffer&& other) noexcept;

    DibBuffer(const DibBuffer&) = delete;
    DibBuffer& operator=(const DibBuffer&) = delete;

    bool Create(int width, int height) noexcept;
    void Reset() noexcept;

    bool IsValid() const noexcept { return bitmap_ != nullptr; }
    int Width() const noexcept { return width_; }
    int Height() const noexcept { return height_; }
    UINT Stride() const noexcept { return static_cast<UINT>(width_) * 4u; }
    HBITMAP Handle() const noexcept { return bitmap_; }
    const void* Pixels() const noexcept { return pixels_; }
    void* Pixels() noexcept { return pixels_; }

    // Copies a sub-rectangle into a new buffer. The rectangle is clamped to the
    // bounds of this buffer; an empty result means the intersection was empty.
    DibBuffer Crop(const RECT& area) const noexcept;

private:
    HBITMAP bitmap_ = nullptr;
    void* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace ccl::capture

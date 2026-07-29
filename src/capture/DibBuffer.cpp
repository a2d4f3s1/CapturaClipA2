#include "capture/DibBuffer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ccl::capture {

DibBuffer::~DibBuffer() { Reset(); }

DibBuffer::DibBuffer(DibBuffer&& other) noexcept
    : bitmap_(std::exchange(other.bitmap_, nullptr)),
      pixels_(std::exchange(other.pixels_, nullptr)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)) {}

DibBuffer& DibBuffer::operator=(DibBuffer&& other) noexcept {
    if (this != &other) {
        Reset();
        bitmap_ = std::exchange(other.bitmap_, nullptr);
        pixels_ = std::exchange(other.pixels_, nullptr);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

void DibBuffer::Reset() noexcept {
    if (bitmap_ != nullptr) {
        ::DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    pixels_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool DibBuffer::Create(int width, int height) noexcept {
    Reset();

    if (width <= 0 || height <= 0) {
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // negative = top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                              &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        return false;
    }

    bitmap_ = bitmap;
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    return true;
}

DibBuffer DibBuffer::Crop(const RECT& area) const noexcept {
    DibBuffer result;
    if (!IsValid()) {
        return result;
    }

    const int left = std::clamp(static_cast<int>(area.left), 0, width_);
    const int top = std::clamp(static_cast<int>(area.top), 0, height_);
    const int right = std::clamp(static_cast<int>(area.right), left, width_);
    const int bottom = std::clamp(static_cast<int>(area.bottom), top, height_);

    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0 || !result.Create(width, height)) {
        return result;
    }

    const auto* source = static_cast<const unsigned char*>(pixels_) +
                         static_cast<size_t>(top) * Stride() +
                         static_cast<size_t>(left) * 4u;
    auto* destination = static_cast<unsigned char*>(result.pixels_);

    for (int y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * result.Stride(),
                    source + static_cast<size_t>(y) * Stride(),
                    static_cast<size_t>(width) * 4u);
    }
    return result;
}

}  // namespace ccl::capture

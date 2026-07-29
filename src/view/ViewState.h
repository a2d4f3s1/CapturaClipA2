#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace ccl::view {

// How the document is presented: zoom, scroll offset and opacity.
class ViewState {
public:
    static constexpr float kMinZoom = 0.1f;
    static constexpr float kMaxZoom = 5.0f;

    // Percentage change per wheel notch. Multiplicative rather than additive:
    // adding a fixed percentage collapses to zero when zooming out, while
    // multiplying gives the same felt step in both directions and never
    // reaches zero. Becomes a setting in a later phase.
    static constexpr float kZoomStepPercent = 10.0f;

    // Fraction of a step used when the modifier for fine adjustment is held.
    static constexpr float kFineStepFactor = 0.2f;

    static constexpr BYTE kMinOpacity = 40;
    static constexpr BYTE kMaxOpacity = 255;
    static constexpr int kOpacityStep = 16;

    float Zoom() const noexcept { return zoom_; }
    POINT Scroll() const noexcept { return scroll_; }
    BYTE Opacity() const noexcept { return opacity_; }

    void SetZoom(float zoom) noexcept {
        zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    }

    void StepZoom(int notches, bool fine) noexcept {
        if (notches == 0) {
            return;
        }
        const float exponent =
            static_cast<float>(notches) * (fine ? kFineStepFactor : 1.0f);
        const float factor = std::pow(1.0f + kZoomStepPercent / 100.0f, exponent);
        SetZoom(zoom_ * factor);
    }

    void StepOpacity(int notches) noexcept {
        const int value = static_cast<int>(opacity_) + notches * kOpacityStep;
        opacity_ = static_cast<BYTE>(
            std::clamp(value, static_cast<int>(kMinOpacity),
                       static_cast<int>(kMaxOpacity)));
    }

    // Scrolling only has an effect while the window is smaller than the zoomed
    // image; the offset is clamped so the image can never be dragged off.
    void SetScroll(POINT offset, SIZE content, SIZE viewport) noexcept {
        const LONG maxX = std::max(0L, content.cx - viewport.cx);
        const LONG maxY = std::max(0L, content.cy - viewport.cy);
        scroll_.x = std::clamp(offset.x, 0L, maxX);
        scroll_.y = std::clamp(offset.y, 0L, maxY);
    }

    void ScrollBy(int dx, int dy, SIZE content, SIZE viewport) noexcept {
        SetScroll(POINT{scroll_.x + dx, scroll_.y + dy}, content, viewport);
    }

    // Size the zoomed image occupies, in pixels.
    SIZE ContentSize(int imageWidth, int imageHeight) const noexcept {
        return SIZE{std::max(1L, static_cast<LONG>(std::lround(imageWidth * zoom_))),
                    std::max(1L, static_cast<LONG>(std::lround(imageHeight * zoom_)))};
    }

private:
    float zoom_ = 1.0f;
    POINT scroll_{0, 0};
    BYTE opacity_ = kMaxOpacity;
};

}  // namespace ccl::view

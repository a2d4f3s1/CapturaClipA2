#pragma once

#include <algorithm>
#include <array>

#include "doc/Annotation.h"

namespace ccl::tool {

enum class Tool {
    View,
    Pen,
    Eraser,
};

// Quick colours, reachable with Shift+1..8 until the colour picker exists.
inline constexpr std::array<ccl::doc::Color, 8> kQuickColors = {{
    {1.00f, 0.20f, 0.20f, 1.0f},  // red
    {0.20f, 0.85f, 0.30f, 1.0f},  // green
    {0.25f, 0.55f, 1.00f, 1.0f},  // blue
    {1.00f, 0.85f, 0.15f, 1.0f},  // yellow
    {1.00f, 0.40f, 0.85f, 1.0f},  // magenta
    {0.20f, 0.85f, 0.90f, 1.0f},  // cyan
    {1.00f, 1.00f, 1.00f, 1.0f},  // white
    {0.05f, 0.05f, 0.05f, 1.0f},  // black
}};

class ToolState {
public:
    static constexpr float kMinWidth = 1.0f;
    static constexpr float kMaxWidth = 200.0f;

    Tool tool = Tool::View;
    ccl::doc::Color color = kQuickColors[1];  // green
    bool antialias = true;

    float Width() const noexcept { return width_; }

    void SetWidth(float width) noexcept {
        width_ = std::clamp(width, kMinWidth, kMaxWidth);
    }

    // Multiplicative so that the step feels the same at 2px and at 60px.
    void StepWidth(int steps) noexcept {
        for (int i = 0; i < steps; ++i) {
            SetWidth(width_ * 1.25f + 0.5f);
        }
        for (int i = 0; i > steps; --i) {
            SetWidth((width_ - 0.5f) / 1.25f);
        }
    }

private:
    float width_ = 4.0f;
};

}  // namespace ccl::tool

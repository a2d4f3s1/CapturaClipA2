#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include "doc/Annotation.h"

namespace ccl::tool {

enum class Tool {
    View,
    Pen,
    Eraser,
    Eyedropper,
};

// Quick colours, reachable with Shift+1..8.
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

inline constexpr size_t kMaxRecentColors = 8;

inline bool SameColor(const ccl::doc::Color& a, const ccl::doc::Color& b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

class ToolState {
public:
    static constexpr float kMinWidth = 1.0f;
    static constexpr float kMaxWidth = 200.0f;

    Tool tool = Tool::View;
    bool antialias = true;
    // Runtime state, seeded from the settings file but toggleable from the
    // menu; editing the file to try it out is too much friction.
    bool usePressure = true;

    const ccl::doc::Color& Color() const noexcept { return color_; }

    // Sets the colour without touching the recent list. Used while a colour is
    // still being chosen, so that dragging through a gradient does not fill the
    // history with every shade passed over.
    void SetColor(const ccl::doc::Color& color) noexcept { color_ = color; }

    // Commits a colour. Every deliberate choice goes through here so the recent
    // list stays accurate however it was made -- quick key, palette or
    // eyedropper.
    void UseColor(const ccl::doc::Color& color) noexcept {
        color_ = color;

        const auto existing = std::find_if(
            recent_.begin(), recent_.end(),
            [&](const ccl::doc::Color& entry) { return SameColor(entry, color); });
        if (existing != recent_.end()) {
            recent_.erase(existing);
        }
        recent_.insert(recent_.begin(), color);
        if (recent_.size() > kMaxRecentColors) {
            recent_.resize(kMaxRecentColors);
        }
    }

    const std::vector<ccl::doc::Color>& RecentColors() const noexcept {
        return recent_;
    }

    float Width() const noexcept { return width_; }

    void SetWidth(float width) noexcept {
        width_ = std::clamp(width, kMinWidth, kMaxWidth);
    }

    // Multiplicative so that a step feels the same at 2px and at 60px.
    //
    // Exposed as a free-standing calculation because the ends of a tapered line
    // each step from their own width. Routing those through the brush size
    // would make adjusting one end drag the other to match.
    static float SteppedWidth(float width, int steps) noexcept {
        for (int i = 0; i < steps; ++i) {
            width = std::clamp(width * 1.25f + 0.5f, kMinWidth, kMaxWidth);
        }
        for (int i = 0; i > steps; --i) {
            width = std::clamp((width - 0.5f) / 1.25f, kMinWidth, kMaxWidth);
        }
        return width;
    }

    void StepWidth(int steps) noexcept { SetWidth(SteppedWidth(width_, steps)); }

private:
    ccl::doc::Color color_ = kQuickColors[1];  // green
    std::vector<ccl::doc::Color> recent_;
    float width_ = 4.0f;
};

}  // namespace ccl::tool

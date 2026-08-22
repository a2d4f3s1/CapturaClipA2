#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "doc/Annotation.h"
#include "tool/Tool.h"

namespace ccl::tool {

using ccl::doc::kDefaultQuickColors;
using QuickColors = ccl::doc::QuickColors;

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
    // A mode of the pen rather than a tool of its own: the same size and colour,
    // laid down as a translucent wash instead of an opaque line.
    bool highlighter = false;

    // Text styling. Held here rather than only on each annotation so that the
    // same switches serve both new text and text being re-edited: opening an
    // existing piece loads its styling in, and committing writes back whatever
    // is set now.
    bool textBold = false;
    bool textItalic = false;
    bool textUnderline = false;
    bool textStrikethrough = false;
    // What the next piece of text is given. Seeded from the settings file like
    // the rest of these and changed from the menu during a session -- which is
    // why it lives here rather than in the settings: written there, a size
    // meant for one piece of text became the saved default the next time the
    // settings dialog was closed with OK.
    float textFontSize = 30.0f;
    std::wstring textFontFamily = L"Meiryo";
    // Backing that keeps text readable over a screenshot. Seeded from the
    // settings file and toggleable per piece of text.
    bool textShadow = false;
    bool textOutline = true;
    // Whole pixels at 100%, and nothing to do with how large the text is. The
    // shadow is thrown one of nine ways: 0 to 7 clockwise from straight up, and
    // 8 straight underneath, where only its spread shows.
    float textOutlineWidth = 2.0f;
    float textShadowLength = 2.0f;
    int textShadowDirection = 3;  // down-right
    // Carries how strong the shadow is in its alpha, unlike the settings file,
    // which has to keep the two apart.
    ccl::doc::Color textShadowColor{0.0f, 0.0f, 0.0f, 1.0f};
    ccl::doc::Color textOutlineColor{0.0f, 0.0f, 0.0f, 1.0f};

    // Strength last used for each obscuring effect, so the next one starts
    // where the last was left. Negative means "not chosen yet", in which case
    // the strength is derived from the size of the area. Zero is a real value:
    // it leaves the area untouched.
    float mosaicStrength = -1.0f;
    float blurStrength = -1.0f;
    // Runtime state, seeded from the settings file but toggleable from the
    // menu; editing the file to try it out is too much friction.
    bool usePressure = true;

    const ccl::doc::Color& Color() const noexcept { return penColor_; }

    // Sets the colour without touching the recent list. Used while a colour is
    // still being chosen, so that dragging through a gradient does not fill the
    // history with every shade passed over.
    void SetColor(const ccl::doc::Color& color) noexcept { penColor_ = color; }

    // Commits a colour. Every deliberate choice goes through here so the recent
    // list stays accurate however it was made -- quick key, palette or
    // eyedropper.
    void UseColor(const ccl::doc::Color& color) noexcept {
        SetColor(color);

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

    float Width() const noexcept { return WidthSlot(); }

    void SetWidth(float width) noexcept {
        WidthSlot() = std::clamp(width, kMinWidth, kMaxWidth);
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

    void StepWidth(int steps) noexcept {
        SetWidth(SteppedWidth(WidthSlot(), steps));
    }

    // Defaults from the settings file, applied before anything has been
    // adjusted by hand.
    void SeedDefaults(const ccl::doc::Color& pen, float penWidth,
                      float eraserWidth, const QuickColors& colors) noexcept {
        penColor_ = pen;
        penWidth_ = std::clamp(penWidth, kMinWidth, kMaxWidth);
        eraserWidth_ = std::clamp(eraserWidth, kMinWidth, kMaxWidth);
        quickColors = colors;
    }

    QuickColors quickColors = kDefaultQuickColors;

private:
    // The eraser keeps its own size: it is usually wanted much wider than the
    // line it is rubbing out, and having to resize twice on every switch would
    // be tedious.
    float& WidthSlot() noexcept {
        return tool == Tool::Eraser ? eraserWidth_ : penWidth_;
    }
    const float& WidthSlot() const noexcept {
        return const_cast<ToolState*>(this)->WidthSlot();
    }

    ccl::doc::Color penColor_ = kDefaultQuickColors[1];  // green
    std::vector<ccl::doc::Color> recent_;
    float penWidth_ = 4.0f;
    float eraserWidth_ = 24.0f;
};

}  // namespace ccl::tool

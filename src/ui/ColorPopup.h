#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

#include "doc/Annotation.h"

namespace ccl::ui {

// Colour palette shown as a borderless popup.
//
// Deliberately not a dialog: it is opened by a keystroke in the middle of
// drawing, so it has no buttons. It stays open while a colour is being chosen
// and commits when the pointer goes elsewhere, which allows a swatch to be
// picked and then adjusted. The layout is a saturation/value field and a hue
// bar for mixing a colour over two rows of swatches -- the fixed template
// colours, and the recently used ones with the colour being mixed at their
// left end.
class ColorPopup {
public:
    // Returns the chosen colour, or nothing if it was dismissed.
    // Reports the colour as it is being chosen, before anything is committed.
    using ColorChanged = std::function<void(const ccl::doc::Color&)>;

    // scalePercent sizes the whole popup, on top of the monitor's DPI scaling.
    //
    // `onChange` fires on every adjustment. Applying the colour live matters
    // for more than preview: the click that dismisses the popup is delivered to
    // the window underneath, and without live updates that click would act on
    // the previous colour.
    std::optional<ccl::doc::Color> Show(HWND owner, POINT screen,
                                        const ccl::doc::Color& current,
                                        const std::vector<ccl::doc::Color>& recent,
                                        int scalePercent,
                                        ColorChanged onChange) noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Paint(HDC dc) noexcept;
    void RebuildFieldBitmap() noexcept;
    void HandlePress(POINT point) noexcept;
    void NotifyChange() noexcept;
    void Close(bool accepted) noexcept;

    // The recent row, with the colour currently being mixed occupying the
    // leftmost cell. That doubles as the preview, so no separate swatch is
    // needed to show what is being chosen.
    std::vector<ccl::doc::Color> RecentRow() const;

    HWND hwnd_ = nullptr;
    HBITMAP fieldBitmap_ = nullptr;
    int builtForHue_ = -1;

    // Layout in physical pixels, scaled for the monitor's DPI.
    int scale_ = 96;
    RECT fieldRect_{};
    RECT hueRect_{};
    RECT templateRect_{};
    RECT recentRect_{};
    int swatchWidth_ = 0;
    int swatchHeight_ = 0;

    float hue_ = 0.0f;         // 0..360
    float saturation_ = 0.0f;  // 0..1
    float value_ = 1.0f;       // 0..1

    std::vector<ccl::doc::Color> recent_;
    ColorChanged onChange_;

    bool draggingField_ = false;
    bool draggingHue_ = false;

    bool finished_ = false;
    bool accepted_ = false;
};

}  // namespace ccl::ui

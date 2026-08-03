#pragma once

#include <windows.h>

namespace ccl::ui {

// A magnifier that follows the cursor while a colour is being picked.
//
// The eyedropper samples a single pixel from anywhere on screen, and a single
// pixel is far too small to aim at. This shows the pixels around the cursor
// blown up, with the one that would actually be taken marked, and its value
// written underneath.
//
// It is a window of its own rather than something drawn into the capture,
// because sampling is not limited to the capture: the pointer is captured and
// the colour can come from any application on screen.
class ColorPreview {
public:
    ~ColorPreview();

    // Brings the magnifier up, or moves it if it is already up. `screen` is the
    // cursor position in virtual-desktop coordinates.
    void Update(POINT screen) noexcept;
    void Hide() noexcept;

    bool IsVisible() const noexcept { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    bool EnsureWindow() noexcept;
    void Paint(HDC dc) noexcept;

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    // Cursor position the current contents were read for.
    POINT sample_{};
    COLORREF color_ = 0;
    int scale_ = 96;
};

}  // namespace ccl::ui

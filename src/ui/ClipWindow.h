#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "render/Renderer.h"
#include "view/ViewState.h"

namespace ccl::doc {
class Document;
}

namespace ccl::render {
class D2DContext;
}

namespace ccl::ui {

// The window that holds a capture on top of everything else.
//
// It has no title bar and is always topmost -- both are fixed behaviour rather
// than settings. With no title bar there is nothing to drag, so window movement
// is bound to the middle button and the left button scrolls the image.
class ClipWindow {
public:
    bool Create(ccl::render::D2DContext& context,
                const ccl::doc::Document& document,
                const ccl::app::Settings& settings, POINT position,
                const std::wstring& sourceTitle, LONGLONG releasedAt) noexcept;

    void Run() noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Draw() noexcept;
    void OnWheel(int notches, WPARAM keys) noexcept;
    void OnKeyDown(WPARAM key) noexcept;

    // Resizes the window to the zoomed image (bounded by the work area),
    // re-clamps the scroll offset and repaints.
    void ApplyZoom() noexcept;
    void ApplyOpacity() noexcept;
    void FitToImage() noexcept;
    void UpdateTitle() noexcept;

    void SaveAs() noexcept;
    void CopyImage() noexcept;
    // Saves automatically before the capture is discarded, unless the image has
    // already been saved or Shift is held to skip it.
    void AutoSaveBeforeClosing() noexcept;

    SIZE ContentSize() const noexcept;
    SIZE ViewportSize() const noexcept;
    void ClampScroll() noexcept;

    HWND hwnd_ = nullptr;
    ccl::render::Renderer renderer_;
    ccl::render::D2DContext* context_ = nullptr;
    const ccl::doc::Document* document_ = nullptr;
    const ccl::app::Settings* settings_ = nullptr;
    ccl::view::ViewState view_;
    std::wstring sourceTitle_;
    bool saved_ = false;

    bool moving_ = false;
    POINT dragOrigin_{};
    RECT windowOrigin_{};

    bool scrolling_ = false;
    POINT scrollOrigin_{};
    POINT scrollStart_{};

    LONGLONG releasedAt_ = 0;
    bool reportedFirstFrame_ = false;
};

}  // namespace ccl::ui

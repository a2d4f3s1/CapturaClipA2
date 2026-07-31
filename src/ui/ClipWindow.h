#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "doc/History.h"
#include "render/Renderer.h"
#include "tool/ToolState.h"
#include "util/Timing.h"
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
// is bound to the middle button, and what the left button does depends on the
// selected tool.
class ClipWindow {
public:
    bool Create(ccl::render::D2DContext& context, ccl::doc::Document& document,
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
    void OnLeftDown(POINT client) noexcept;
    void OnMouseMove(POINT client) noexcept;
    void OnLeftUp() noexcept;

    // Resizes the window to the zoomed image (bounded by the work area),
    // re-clamps the scroll offset and repaints.
    void ApplyZoom() noexcept;
    void ApplyOpacity() noexcept;
    void FitToImage() noexcept;
    void UpdateTitle() noexcept;
    void UpdateCursor() noexcept;

    void ShowContextMenu(POINT screen) noexcept;
    void OnCommand(int command) noexcept;

    void SaveAs() noexcept;
    void CopyImage() noexcept;
    // Saves automatically before the capture is discarded, unless the image has
    // already been saved or Shift is held to skip it.
    void AutoSaveBeforeClosing() noexcept;

    // Client pixels to image coordinates, undoing zoom and scroll.
    D2D1_POINT_2F ToImage(POINT client) const noexcept;

    void BeginStroke(POINT client) noexcept;
    void ContinueStroke(POINT client) noexcept;
    void EndStroke() noexcept;
    void EraseAt(POINT client) noexcept;

    SIZE ContentSize() const noexcept;
    SIZE ViewportSize() const noexcept;
    void ClampScroll() noexcept;

    // True when the left button should scroll rather than use the active tool.
    bool ScrollingWithLeftButton() const noexcept;
    // True when the brush size outline should follow the cursor.
    bool ShowsBrushCursor() const noexcept;
    void TrackMouseLeave() noexcept;

    HWND hwnd_ = nullptr;
    ccl::render::Renderer renderer_;
    ccl::render::D2DContext* context_ = nullptr;
    ccl::doc::Document* document_ = nullptr;
    const ccl::app::Settings* settings_ = nullptr;

    ccl::view::ViewState view_;
    ccl::tool::ToolState tool_;
    ccl::doc::History history_;

    std::wstring sourceTitle_;
    bool saved_ = false;

    bool moving_ = false;
    POINT dragOrigin_{};
    RECT windowOrigin_{};

    bool scrolling_ = false;
    POINT scrollOrigin_{};
    POINT scrollStart_{};

    bool drawing_ = false;
    bool straightLine_ = false;
    ccl::doc::Stroke activeStroke_;

    bool erasing_ = false;
    bool erasedAny_ = false;

    bool spaceHeld_ = false;

    POINT lastCursor_{};
    bool cursorInside_ = false;
    bool trackingLeave_ = false;

    LONGLONG releasedAt_ = 0;
    bool reportedFirstFrame_ = false;
    ccl::timing::FrameStats drawStats_;
};

}  // namespace ccl::ui

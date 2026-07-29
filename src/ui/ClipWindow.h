#pragma once

#include <windows.h>

#include "render/Renderer.h"

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
// is bound to the middle button; the left button is reserved for scrolling.
class ClipWindow {
public:
    bool Create(const ccl::render::D2DContext& context,
                const ccl::doc::Document& document, POINT position,
                LONGLONG releasedAt) noexcept;

    void Run() noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void Draw() noexcept;

    HWND hwnd_ = nullptr;
    ccl::render::Renderer renderer_;

    bool moving_ = false;
    POINT dragOrigin_{};
    RECT windowOrigin_{};

    LONGLONG releasedAt_ = 0;
    bool reportedFirstFrame_ = false;
};

}  // namespace ccl::ui

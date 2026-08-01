#include "ui/ClipWindow.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "doc/Document.h"
#include "io/AutoSave.h"
#include "io/Clipboard.h"
#include "io/ImageCodec.h"
#include "render/D2DContext.h"
#include "ui/ColorPopup.h"
#include "util/Dpi.h"
#include "util/NameFormat.h"
#include "util/Timing.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kClipWindowClass[] = L"CapturaClipA2.ClipWindow";

// Pixels scrolled per arrow key press.
constexpr int kKeyScrollStep = 40;

// How far in from the edge counts as a resize grip. The window has no visible
// frame to grab, so the grip lives just inside the outline.
constexpr LONG kResizeGrip = 6;

// Points closer together than this are dropped while drawing, which keeps the
// stroke geometry small without any visible difference.
constexpr float kMinPointSpacing = 0.75f;

bool IsKeyDown(int key) noexcept {
    return (::GetKeyState(key) & 0x8000) != 0;
}

// Context menu command ids. Ranges leave room for the per-entry items that
// follow each base value.
enum MenuId : UINT {
    kMenuSave = 100,
    kMenuCopy,
    kMenuUndo,
    kMenuRedo,
    kMenuFit,
    kMenuAntialias,
    kMenuEyedropper,
    kMenuColorPicker,
    kMenuExit,

    kMenuToolBase = 200,   // + Tool
    kMenuWidthBase = 400,  // + index into kWidthPresets
    kMenuZoomBase = 500,   // + zoom in hundreds of percent
};

ccl::doc::Color FromColorRef(COLORREF value) noexcept {
    return ccl::doc::Color{GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
                           GetBValue(value) / 255.0f, 1.0f};
}

constexpr float kWidthPresets[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};

float DistanceToSegmentSquared(float px, float py, float ax, float ay, float bx,
                               float by) noexcept {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;

    float closestX = ax;
    float closestY = ay;
    if (lengthSquared > 0.0f) {
        float t = ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
        t = std::clamp(t, 0.0f, 1.0f);
        closestX = ax + t * dx;
        closestY = ay + t * dy;
    }

    const float ox = px - closestX;
    const float oy = py - closestY;
    return ox * ox + oy * oy;
}

// Measured against the segments rather than only the recorded points, so that
// a straight line -- which is just two points far apart -- can still be erased
// anywhere along its length.
bool StrokeHit(const ccl::doc::Stroke& stroke, D2D1_POINT_2F point,
               float radius) noexcept {
    if (stroke.points.empty()) {
        return false;
    }

    const float threshold = radius + stroke.width * 0.5f;
    const float thresholdSquared = threshold * threshold;

    if (stroke.points.size() == 1) {
        const float dx = stroke.points[0].x - point.x;
        const float dy = stroke.points[0].y - point.y;
        return dx * dx + dy * dy <= thresholdSquared;
    }

    for (size_t i = 1; i < stroke.points.size(); ++i) {
        if (DistanceToSegmentSquared(point.x, point.y, stroke.points[i - 1].x,
                                     stroke.points[i - 1].y, stroke.points[i].x,
                                     stroke.points[i].y) <= thresholdSquared) {
            return true;
        }
    }
    return false;
}

}  // namespace

LRESULT CALLBACK ClipWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<ClipWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        self->hwnd_ = hwnd;
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ClipWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_NCCALCSIZE:
            // Claim the whole window as client area: the window is sizable
            // (WS_THICKFRAME) but must not draw a system frame.
            //
            // Compared against FALSE rather than TRUE because the flag arrives
            // as any non-zero value; testing for exactly 1 let some calls fall
            // through to the default handling, which reserved the resize
            // border and made the frame visibly thicken on activation.
            if (wParam != FALSE) {
                return 0;
            }
            break;

        case WM_NCACTIVATE:
            // Passing -1 as the region tells the default handler not to repaint
            // the non-client area, which does not exist here.
            return ::DefWindowProcW(hwnd_, msg, wParam, -1);

        case WM_NCHITTEST: {
            // Supply the resize grips by hand, since there is no visible frame
            // for the system to hit-test against.
            RECT window{};
            ::GetWindowRect(hwnd_, &window);
            const LONG x = GET_X_LPARAM(lParam);
            const LONG y = GET_Y_LPARAM(lParam);

            const bool left = x < window.left + kResizeGrip;
            const bool right = x >= window.right - kResizeGrip;
            const bool top = y < window.top + kResizeGrip;
            const bool bottom = y >= window.bottom - kResizeGrip;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                UpdateCursor();
                return TRUE;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            Draw();
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_SIZE:
            renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
            ClampScroll();
            Draw();
            return 0;

        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA,
                    GET_KEYSTATE_WPARAM(wParam));
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_SPACE) {
                spaceHeld_ = true;
                UpdateCursor();
                return 0;
            }
            OnKeyDown(wParam);
            return 0;

        case WM_KEYUP:
            if (wParam == VK_SPACE) {
                spaceHeld_ = false;
                UpdateCursor();
            }
            return 0;

        case WM_MBUTTONDOWN:
            moving_ = true;
            ::GetCursorPos(&dragOrigin_);
            ::GetWindowRect(hwnd_, &windowOrigin_);
            ::SetCapture(hwnd_);
            return 0;

        case WM_LBUTTONDOWN:
            OnLeftDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_LBUTTONUP:
            OnLeftUp();
            return 0;

        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            cursorInside_ = false;
            Draw();
            return 0;

        case WM_MBUTTONUP:
            if (moving_) {
                moving_ = false;
                ::ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            moving_ = false;
            scrolling_ = false;
            sampling_ = false;
            if (drawing_) {
                EndStroke();
            }
            erasing_ = false;
            return 0;

        case WM_RBUTTONUP: {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ClientToScreen(hwnd_, &screen);
            ShowContextMenu(screen);
            return 0;
        }

        case WM_CLOSE:
            ccl::timing::ReportFrames(L"clip window draw", drawStats_);
            AutoSaveBeforeClosing();
            ::DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

bool ClipWindow::ScrollingWithLeftButton() const noexcept {
    return spaceHeld_ || tool_.tool == ccl::tool::Tool::View;
}

bool ClipWindow::ShowsBrushCursor() const noexcept {
    return cursorInside_ && !ScrollingWithLeftButton() &&
           (tool_.tool == ccl::tool::Tool::Pen ||
            tool_.tool == ccl::tool::Tool::Eraser);
}

void ClipWindow::TrackMouseLeave() noexcept {
    cursorInside_ = true;
    if (trackingLeave_) {
        return;
    }
    // Needed to get WM_MOUSELEAVE, without which the brush outline would stay
    // painted after the cursor has gone.
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd_;
    trackingLeave_ = ::TrackMouseEvent(&track) != FALSE;
}

void ClipWindow::UpdateCursor() noexcept {
    if (ScrollingWithLeftButton()) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    }

    if (tool_.tool == ccl::tool::Tool::Eyedropper) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
        return;
    }

    if (tool_.tool == ccl::tool::Tool::Pen ||
        tool_.tool == ccl::tool::Tool::Eraser) {
        // The size ring stands in for the pointer. A crosshair drawn on top of
        // it just sits in the middle and hides how big the brush actually is.
        ::SetCursor(nullptr);
        return;
    }

    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
}

D2D1_POINT_2F ClipWindow::ToImage(POINT client) const noexcept {
    const float zoom = view_.Zoom();
    const POINT scroll = view_.Scroll();
    const auto border = static_cast<float>(ccl::render::kWindowBorder);

    return D2D1::Point2F(
        (static_cast<float>(client.x) - border + static_cast<float>(scroll.x)) / zoom,
        (static_cast<float>(client.y) - border + static_cast<float>(scroll.y)) / zoom);
}

void ClipWindow::BeginStroke(POINT client) noexcept {
    activeStroke_ = ccl::doc::Stroke{};
    activeStroke_.color = tool_.Color();
    activeStroke_.width = tool_.Width();
    activeStroke_.antialias = tool_.antialias;

    const D2D1_POINT_2F point = ToImage(client);
    activeStroke_.points.push_back({point.x, point.y, 1.0f});

    drawing_ = true;
    // Decided at press time and held for the whole stroke, so the line does not
    // flip between freehand and straight midway through.
    straightLine_ = IsKeyDown(VK_SHIFT);
    ::SetCapture(hwnd_);
    Draw();
}

void ClipWindow::ContinueStroke(POINT client) noexcept {
    const D2D1_POINT_2F point = ToImage(client);

    if (straightLine_) {
        activeStroke_.points.resize(1);
        activeStroke_.points.push_back({point.x, point.y, 1.0f});
    } else {
        const auto& last = activeStroke_.points.back();
        if (std::abs(last.x - point.x) < kMinPointSpacing &&
            std::abs(last.y - point.y) < kMinPointSpacing) {
            return;
        }
        activeStroke_.points.push_back({point.x, point.y, 1.0f});
    }
    Draw();
}

void ClipWindow::EndStroke() noexcept {
    if (!drawing_) {
        return;
    }
    drawing_ = false;
    ::ReleaseCapture();

    if (document_ != nullptr && !activeStroke_.points.empty()) {
        history_.Record(document_->Annotations());

        ccl::doc::Annotation annotation;
        annotation.kind = ccl::doc::AnnotationKind::Stroke;
        annotation.stroke = std::move(activeStroke_);
        document_->Annotations().push_back(std::move(annotation));
    }

    activeStroke_ = ccl::doc::Stroke{};
    Draw();
}

void ClipWindow::EraseAt(POINT client) noexcept {
    if (document_ == nullptr) {
        return;
    }

    auto& annotations = document_->Annotations();
    const D2D1_POINT_2F point = ToImage(client);
    const float radius = tool_.Width() * 0.5f;

    std::vector<size_t> victims;
    for (size_t i = 0; i < annotations.size(); ++i) {
        if (annotations[i].kind == ccl::doc::AnnotationKind::Stroke &&
            StrokeHit(annotations[i].stroke, point, radius)) {
            victims.push_back(i);
        }
    }
    if (victims.empty()) {
        return;
    }

    // One undo entry per erase drag, not per stroke removed.
    if (!erasedAny_) {
        history_.Record(annotations);
        erasedAny_ = true;
    }

    for (size_t i = victims.size(); i > 0; --i) {
        annotations.erase(annotations.begin() +
                          static_cast<std::ptrdiff_t>(victims[i - 1]));
    }
}

void ClipWindow::OnLeftDown(POINT client) noexcept {
    // Alt+click samples a colour without leaving the current tool, matching
    // the shortcut image editors use.
    if (IsKeyDown(VK_MENU) && tool_.tool != ccl::tool::Tool::Eyedropper) {
        if (PickColorAt(client)) {
            Draw();
        }
        return;
    }

    if (ScrollingWithLeftButton()) {
        scrolling_ = true;
        ::GetCursorPos(&scrollOrigin_);
        scrollStart_ = view_.Scroll();
        ::SetCapture(hwnd_);
        return;
    }

    switch (tool_.tool) {
        case ccl::tool::Tool::Pen:
            BeginStroke(client);
            return;
        case ccl::tool::Tool::Eraser:
            erasing_ = true;
            erasedAny_ = false;
            ::SetCapture(hwnd_);
            EraseAt(client);
            Draw();
            return;

        case ccl::tool::Tool::Eyedropper:
            // Captured so the sample can be taken from anywhere on screen,
            // not just from inside this window.
            ::SetCapture(hwnd_);
            sampling_ = true;
            return;
        default:
            return;
    }
}

void ClipWindow::OnMouseMove(POINT client) noexcept {
    lastCursor_ = client;
    TrackMouseLeave();

    if (moving_) {
        POINT now{};
        ::GetCursorPos(&now);
        ::SetWindowPos(hwnd_, nullptr,
                       windowOrigin_.left + (now.x - dragOrigin_.x),
                       windowOrigin_.top + (now.y - dragOrigin_.y), 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    if (scrolling_) {
        POINT now{};
        ::GetCursorPos(&now);
        // Dragging moves the image itself, so the scroll offset goes the
        // opposite way to the cursor.
        view_.SetScroll(POINT{scrollStart_.x - (now.x - scrollOrigin_.x),
                              scrollStart_.y - (now.y - scrollOrigin_.y)},
                        ContentSize(), ViewportSize());
        Draw();
        return;
    }

    if (sampling_) {
        if (PickColorAt(client)) {
            Draw();
        }
        return;
    }

    if (drawing_) {
        ContinueStroke(client);
        return;
    }

    if (erasing_) {
        EraseAt(client);
    }

    if (ShowsBrushCursor() || erasing_) {
        // Only ask for a repaint rather than drawing here. Moving the pointer
        // generates far more messages than the screen can show, and letting
        // them collapse into a single WM_PAINT keeps the cost proportional to
        // what is actually displayed.
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ClipWindow::OnLeftUp() noexcept {
    if (sampling_) {
        sampling_ = false;
        ::ReleaseCapture();
        // One sample, then back to whatever tool was in use before.
        SelectTool(toolBeforeEyedropper_);
        return;
    }

    if (drawing_) {
        EndStroke();
        return;
    }
    if (scrolling_ || erasing_) {
        scrolling_ = false;
        erasing_ = false;
        ::ReleaseCapture();
    }
}

SIZE ClipWindow::ContentSize() const noexcept {
    if (document_ == nullptr) {
        return SIZE{0, 0};
    }
    return view_.ContentSize(document_->Width(), document_->Height());
}

SIZE ClipWindow::ViewportSize() const noexcept {
    RECT client{};
    if (!::GetClientRect(hwnd_, &client)) {
        return SIZE{0, 0};
    }
    const LONG border = 2 * ccl::render::kWindowBorder;
    return SIZE{std::max(0L, client.right - client.left - border),
                std::max(0L, client.bottom - client.top - border)};
}

void ClipWindow::ClampScroll() noexcept {
    view_.SetScroll(view_.Scroll(), ContentSize(), ViewportSize());
}

void ClipWindow::OnWheel(int notches, WPARAM keys) noexcept {
    if (notches == 0) {
        return;
    }

    if ((keys & MK_CONTROL) != 0) {
        view_.StepZoom(notches, (keys & MK_SHIFT) != 0);
        ApplyZoom();
    } else {
        view_.StepOpacity(notches);
        ApplyOpacity();
    }
}

void ClipWindow::OnKeyDown(WPARAM key) noexcept {
    if (IsKeyDown(VK_CONTROL)) {
        switch (key) {
            case 'S':
                SaveAs();
                return;
            case 'C':
                CopyImage();
                return;
            case 'Z':
                if (document_ != nullptr &&
                    history_.Undo(document_->Annotations())) {
                    Draw();
                }
                return;
            case 'Y':
                if (document_ != nullptr &&
                    history_.Redo(document_->Annotations())) {
                    Draw();
                }
                return;
            default:
                break;
        }
    }

    // Shift+digit picks a quick colour; the digits alone are zoom presets.
    if (IsKeyDown(VK_SHIFT) && key >= '1' && key <= '8') {
        tool_.UseColor(ccl::tool::kQuickColors[key - '1']);
        return;
    }

    switch (key) {
        case 'V':
            SelectTool(ccl::tool::Tool::View);
            return;
        case 'B':
            SelectTool(ccl::tool::Tool::Pen);
            return;
        case 'E':
            SelectTool(ccl::tool::Tool::Eraser);
            return;

        case 'I':
            ChooseColorFromPicker();
            return;

        case VK_OEM_4:  // [
            tool_.StepWidth(-1);
            UpdateTitle();
            Draw();
            return;
        case VK_OEM_6:  // ]
            tool_.StepWidth(1);
            UpdateTitle();
            Draw();
            return;

        case 'A':
            tool_.antialias = !tool_.antialias;
            UpdateTitle();
            Draw();
            return;

        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
            view_.SetZoom(static_cast<float>(key - '0'));
            ApplyZoom();
            return;

        case 'F':
            FitToImage();
            return;

        case VK_LEFT:
            view_.ScrollBy(-kKeyScrollStep, 0, ContentSize(), ViewportSize());
            Draw();
            return;

        case VK_RIGHT:
            view_.ScrollBy(kKeyScrollStep, 0, ContentSize(), ViewportSize());
            Draw();
            return;

        case VK_UP:
            view_.ScrollBy(0, -kKeyScrollStep, ContentSize(), ViewportSize());
            Draw();
            return;

        case VK_DOWN:
            view_.ScrollBy(0, kKeyScrollStep, ContentSize(), ViewportSize());
            Draw();
            return;

        default:
            return;
    }
}

void ClipWindow::ApplyZoom() noexcept {
    const SIZE content = ContentSize();
    const LONG frame = 2 * ccl::render::kWindowBorder;

    // Keep the window on screen: past the work area the image is scrolled
    // instead of the window growing beyond the monitor.
    RECT work{};
    const HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    }
    const LONG maxWidth =
        work.right > work.left ? work.right - work.left : content.cx;
    const LONG maxHeight =
        work.bottom > work.top ? work.bottom - work.top : content.cy;

    ::SetWindowPos(hwnd_, nullptr, 0, 0,
                   std::min(content.cx, maxWidth) + frame,
                   std::min(content.cy, maxHeight) + frame,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    ClampScroll();
    UpdateTitle();
    Draw();
}

void ClipWindow::UpdateTitle() noexcept {
    // No title bar, so this surfaces in the taskbar button. It is also the
    // only feedback for what the current zoom is.
    const std::wstring& format =
        settings_ != nullptr ? settings_->titleFormat : sourceTitle_;
    std::wstring name = ccl::util::ExpandPlaceholders(format, sourceTitle_);
    if (name.empty()) {
        name = L"CapturaClipA2";
    }

    const int zoom = static_cast<int>(std::lround(view_.Zoom() * 100.0f));

    wchar_t title[440];
    switch (tool_.tool) {
        case ccl::tool::Tool::Pen:
            ::swprintf_s(title, L"%s  %d%%  Pen %.0fpx%s", name.c_str(), zoom,
                         tool_.Width(), tool_.antialias ? L"" : L" (aliased)");
            break;
        case ccl::tool::Tool::Eraser:
            ::swprintf_s(title, L"%s  %d%%  Eraser %.0fpx", name.c_str(), zoom,
                         tool_.Width());
            break;
        case ccl::tool::Tool::Eyedropper:
            ::swprintf_s(title, L"%s  %d%%  Eyedropper", name.c_str(), zoom);
            break;
        default:
            ::swprintf_s(title, L"%s  %d%%", name.c_str(), zoom);
            break;
    }
    ::SetWindowTextW(hwnd_, title);
}

void ClipWindow::ApplyOpacity() noexcept {
    ::SetLayeredWindowAttributes(hwnd_, 0, view_.Opacity(), LWA_ALPHA);
}

void ClipWindow::FitToImage() noexcept {
    view_.SetScroll(POINT{0, 0}, ContentSize(), ViewportSize());
    ApplyZoom();
}

void ClipWindow::ChooseColorFromPicker() noexcept {
    POINT screen = lastCursor_;
    ::ClientToScreen(hwnd_, &screen);

    const ccl::doc::Color original = tool_.Color();

    ColorPopup popup;
    const auto chosen = popup.Show(
        hwnd_, screen, original, tool_.RecentColors(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this](const ccl::doc::Color& colour) {
            // Applied without recording it: dragging across a gradient would
            // otherwise fill the recent list with every shade passed over.
            tool_.SetColor(colour);
            Draw();
        });

    if (chosen.has_value()) {
        tool_.UseColor(*chosen);
    } else {
        tool_.SetColor(original);
    }
    Draw();
}

bool ClipWindow::PickColorAt(POINT client) noexcept {
    // Read straight off the screen rather than out of the captured image, so
    // the eyedropper works anywhere -- over the capture, over its annotations,
    // or over another application entirely.
    POINT screen = client;
    ::ClientToScreen(hwnd_, &screen);

    const HDC screenDc = ::GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }

    const COLORREF sample = ::GetPixel(screenDc, screen.x, screen.y);
    ::ReleaseDC(nullptr, screenDc);

    if (sample == CLR_INVALID) {
        return false;
    }

    tool_.UseColor(FromColorRef(sample));
    return true;
}

void ClipWindow::SelectTool(ccl::tool::Tool tool) noexcept {
    if (tool == ccl::tool::Tool::Eyedropper &&
        tool_.tool != ccl::tool::Tool::Eyedropper) {
        toolBeforeEyedropper_ = tool_.tool;
    }
    tool_.tool = tool;
    UpdateCursor();
    UpdateTitle();
    Draw();
}

void ClipWindow::ShowContextMenu(POINT screen) noexcept {
    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const UINT checked = MF_STRING | MF_CHECKED;
    const UINT plain = MF_STRING;

    ::AppendMenuW(menu, plain, kMenuSave, L"保存...\tCtrl+S");
    ::AppendMenuW(menu, plain, kMenuCopy, L"クリップボードにコピー\tCtrl+C");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(menu, plain | (history_.CanUndo() ? 0u : MF_GRAYED), kMenuUndo,
                  L"元に戻す\tCtrl+Z");
    ::AppendMenuW(menu, plain | (history_.CanRedo() ? 0u : MF_GRAYED), kMenuRedo,
                  L"やり直し\tCtrl+Y");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const HMENU tools = ::CreatePopupMenu();
    const auto toolEntry = [&](ccl::tool::Tool tool, const wchar_t* label) {
        ::AppendMenuW(tools, tool_.tool == tool ? checked : plain,
                      kMenuToolBase + static_cast<UINT>(tool), label);
    };
    toolEntry(ccl::tool::Tool::View, L"ビュー\tV");
    toolEntry(ccl::tool::Tool::Pen, L"ペン\tB");
    toolEntry(ccl::tool::Tool::Eraser, L"消しゴム\tE");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"ツール");

    ::AppendMenuW(menu, plain, kMenuColorPicker, L"色...\tI");
    ::AppendMenuW(menu, plain, kMenuEyedropper, L"画面から色を拾う");

    const HMENU widths = ::CreatePopupMenu();
    for (size_t i = 0; i < ARRAYSIZE(kWidthPresets); ++i) {
        wchar_t label[32];
        ::swprintf_s(label, L"%.0f px", kWidthPresets[i]);
        ::AppendMenuW(widths,
                      tool_.Width() == kWidthPresets[i] ? checked : plain,
                      kMenuWidthBase + static_cast<UINT>(i), label);
    }
    ::AppendMenuW(widths, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(widths, tool_.antialias ? checked : plain, kMenuAntialias,
                  L"なめらかにする\tA");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widths), L"線");

    const HMENU zoom = ::CreatePopupMenu();
    for (int percent = 100; percent <= 500; percent += 100) {
        wchar_t label[32];
        ::swprintf_s(label, L"%d%%\t%d", percent, percent / 100);
        const bool active =
            std::lround(view_.Zoom() * 100.0f) == static_cast<long>(percent);
        ::AppendMenuW(zoom, active ? checked : plain,
                      kMenuZoomBase + static_cast<UINT>(percent), label);
    }
    ::AppendMenuW(zoom, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(zoom, plain, kMenuFit, L"画像サイズに合わせる\tF");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), L"表示");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, plain, kMenuExit, L"終了");

    // TPM_RETURNCMD hands the choice back directly, which avoids routing it
    // through WM_COMMAND for a menu that only exists for the duration of the
    // call.
    const int command = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        screen.x, screen.y, 0, hwnd_, nullptr);

    ::DestroyMenu(menu);

    if (command != 0) {
        OnCommand(command);
    }
}

void ClipWindow::OnCommand(int command) noexcept {
    const auto id = static_cast<UINT>(command);

    if (id >= kMenuZoomBase) {
        view_.SetZoom(static_cast<float>(id - kMenuZoomBase) / 100.0f);
        ApplyZoom();
        return;
    }
    if (id >= kMenuWidthBase) {
        tool_.SetWidth(kWidthPresets[id - kMenuWidthBase]);
        UpdateTitle();
        Draw();
        return;
    }
    if (id >= kMenuToolBase) {
        SelectTool(static_cast<ccl::tool::Tool>(id - kMenuToolBase));
        return;
    }

    switch (id) {
        case kMenuSave:
            SaveAs();
            return;
        case kMenuCopy:
            CopyImage();
            return;
        case kMenuUndo:
            if (document_ != nullptr && history_.Undo(document_->Annotations())) {
                Draw();
            }
            return;
        case kMenuRedo:
            if (document_ != nullptr && history_.Redo(document_->Annotations())) {
                Draw();
            }
            return;
        case kMenuFit:
            FitToImage();
            return;
        case kMenuAntialias:
            tool_.antialias = !tool_.antialias;
            UpdateTitle();
            Draw();
            return;
        case kMenuEyedropper:
            SelectTool(ccl::tool::Tool::Eyedropper);
            return;
        case kMenuColorPicker:
            ChooseColorFromPicker();
            return;
        case kMenuExit:
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        default:
            return;
    }
}

void ClipWindow::SaveAs() noexcept {
    if (context_ == nullptr || document_ == nullptr || settings_ == nullptr) {
        return;
    }

    // Filter order has to match the format picked from nFilterIndex below.
    static constexpr wchar_t kFilter[] =
        L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0Bitmap (*.bmp)\0*.bmp\0\0";

    ccl::app::ImageFormat format = settings_->defaultFormat;
    DWORD filterIndex = 1;
    switch (format) {
        case ccl::app::ImageFormat::Jpeg: filterIndex = 2; break;
        case ccl::app::ImageFormat::Bmp: filterIndex = 3; break;
        default: filterIndex = 1; break;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    wchar_t path[MAX_PATH];
    ::swprintf_s(path, L"%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth,
                 now.wDay, now.wHour, now.wMinute, now.wSecond);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = kFilter;
    dialog.nFilterIndex = filterIndex;
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = ccl::io::ExtensionFor(format);
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!::GetSaveFileNameW(&dialog)) {
        return;
    }

    switch (dialog.nFilterIndex) {
        case 2: format = ccl::app::ImageFormat::Jpeg; break;
        case 3: format = ccl::app::ImageFormat::Bmp; break;
        default: format = ccl::app::ImageFormat::Png; break;
    }

    if (ccl::io::SaveImage(*context_, document_->Image(), path, format,
                           settings_->jpegQuality)) {
        saved_ = true;
    } else {
        ::MessageBoxW(hwnd_, L"Failed to save the image.", L"CapturaClipA2",
                      MB_ICONERROR | MB_OK);
    }
}

void ClipWindow::CopyImage() noexcept {
    if (document_ == nullptr) {
        return;
    }
    ccl::io::CopyToClipboard(hwnd_, document_->Image());
}

void ClipWindow::AutoSaveBeforeClosing() noexcept {
    if (saved_ || context_ == nullptr || document_ == nullptr ||
        settings_ == nullptr) {
        return;
    }
    // Holding Shift while closing skips the automatic save.
    if (IsKeyDown(VK_SHIFT)) {
        return;
    }

    ccl::io::AutoSaveImage(*context_, document_->Image(), *settings_,
                           sourceTitle_);
}

void ClipWindow::Draw() noexcept {
    const LONGLONG frameStart = ccl::timing::Now();

    ccl::render::BrushCursor cursor{};
    const bool showCursor = ShowsBrushCursor();
    if (showCursor) {
        cursor.position = ToImage(lastCursor_);
        cursor.radius = tool_.Width() * 0.5f;
        cursor.antialias = tool_.antialias;
    }

    renderer_.Draw(view_, drawing_ ? &activeStroke_ : nullptr,
                   showCursor ? &cursor : nullptr);

    // Frames before the window is actually on screen are not representative,
    // so they are kept out of the statistics.
    if (reportedFirstFrame_) {
        drawStats_.Add(ccl::timing::MillisecondsSince(frameStart));
    }
}

bool ClipWindow::Create(ccl::render::D2DContext& context,
                        ccl::doc::Document& document,
                        const ccl::app::Settings& settings, POINT position,
                        const std::wstring& sourceTitle,
                        LONGLONG releasedAt) noexcept {
    if (!document.IsValid()) {
        return false;
    }
    releasedAt_ = releasedAt;
    context_ = &context;
    document_ = &document;
    settings_ = &settings;
    sourceTitle_ = sourceTitle;
    view_.SetZoomStepPercent(settings.zoomStepPercent);
    renderer_.SetSmoothScaling(settings.smoothScaling);
    ccl::timing::Stopwatch watch;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ClipWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClipWindowClass;

    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_EX_APPWINDOW forces a taskbar button even though the window has no
    // title bar, so it is obvious whether a capture is still alive. WS_EX_LAYERED
    // is what makes the window translucent; at full opacity it costs nothing
    // visible. Both become settings alongside the other appearance options.
    //
    // WS_THICKFRAME makes the window sizable; WM_NCCALCSIZE then hides the
    // frame it would otherwise draw, and WM_NCHITTEST supplies the grips.
    //
    // The window is the image plus the outline on each side, and it is placed
    // so that the image itself lands exactly where the selection was.
    const int frame = 2 * ccl::render::kWindowBorder;
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_LAYERED, kClipWindowClass,
        L"CapturaClipA2", WS_POPUP | WS_THICKFRAME,
        position.x - ccl::render::kWindowBorder,
        position.y - ccl::render::kWindowBorder, document.Width() + frame,
        document.Height() + frame, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    watch.Lap(L"  clip window create");

    // Fully transparent until the first frame is on it. Otherwise the window
    // is briefly visible as an undrawn white rectangle between being shown and
    // being painted.
    ::SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    UpdateTitle();
    renderer_.Attach(context, hwnd_);
    renderer_.SetDocument(&document);

    ::ShowWindow(hwnd_, SW_SHOW);
    Draw();

    // Reported here rather than inside Draw: window creation itself sends a
    // WM_SIZE, and the draw it triggers happens before there is anything to
    // draw onto, so treating that as the first frame measured the wrong thing.
    ccl::timing::Report(L"release -> window shown", releasedAt_);
    reportedFirstFrame_ = true;

    ApplyOpacity();

    // Keyboard and wheel messages go to the focused window, so the capture has
    // to take focus for its shortcuts to work without clicking it first.
    ::SetForegroundWindow(hwnd_);

    if (settings.copyOnCapture) {
        CopyImage();
    }
    return true;
}

void ClipWindow::Run() noexcept {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

}  // namespace ccl::ui

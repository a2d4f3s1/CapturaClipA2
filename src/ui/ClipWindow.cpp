#include "ui/ClipWindow.h"

#include <commdlg.h>
#include <imm.h>
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
    kMenuPressure,
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

COLORREF ToColorRef(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

// Posted to the window to finish text entry. The edit control handles these
// keys itself, and the commit has to happen after the control has finished
// with the message, so it is deferred rather than done inline.
//
// wParam carries the control being committed: clicking away starts a new entry
// before the posted message arrives, and without the check that message would
// immediately close the entry that just opened.
constexpr UINT kCommitTextMessage = WM_APP + 1;

// Edit controls separate lines with CRLF; text layout wants a bare LF. The two
// conversions are kept next to each other so they cannot drift apart -- getting
// only one of them right leaves text that looks fine until it is reopened.
std::wstring ToStoredLineEndings(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        if (character != L'\r') {
            result.push_back(character);
        }
    }
    return result;
}

std::wstring ToEditorLineEndings(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size() + 8);
    for (const wchar_t character : text) {
        if (character == L'\n') {
            result.push_back(L'\r');
        }
        result.push_back(character);
    }
    return result;
}

WNDPROC g_originalEditProc = nullptr;

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
    const auto commit = [hwnd]() {
        ::PostMessageW(::GetParent(hwnd), kCommitTextMessage,
                       reinterpret_cast<WPARAM>(hwnd), 0);
    };

    switch (msg) {
        case WM_KEYDOWN:
            // Enter inserts a line break, so committing needs its own gesture.
            if (wParam == VK_ESCAPE ||
                (wParam == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000))) {
                commit();
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            // Clicking away commits, which is what most people try first.
            commit();
            break;

        default:
            break;
    }
    return ::CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);
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

    if (stroke.points.size() == 1) {
        const float threshold = radius + stroke.points[0].width * 0.5f;
        const float dx = stroke.points[0].x - point.x;
        const float dy = stroke.points[0].y - point.y;
        return dx * dx + dy * dy <= threshold * threshold;
    }

    // Compared against each segment's own width, since a tapered line is much
    // thinner at one end than the other.
    for (size_t i = 1; i < stroke.points.size(); ++i) {
        const float widest =
            std::max(stroke.points[i - 1].width, stroke.points[i].width);
        const float threshold = radius + widest * 0.5f;

        if (DistanceToSegmentSquared(point.x, point.y, stroke.points[i - 1].x,
                                     stroke.points[i - 1].y, stroke.points[i].x,
                                     stroke.points[i].y) <=
            threshold * threshold) {
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

        case kCommitTextMessage:
            // Only if it is still the control that asked; see the message's
            // definition.
            if (reinterpret_cast<HWND>(wParam) == editor_) {
                CommitText();
            }
            return 0;

        case WM_COMMAND:
            // The editor draws with a transparent background, so the image
            // underneath has to be repainted as the text changes; otherwise
            // deleted characters leave their pixels behind.
            if (HIWORD(wParam) == EN_CHANGE &&
                reinterpret_cast<HWND>(lParam) == editor_) {
                ResizeEditor();
                Draw();
                return 0;
            }
            break;

        case WM_CTLCOLOREDIT: {
            // Drawn in the colour the text will end up, over the image rather
            // than over a filled box, so the editor previews the result
            // instead of covering it.
            const auto dc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(dc, ToColorRef(tool_.Color()));
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetStockObject(NULL_BRUSH));
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

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
            if (HandlePointerMessage(msg, wParam)) {
                return 0;
            }
            // Not a pen: let the default handling turn it into the mouse
            // messages the rest of this window expects.
            break;

        case WM_LBUTTONDOWN:
            // Ignored while a pen is down, since the same gesture would
            // otherwise be handled twice.
            if (penActive_) {
                return 0;
            }
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
            // Text still being typed is kept rather than discarded.
            CommitText();
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

    if (tool_.tool == ccl::tool::Tool::Text) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_IBEAM));
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

size_t ClipWindow::FindTextAt(D2D1_POINT_2F image) noexcept {
    if (document_ == nullptr) {
        return static_cast<size_t>(-1);
    }

    const auto& annotations = document_->Annotations();
    // Searched back to front so the topmost text wins, matching what is drawn.
    for (size_t i = annotations.size(); i > 0; --i) {
        const ccl::doc::Annotation& annotation = annotations[i - 1];
        if (annotation.kind != ccl::doc::AnnotationKind::Text) {
            continue;
        }

        D2D1_RECT_F bounds{};
        if (!renderer_.MeasureText(annotation.text, bounds)) {
            continue;
        }
        if (image.x >= bounds.left && image.x <= bounds.right &&
            image.y >= bounds.top && image.y <= bounds.bottom) {
            return i - 1;
        }
    }
    return static_cast<size_t>(-1);
}

void ClipWindow::BeginTextAt(POINT client) noexcept {
    if (settings_ == nullptr) {
        return;
    }
    // Clicking elsewhere finishes the previous piece of text rather than
    // discarding it.
    CommitText();

    const D2D1_POINT_2F image = ToImage(client);
    editorX_ = image.x;
    editorY_ = image.y;
    editingExisting_ = false;

    // Clicking on existing text reopens it. It is lifted out of the document
    // for the duration, so the old copy is not drawn underneath the editor,
    // and the state before that is what undo returns to.
    const size_t existing = FindTextAt(image);
    if (existing != static_cast<size_t>(-1) && document_ != nullptr) {
        history_.Record(document_->Annotations());

        editingOriginal_ = document_->Annotations()[existing].text;
        editingExisting_ = true;
        editorX_ = editingOriginal_.x;
        editorY_ = editingOriginal_.y;

        document_->Annotations().erase(document_->Annotations().begin() +
                                       static_cast<std::ptrdiff_t>(existing));
        Draw();

        // The editor has to open where the text actually is, not where the
        // click landed.
        const float zoom = view_.Zoom();
        const POINT scroll = view_.Scroll();
        const auto border = static_cast<float>(ccl::render::kWindowBorder);
        client.x = static_cast<LONG>(editorX_ * zoom + border - scroll.x);
        client.y = static_cast<LONG>(editorY_ * zoom + border - scroll.y);
    }

    const float zoom = view_.Zoom();
    const float fontSize =
        editingExisting_ ? editingOriginal_.fontSize : settings_->textFontSize;
    const int fontPixels =
        std::max(4, static_cast<int>(std::lround(fontSize * zoom)));

    // Roomy enough to type into; the text itself is what gets measured when it
    // is committed, so this only affects the editing box.
    const int width = std::max(80, static_cast<int>(fontPixels * 12));
    const int height = static_cast<int>(fontPixels * 1.6f);

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    // ES_WANTRETURN is what makes Enter insert a line break instead of being
    // treated as a default-button press.
    editor_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOHSCROLL |
            ES_AUTOVSCROLL | ES_NOHIDESEL,
        client.x, client.y, width, height, hwnd_, nullptr, instance, nullptr);
    if (editor_ == nullptr) {
        return;
    }

    // Matched to how the text will actually be drawn, so the box is a preview
    // rather than just an input field.
    editorFont_ = ::CreateFontW(-fontPixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE,
                                settings_->textFontFamily.c_str());
    if (editorFont_ != nullptr) {
        ::SendMessageW(editor_, WM_SETFONT,
                       reinterpret_cast<WPARAM>(editorFont_), TRUE);
    }

    // Edit controls inset their text by a few pixels. Text layout draws from
    // the origin it is given, so without clearing the margins the preview sits
    // slightly off from where the text will land.
    ::SendMessageW(editor_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(0, 0));

    if (editorBackground_ == nullptr) {
        editorBackground_ = ::CreateSolidBrush(RGB(24, 24, 24));
    }

    g_originalEditProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        editor_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc)));

    if (editingExisting_) {
        ::SetWindowTextW(editor_,
                         ToEditorLineEndings(editingOriginal_.text).c_str());
        // Caret at the end, which is where editing usually continues.
        ::SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    }

    ResizeEditor();
    ::SetFocus(editor_);
}

void ClipWindow::ResizeEditor() noexcept {
    if (editor_ == nullptr) {
        return;
    }

    const int lines = std::max(
        1, static_cast<int>(::SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));

    // Line height comes from the font actually in use rather than a guess, so
    // the box tracks the text at any size.
    int lineHeight = 16;
    const HDC dc = ::GetDC(editor_);
    if (dc != nullptr) {
        const HGDIOBJ previous =
            editorFont_ != nullptr ? ::SelectObject(dc, editorFont_) : nullptr;

        TEXTMETRICW metrics{};
        if (::GetTextMetricsW(dc, &metrics)) {
            lineHeight = metrics.tmHeight;
        }
        if (previous != nullptr) {
            ::SelectObject(dc, previous);
        }
        ::ReleaseDC(editor_, dc);
    }

    RECT current{};
    ::GetWindowRect(editor_, &current);

    ::SetWindowPos(editor_, nullptr, 0, 0, current.right - current.left,
                   lineHeight * lines + lineHeight / 4,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ClipWindow::TurnOffIme() noexcept {
    // Leaving the IME composing after text entry would swallow the single-key
    // shortcuts -- B, E, V and the rest -- so it is switched back to direct
    // input whenever text entry ends.
    const HIMC context = ::ImmGetContext(hwnd_);
    if (context != nullptr) {
        ::ImmSetOpenStatus(context, FALSE);
        ::ImmReleaseContext(hwnd_, context);
    }
}

void ClipWindow::DestroyEditor() noexcept {
    if (editor_ != nullptr) {
        ::DestroyWindow(editor_);
        editor_ = nullptr;
    }
    if (editorFont_ != nullptr) {
        ::DeleteObject(editorFont_);
        editorFont_ = nullptr;
    }
    ::SetFocus(hwnd_);
    TurnOffIme();
}

void ClipWindow::CommitText() noexcept {
    if (editor_ == nullptr) {
        return;
    }

    const int length = ::GetWindowTextLengthW(editor_);
    std::wstring text;
    if (length > 0) {
        std::wstring raw(static_cast<size_t>(length), L'\0');
        ::GetWindowTextW(editor_, raw.data(), length + 1);
        text = ToStoredLineEndings(raw);
    }

    const bool wasEditing = editingExisting_;
    editingExisting_ = false;
    DestroyEditor();

    if (text.empty() || document_ == nullptr || settings_ == nullptr) {
        // Emptying existing text is how it gets deleted; the removal was
        // already recorded when editing began.
        Draw();
        return;
    }

    // Editing an existing piece already recorded the state it started from.
    if (!wasEditing) {
        history_.Record(document_->Annotations());
    }

    ccl::doc::Annotation annotation;
    annotation.kind = ccl::doc::AnnotationKind::Text;

    // Re-edited text keeps the properties it was created with, so reopening it
    // does not silently restyle it with whatever is currently selected.
    annotation.text = wasEditing ? editingOriginal_ : ccl::doc::TextAnnotation{};
    annotation.text.text = std::move(text);
    annotation.text.x = editorX_;
    annotation.text.y = editorY_;

    if (!wasEditing) {
        annotation.text.fontSize = settings_->textFontSize;
        annotation.text.fontFamily = settings_->textFontFamily;
        annotation.text.color = tool_.Color();
        annotation.text.shadow = settings_->textShadow;
        annotation.text.outline = settings_->textOutline;
    }

    document_->Annotations().push_back(std::move(annotation));
    Draw();
}

void ClipWindow::CancelText() noexcept {
    DestroyEditor();
    Draw();
}

float ClipWindow::WidthForPressure(float pressure) const noexcept {
    if (!tool_.usePressure || settings_ == nullptr) {
        return tool_.Width();
    }
    const float minScale = settings_->pressureMinScale;
    return tool_.Width() * (minScale + (1.0f - minScale) * pressure);
}

void ClipWindow::BeginStroke(POINT client, float pressure) noexcept {
    activeStroke_ = ccl::doc::Stroke{};
    activeStroke_.color = tool_.Color();
    activeStroke_.antialias = tool_.antialias;

    const D2D1_POINT_2F point = ToImage(client);
    activeStroke_.points.push_back({point.x, point.y, WidthForPressure(pressure)});

    drawing_ = true;
    // Decided at press time and held for the whole stroke, so the line does not
    // flip between freehand and straight midway through.
    straightLine_ = IsKeyDown(VK_SHIFT);
    ::SetCapture(hwnd_);
    Draw();
}

void ClipWindow::ContinueStroke(POINT client, float pressure) noexcept {
    const D2D1_POINT_2F point = ToImage(client);

    if (straightLine_) {
        // Two points only. Both ends take a brush size rather than a pressure:
        // the force of putting a pen down is hard to aim, whereas [ and ] can
        // be nudged while the line is previewed. The far end keeps whatever
        // width it was given, so moving the pointer does not undo it.
        const float endWidth = activeStroke_.points.size() == 2
                                   ? activeStroke_.points.back().width
                                   : tool_.Width();
        activeStroke_.points.resize(1);
        activeStroke_.points.push_back({point.x, point.y, endWidth});
    } else {
        const auto& last = activeStroke_.points.back();
        if (std::abs(last.x - point.x) < kMinPointSpacing &&
            std::abs(last.y - point.y) < kMinPointSpacing) {
            return;
        }
        activeStroke_.points.push_back(
            {point.x, point.y, WidthForPressure(pressure)});
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

bool ClipWindow::HandlePointerMessage(UINT msg, WPARAM wParam) noexcept {
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);

    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!::GetPointerType(pointerId, &type) || type != PT_PEN) {
        // Mouse and touch keep going through the ordinary handlers.
        return false;
    }

    POINTER_PEN_INFO pen{};
    if (!::GetPointerPenInfo(pointerId, &pen)) {
        return false;
    }

    POINT client = pen.pointerInfo.ptPixelLocation;
    ::ScreenToClient(hwnd_, &client);

    // Pressure is reported 0-1024; a pen that does not report it sends 0, in
    // which case the stroke is drawn at full width.
    const bool reportsPressure = (pen.penMask & PEN_MASK_PRESSURE) != 0 &&
                                 pen.pressure > 0;
    const float pressure =
        reportsPressure ? static_cast<float>(pen.pressure) / 1024.0f : 1.0f;

    // The tail end of the pen is a natural eraser, so it selects the eraser for
    // the duration of the stroke and then hands the tool back.
    const bool inverted =
        (pen.penFlags & (PEN_FLAG_INVERTED | PEN_FLAG_ERASER)) != 0;

    switch (msg) {
        case WM_POINTERDOWN: {
            penActive_ = true;
            lastCursor_ = client;
            TrackMouseLeave();

            if (inverted && tool_.tool != ccl::tool::Tool::Eraser) {
                toolBeforePenEraser_ = tool_.tool;
                penEraserActive_ = true;
                tool_.tool = ccl::tool::Tool::Eraser;
                UpdateTitle();
            }

            if (ScrollingWithLeftButton()) {
                scrolling_ = true;
                ::GetCursorPos(&scrollOrigin_);
                scrollStart_ = view_.Scroll();
            } else if (tool_.tool == ccl::tool::Tool::Eraser) {
                erasing_ = true;
                erasedAny_ = false;
                EraseAt(client);
                Draw();
            } else if (tool_.tool == ccl::tool::Tool::Pen) {
                BeginStroke(client, pressure);
            }
            return true;
        }

        case WM_POINTERUPDATE:
            lastCursor_ = client;
            if (drawing_) {
                ContinueStroke(client, pressure);
            } else if (erasing_) {
                EraseAt(client);
                Draw();
            } else if (scrolling_) {
                POINT now{};
                ::GetCursorPos(&now);
                view_.SetScroll(POINT{scrollStart_.x - (now.x - scrollOrigin_.x),
                                      scrollStart_.y - (now.y - scrollOrigin_.y)},
                                ContentSize(), ViewportSize());
                Draw();
            } else if (ShowsBrushCursor()) {
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;

        case WM_POINTERUP:
            penActive_ = false;
            if (drawing_) {
                EndStroke();
            }
            erasing_ = false;
            scrolling_ = false;

            if (penEraserActive_) {
                penEraserActive_ = false;
                tool_.tool = toolBeforePenEraser_;
                UpdateCursor();
                UpdateTitle();
                Draw();
            }
            return true;

        default:
            return false;
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
            BeginStroke(client, 1.0f);
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

        case ccl::tool::Tool::Text:
            BeginTextAt(client);
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
        ContinueStroke(client, 1.0f);
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
        case 'T':
            SelectTool(ccl::tool::Tool::Text);
            return;

        case 'I':
            ChooseColorFromPicker();
            return;

        case VK_OEM_4:  // [
        case VK_OEM_6: {  // ]
            const int steps = key == VK_OEM_6 ? 1 : -1;

            // While a straight line is being previewed, the size keys retarget
            // one of its ends -- the far end normally, the starting end with
            // Ctrl -- which is how a taper is dialled in without having to aim
            // the pen pressure.
            //
            // Each end steps from its own current width rather than from the
            // brush size, so adjusting one end leaves the other alone.
            if (drawing_ && straightLine_ && activeStroke_.points.size() == 2) {
                float& target = IsKeyDown(VK_CONTROL)
                                    ? activeStroke_.points.front().width
                                    : activeStroke_.points.back().width;
                target = ccl::tool::ToolState::SteppedWidth(target, steps);
            } else {
                tool_.StepWidth(steps);
            }

            UpdateTitle();
            Draw();
            return;
        }

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
            // While a straight line is being drawn, both ends are reported, so
            // it is clear which one the size keys just changed.
            if (drawing_ && straightLine_ && activeStroke_.points.size() == 2) {
                ::swprintf_s(title, L"%s  %d%%  Line %.0f → %.0fpx", name.c_str(),
                             zoom, activeStroke_.points.front().width,
                             activeStroke_.points.back().width);
                break;
            }
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
        case ccl::tool::Tool::Text:
            ::swprintf_s(title, L"%s  %d%%  Text %.0fpx", name.c_str(), zoom,
                         settings_ != nullptr ? settings_->textFontSize : 0.0f);
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
    if (tool != ccl::tool::Tool::Text) {
        // Leaving text entry keeps what was typed and returns the keyboard to
        // direct input, so the shortcuts work again.
        CommitText();
        TurnOffIme();
    }

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
    toolEntry(ccl::tool::Tool::Text, L"テキスト\tT");
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
    ::AppendMenuW(widths, tool_.usePressure ? checked : plain, kMenuPressure,
                  L"筆圧を使う");
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
        case kMenuPressure:
            tool_.usePressure = !tool_.usePressure;
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

    // Direct2D paints the whole window including the area the edit control
    // occupies, so the control has to be told to put itself back on top.
    if (editor_ != nullptr) {
        ::RedrawWindow(editor_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

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
    tool_.usePressure = settings.usePenPressure;
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

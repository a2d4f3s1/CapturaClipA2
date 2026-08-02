#include "ui/ClipWindow.h"

#include <commdlg.h>
#include <imm.h>
#include <richedit.h>
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

// Movement below this counts as a click rather than a drag, so that a slight
// tremor while clicking text does not nudge it.
constexpr int kClickThreshold = 3;


// The rich edit control lives in its own library, which has to be loaded
// before the class can be used.
bool EnsureRichEditLoaded() noexcept {
    static const HMODULE library = ::LoadLibraryW(L"Msftedit.dll");
    return library != nullptr;
}

// Twips are twentieths of a point, which is what character formatting uses.
LONG PixelsToTwips(float pixels, UINT dpi) noexcept {
    return static_cast<LONG>(std::lround(pixels * 1440.0f /
                                         static_cast<float>(dpi)));
}

float TwipsToPixels(LONG twips, UINT dpi) noexcept {
    return static_cast<float>(twips) * static_cast<float>(dpi) / 1440.0f;
}

// Font size steps, in image pixels.
constexpr float kMinFontSize = 6.0f;
constexpr float kMaxFontSize = 400.0f;

// Menu entries per column before starting a new one, so a long font list stays
// on screen instead of running off the bottom.
constexpr int kMenuColumnLength = 30;

// Installed font families, in the user's locale, sorted for browsing.
const std::vector<std::wstring>& InstalledFonts(IDWriteFactory* writer) {
    static std::vector<std::wstring> fonts = [writer] {
        std::vector<std::wstring> names;
        if (writer == nullptr) {
            return names;
        }

        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
        if (FAILED(writer->GetSystemFontCollection(&collection))) {
            return names;
        }

        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        if (::GetUserDefaultLocaleName(locale, ARRAYSIZE(locale)) == 0) {
            ::wcscpy_s(locale, L"en-us");
        }

        const UINT32 count = collection->GetFontFamilyCount();
        names.reserve(count);

        for (UINT32 i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(i, &family))) {
                continue;
            }

            Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> familyNames;
            if (FAILED(family->GetFamilyNames(&familyNames))) {
                continue;
            }

            // Prefer the name in the user's language, falling back to the
            // first one the font offers.
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(familyNames->FindLocaleName(locale, &index, &exists)) ||
                !exists) {
                index = 0;
            }

            UINT32 length = 0;
            if (FAILED(familyNames->GetStringLength(index, &length)) ||
                length == 0) {
                continue;
            }

            std::wstring name(length + 1, L'\0');
            if (SUCCEEDED(familyNames->GetString(index, name.data(),
                                                 length + 1))) {
                name.resize(length);
                names.push_back(std::move(name));
            }
        }

        std::sort(names.begin(), names.end());
        return names;
    }();
    return fonts;
}

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
    kMenuBold,
    kMenuItalic,
    kMenuUnderline,
    kMenuStrikethrough,
    kMenuTextOutline,
    kMenuTextShadow,
    kMenuCommitText,
    kMenuEyedropper,
    kMenuColorPicker,
    kMenuExit,

    // Range bases, kept together at the end. Putting one in the middle renumbers
    // everything after it into that range, which is how the colour entry ended
    // up being treated as a font choice.
    kMenuToolBase = 200,    // + Tool
    kMenuWidthBase = 400,   // + index into kWidthPresets
    kMenuZoomBase = 500,    // + zoom in hundreds of percent
    kMenuFontBase = 1000,   // + index into the installed font list
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

// Styling shortcut pressed inside the editor; wParam is the key.
constexpr UINT kStyleTextMessage = WM_APP + 2;

// The editor's content may have changed and its box needs re-measuring. Driven
// from the messages that can change it rather than from a change notification,
// which rich edit does not deliver the way a plain edit control does.
constexpr UINT kResizeEditorMessage = WM_APP + 3;

// Right click inside the editor. The styling menu has to be reachable from
// there: while typing, the keys that would open it are just characters.
constexpr UINT kEditorMenuMessage = WM_APP + 4;

// Edit controls separate lines with CRLF; text layout wants a bare LF. The two
// conversions are kept next to each other so they cannot drift apart -- getting
// only one of them right leaves text that looks fine until it is reopened.
std::wstring ToStoredLineEndings(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'\r') {
            result.push_back(text[i]);
            continue;
        }

        // A carriage return is a line break in its own right -- rich edit
        // returns bare CR where a plain edit control returns CRLF. Dropping it
        // instead of translating it silently deleted every line break.
        result.push_back(L'\n');
        if (i + 1 < text.size() && text[i + 1] == L'\n') {
            ++i;
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
        case WM_KEYDOWN: {
            // Enter inserts a line break, so committing needs its own gesture.
            if (wParam == VK_ESCAPE ||
                (wParam == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000))) {
                commit();
                return 0;
            }

            // Styling shortcuts have to be intercepted here: the edit control
            // would otherwise swallow them without acting on them.
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (wParam == 'B' || wParam == 'I' || wParam == 'U' ||
                 wParam == VK_OEM_4 || wParam == VK_OEM_6)) {
                ::PostMessageW(::GetParent(hwnd), kStyleTextMessage, wParam, 0);
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS:
            // Clicking away commits, which is what most people try first.
            commit();
            break;

        case WM_CONTEXTMENU:
            ::PostMessageW(::GetParent(hwnd), kEditorMenuMessage,
                           reinterpret_cast<WPARAM>(hwnd), lParam);
            return 0;

        default:
            break;
    }

    const LRESULT result =
        ::CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);

    // Re-measure after anything that can change the content. Driven from the
    // messages themselves because the change notification does not arrive.
    switch (msg) {
        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
            ::PostMessageW(::GetParent(hwnd), kResizeEditorMessage,
                           reinterpret_cast<WPARAM>(hwnd), 0);
            break;

        default:
            break;
    }
    return result;
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
            // Only if it is still the control that asked, and not while a menu
            // or the palette has taken focus; see the message's definition and
            // suppressCommitDepth_.
            if (reinterpret_cast<HWND>(wParam) == editor_ &&
                suppressCommitDepth_ == 0) {
                CommitText();
            }
            return 0;

        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header != nullptr && header->hwndFrom == editor_ &&
                header->code == EN_REQUESTRESIZE) {
                // The control reports the rectangle its content needs; growing
                // to match is what keeps every line visible.
                const auto* request = reinterpret_cast<const REQRESIZE*>(lParam);
                const int margin = std::max(
                    4, static_cast<int>(std::lround(
                           (settings_ != nullptr ? settings_->textFontSize
                                                 : 16.0f) *
                           view_.Zoom() * 0.35f)));

                const int width = std::max(
                    40, static_cast<int>(request->rc.right - request->rc.left) +
                            margin);
                const int height = std::max(
                    12, static_cast<int>(request->rc.bottom - request->rc.top) +
                            margin);

                RECT current{};
                ::GetWindowRect(editor_, &current);
                const int currentWidth = current.right - current.left;
                const int currentHeight = current.bottom - current.top;

                // Only when it actually changes. The control asks to be
                // resized on every keystroke, and resizing it regardless made
                // it flicker.
                if (width == currentWidth && height == currentHeight) {
                    return 0;
                }

                ::SetWindowPos(editor_, nullptr, 0, 0, width, height,
                               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

                // Only a shrink exposes image that the parent has to repaint;
                // growing is covered by the control itself.
                if (width < currentWidth || height < currentHeight) {
                    Draw();
                }
                return 0;
            }
            break;
        }

        case kResizeEditorMessage:
            // No repaint here: the resize notification repaints only if the box
            // actually shrank. Redrawing on every keystroke made the editor
            // flicker.
            if (reinterpret_cast<HWND>(wParam) == editor_) {
                ResizeEditor();
            }
            return 0;

        case kEditorMenuMessage: {
            if (reinterpret_cast<HWND>(wParam) != editor_) {
                return 0;
            }
            POINT where{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // A keyboard-invoked menu reports (-1, -1); put it on the caret.
            if (where.x == -1 && where.y == -1) {
                ::GetCursorPos(&where);
            }
            ShowTextStyleMenu(where);
            return 0;
        }

        case kStyleTextMessage:
            switch (wParam) {
                case 'B':
                    tool_.textBold = !tool_.textBold;
                    ApplyTextEffect(CFM_BOLD, CFE_BOLD, tool_.textBold);
                    break;
                case 'I':
                    tool_.textItalic = !tool_.textItalic;
                    ApplyTextEffect(CFM_ITALIC, CFE_ITALIC, tool_.textItalic);
                    break;
                case 'U':
                    tool_.textUnderline = !tool_.textUnderline;
                    ApplyTextEffect(CFM_UNDERLINE, CFE_UNDERLINE,
                                    tool_.textUnderline);
                    break;
                // Ctrl with the brush size keys, since the keys alone are
                // characters while typing.
                case VK_OEM_4: StepTextSize(-1); break;
                case VK_OEM_6: StepTextSize(1); break;
                default: break;
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
        // Over existing text the press could move it, so the cursor says so;
        // elsewhere it starts new text.
        const bool overText = hoveredTextIndex_ != static_cast<size_t>(-1) ||
                              movingTextIndex_ != static_cast<size_t>(-1);
        ::SetCursor(::LoadCursorW(nullptr, overText ? IDC_SIZEALL : IDC_IBEAM));
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

void ClipWindow::EditTextAt(size_t index) noexcept {
    if (document_ == nullptr || index >= document_->Annotations().size()) {
        return;
    }
    CommitText();

    // Lifted out of the document for the duration, so the old copy is not
    // drawn underneath the editor, and so undo returns to the state before
    // editing began.
    history_.Record(document_->Annotations());

    editingOriginal_ = document_->Annotations()[index].text;
    editingExisting_ = true;
    editorX_ = editingOriginal_.x;
    editorY_ = editingOriginal_.y;

    // The styling of the text being edited becomes the current styling, so the
    // switches reflect it and changing one applies to this piece.
    tool_.SetColor(editingOriginal_.color);
    tool_.textBold = editingOriginal_.bold;
    tool_.textItalic = editingOriginal_.italic;
    tool_.textUnderline = editingOriginal_.underline;
    tool_.textStrikethrough = editingOriginal_.strikethrough;
    tool_.textShadow = editingOriginal_.shadow;
    tool_.textOutline = editingOriginal_.outline;

    document_->Annotations().erase(document_->Annotations().begin() +
                                   static_cast<std::ptrdiff_t>(index));
    Draw();

    // The editor opens where the text is, not where the click landed.
    const float zoom = view_.Zoom();
    const POINT scroll = view_.Scroll();
    const auto border = static_cast<float>(ccl::render::kWindowBorder);
    const POINT client{static_cast<LONG>(editorX_ * zoom + border - scroll.x),
                       static_cast<LONG>(editorY_ * zoom + border - scroll.y)};
    OpenEditor(client);
}

void ClipWindow::BeginTextAt(POINT client) noexcept {
    // Clicking elsewhere finishes the previous piece of text rather than
    // discarding it.
    CommitText();

    const D2D1_POINT_2F image = ToImage(client);
    editorX_ = image.x;
    editorY_ = image.y;
    editingExisting_ = false;
    OpenEditor(client);
}

void ClipWindow::OpenEditor(POINT client) noexcept {
    if (settings_ == nullptr) {
        return;
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
    // A rich edit control rather than a plain one, because the styling has to
    // be visible while typing: choosing which words to colour is not something
    // that can be done blind.
    if (!EnsureRichEditLoaded()) {
        return;
    }

    // ES_WANTRETURN is what makes Enter insert a line break instead of being
    // treated as a default-button press.
    //
    // No scrolling styles: the control is run "bottomless", meaning it reports
    // the size its content needs and the parent grows it to match, so nothing
    // ever scrolls out of view.
    editor_ = ::CreateWindowExW(
        0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL,
        client.x, client.y, width, height, hwnd_, nullptr, instance, nullptr);
    if (editor_ == nullptr) {
        return;
    }


    // System default background. Rich edit cannot be made transparent -- keying
    // a colour out was tried and destroyed the glyph edges -- and this at least
    // stays legible whatever colour the text is. It shows only while typing;
    // committed text is drawn straight onto the image.
    ::SendMessageW(editor_, EM_SETBKGNDCOLOR, 1, 0);

    // Word wrap off: the box has no fixed width to wrap against, so it grows
    // with the text and breaks only where a line break was typed.
    ::SendMessageW(editor_, EM_SETTARGETDEVICE, 0, 0);

    // ENM_REQUESTRESIZE is what makes the control report the size its content
    // needs, which is the supported way to keep it fitted to the text.
    ::SendMessageW(editor_, EM_SETEVENTMASK, 0,
                   ENM_CHANGE | ENM_REQUESTRESIZE);

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

        // Baseline styling first, then the ranges that differ from it, so that
        // reopening text shows exactly what was committed.
        ::SendMessageW(editor_, EM_SETSEL, 0, -1);
        ApplyCharFormat(true);

        for (const ccl::doc::TextRun& run : editingOriginal_.runs) {
            const CHARRANGE range{static_cast<LONG>(run.start),
                                  static_cast<LONG>(run.start + run.length)};
            ::SendMessageW(editor_, EM_EXSETSEL, 0,
                           reinterpret_cast<LPARAM>(&range));

            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE |
                            CFM_STRIKEOUT;
            format.crTextColor = ToColorRef(run.color);
            format.dwEffects = (run.bold ? CFE_BOLD : 0) |
                               (run.italic ? CFE_ITALIC : 0) |
                               (run.underline ? CFE_UNDERLINE : 0) |
                               (run.strikethrough ? CFE_STRIKEOUT : 0);
            ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                           reinterpret_cast<LPARAM>(&format));
        }

        // Caret at the end, which is where editing usually continues.
        ::SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    } else {
        ApplyCharFormat(true);
    }

    ApplyParagraphFormat();
    ResizeEditor();
    ::SetFocus(editor_);
}

void ClipWindow::SetTextFont(const std::wstring& family) noexcept {
    if (editor_ == nullptr) {
        return;
    }

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE;
    ::wcsncpy_s(format.szFaceName, family.c_str(), _TRUNCATE);

    ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));

    ResizeEditor();
    Draw();
}

void ClipWindow::StepTextSize(int steps) noexcept {
    if (editor_ == nullptr) {
        return;
    }

    // Reads the size of the selection and scales it, so the step is relative to
    // what is there rather than to a global setting. With a mixed selection the
    // control reports the first run's size, which becomes the new size for all
    // of it -- the same thing word processors do.
    CHARFORMAT2W current{};
    current.cbSize = sizeof(current);
    current.dwMask = CFM_SIZE;
    ::SendMessageW(editor_, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&current));

    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const float zoom = std::max(0.01f, view_.Zoom());
    float size = TwipsToPixels(current.yHeight, dpi) / zoom;
    if (size <= 0.0f) {
        size = settings_ != nullptr ? settings_->textFontSize : 30.0f;
    }

    for (int i = 0; i < steps; ++i) {
        size = std::min(kMaxFontSize, size * 1.15f + 0.5f);
    }
    for (int i = 0; i > steps; --i) {
        size = std::max(kMinFontSize, (size - 0.5f) / 1.15f);
    }

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_SIZE;
    format.yHeight = PixelsToTwips(size * zoom, dpi);
    ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));

    ResizeEditor();
    Draw();
}

void ClipWindow::ApplyTextEffect(DWORD mask, DWORD effect, bool enabled) noexcept {
    if (editor_ == nullptr) {
        return;
    }

    // Only the attribute being changed is written. Sending the full format
    // instead would reset the font and size along with it, undoing any
    // per-range changes -- and with nothing selected, it would also stop the
    // control carrying the previous character's formatting forward.
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = mask;
    format.dwEffects = enabled ? effect : 0;

    ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));

    ResizeEditor();
    Draw();
}

void ClipWindow::ApplyTextColor() noexcept {
    if (editor_ == nullptr) {
        return;
    }

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = ToColorRef(tool_.Color());

    ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));
    Draw();
}

void ClipWindow::ApplyCharFormat(bool wholeText) noexcept {
    if (editor_ == nullptr || settings_ == nullptr) {
        return;
    }

    const float fontSize =
        editingExisting_ ? editingOriginal_.fontSize : settings_->textFontSize;

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC |
                    CFM_UNDERLINE | CFM_STRIKEOUT;
    format.yHeight =
        PixelsToTwips(fontSize * view_.Zoom(), ccl::dpi::ForWindow(hwnd_));
    format.crTextColor = ToColorRef(tool_.Color());
    format.dwEffects = (tool_.textBold ? CFE_BOLD : 0) |
                       (tool_.textItalic ? CFE_ITALIC : 0) |
                       (tool_.textUnderline ? CFE_UNDERLINE : 0) |
                       (tool_.textStrikethrough ? CFE_STRIKEOUT : 0);
    ::wcsncpy_s(format.szFaceName, settings_->textFontFamily.c_str(), _TRUNCATE);

    // Applied to the selection so that styling affects the chosen characters,
    // or -- with nothing selected -- whatever is typed next.
    ::SendMessageW(editor_, EM_SETCHARFORMAT,
                   wholeText ? SCF_ALL : SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));
}

std::vector<ccl::doc::TextRun> ClipWindow::ReadRuns(int length) noexcept {
    std::vector<ccl::doc::TextRun> runs;
    if (editor_ == nullptr || length <= 0) {
        return runs;
    }

    CHARRANGE saved{};
    ::SendMessageW(editor_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));

    // Walked character by character, collapsing neighbours that share a style.
    // Annotations are short enough that the simple approach is fine.
    ccl::doc::TextRun current{};
    bool open = false;

    for (int i = 0; i < length; ++i) {
        const CHARRANGE single{i, i + 1};
        ::SendMessageW(editor_, EM_EXSETSEL, 0,
                       reinterpret_cast<LPARAM>(&single));

        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR | CFM_SIZE | CFM_FACE | CFM_BOLD |
                        CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
        ::SendMessageW(editor_, EM_GETCHARFORMAT, SCF_SELECTION,
                       reinterpret_cast<LPARAM>(&format));

        ccl::doc::TextRun style{};
        style.color = FromColorRef(format.crTextColor);
        style.fontFamily = format.szFaceName;
        // Back out of twips and out of the zoom the editor is displayed at, to
        // the size the text will be drawn at.
        style.fontSize =
            TwipsToPixels(format.yHeight, ccl::dpi::ForWindow(hwnd_)) /
            std::max(0.01f, view_.Zoom());
        style.bold = (format.dwEffects & CFE_BOLD) != 0;
        style.italic = (format.dwEffects & CFE_ITALIC) != 0;
        style.underline = (format.dwEffects & CFE_UNDERLINE) != 0;
        style.strikethrough = (format.dwEffects & CFE_STRIKEOUT) != 0;

        if (open && current.SameStyle(style)) {
            ++current.length;
            continue;
        }
        if (open) {
            runs.push_back(current);
        }
        current = style;
        current.start = static_cast<unsigned int>(i);
        current.length = 1;
        open = true;
    }
    if (open) {
        runs.push_back(current);
    }

    ::SendMessageW(editor_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    return runs;
}

void ClipWindow::ApplyParagraphFormat() noexcept {
    if (editor_ == nullptr) {
        return;
    }

    // Rich edit indents paragraphs and spaces them apart. Both have to be
    // cleared, or the typed text sits at a different place and a different
    // line pitch from where it will be drawn. Applied to the whole text,
    // because setting it before the text exists has no lasting effect.
    CHARRANGE saved{};
    ::SendMessageW(editor_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    ::SendMessageW(editor_, EM_SETSEL, 0, -1);

    PARAFORMAT2 paragraph{};
    paragraph.cbSize = sizeof(paragraph);
    paragraph.dwMask = PFM_LINESPACING | PFM_SPACEBEFORE | PFM_SPACEAFTER |
                       PFM_STARTINDENT | PFM_RIGHTINDENT | PFM_OFFSET;
    paragraph.bLineSpacingRule = 0;  // single
    paragraph.dyLineSpacing = 0;
    paragraph.dySpaceBefore = 0;
    paragraph.dySpaceAfter = 0;
    paragraph.dxStartIndent = 0;
    paragraph.dxRightIndent = 0;
    paragraph.dxOffset = 0;

    // Pinned to the line height the drawing will use. Left to its own devices
    // the control spaces lines differently, so typed and committed text did
    // not line up once there was more than one line.
    if (settings_ != nullptr) {
        ccl::doc::TextAnnotation probe;
        probe.fontSize = editingExisting_ ? editingOriginal_.fontSize
                                          : settings_->textFontSize;
        probe.fontFamily = settings_->textFontFamily;
        probe.bold = tool_.textBold;
        probe.italic = tool_.textItalic;

        float lineHeight = 0.0f;
        float baseline = 0.0f;
        if (renderer_.MeasureLine(probe, lineHeight, baseline) &&
            lineHeight > 0.0f) {
            paragraph.bLineSpacingRule = 4;  // exactly dyLineSpacing
            paragraph.dyLineSpacing = PixelsToTwips(
                lineHeight * view_.Zoom(), ccl::dpi::ForWindow(hwnd_));
        }
    }

    ::SendMessageW(editor_, EM_SETPARAFORMAT, 0,
                   reinterpret_cast<LPARAM>(&paragraph));

    ::SendMessageW(editor_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
}

void ClipWindow::ResizeEditor() noexcept {
    if (editor_ == nullptr || settings_ == nullptr) {
        return;
    }

    // Asks the control what size its content needs. The answer arrives as an
    // EN_REQUESTRESIZE notification, which is where the resize happens.
    //
    // Measuring it here instead was the mistake behind several attempts: the
    // positions the control reports are in its own scrolled coordinates, so
    // once the box was too short the measurement came back short as well and
    // it could never catch up.
    ::SendMessageW(editor_, EM_REQUESTRESIZE, 0, 0);
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

    // Committed at the position the text is actually sitting at, measured from
    // the control, rather than at the point that was clicked. The two differ by
    // however the control chooses to place its first line, and taking the
    // clicked point made the text jump on commit.
    {
        // Scrolled back first: positions come back in the control's scrolled
        // coordinates, so measuring while scrolled placed the text too high by
        // however far it had scrolled.
        ::SendMessageW(editor_, EM_SETSEL, 0, 0);
        ::SendMessageW(editor_, EM_SCROLLCARET, 0, 0);

        POINTL textOrigin{};
        ::SendMessageW(editor_, EM_POSFROMCHAR,
                       reinterpret_cast<WPARAM>(&textOrigin), 0);

        RECT editorRect{};
        ::GetWindowRect(editor_, &editorRect);
        POINT topLeft{editorRect.left, editorRect.top};
        ::ScreenToClient(hwnd_, &topLeft);

        const D2D1_POINT_2F origin =
            ToImage(POINT{topLeft.x + textOrigin.x, topLeft.y + textOrigin.y});
        editorX_ = origin.x;
        editorY_ = origin.y;
    }


    const int length = ::GetWindowTextLengthW(editor_);
    std::wstring text;
    std::vector<ccl::doc::TextRun> runs;
    if (length > 0) {
        std::wstring raw(static_cast<size_t>(length), L'\0');
        ::GetWindowTextW(editor_, raw.data(), length + 1);

        // Read before the text is normalised, because the styling positions
        // refer to the control's own character indices.
        runs = ReadRuns(static_cast<int>(::wcslen(raw.c_str())));
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

    // Re-edited text keeps the size and decoration it was created with; only
    // what the style switches cover is taken from the current state, which is
    // what was loaded from this text when editing began.
    annotation.text = wasEditing ? editingOriginal_ : ccl::doc::TextAnnotation{};
    annotation.text.text = std::move(text);
    annotation.text.x = editorX_;
    annotation.text.y = editorY_;

    if (!wasEditing) {
        annotation.text.fontSize = settings_->textFontSize;
        annotation.text.fontFamily = settings_->textFontFamily;
    }

    annotation.text.color = tool_.Color();
    annotation.text.bold = tool_.textBold;
    annotation.text.italic = tool_.textItalic;
    annotation.text.underline = tool_.textUnderline;
    annotation.text.strikethrough = tool_.textStrikethrough;
    annotation.text.shadow = tool_.textShadow;
    annotation.text.outline = tool_.textOutline;
    annotation.text.runs = std::move(runs);

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

        case ccl::tool::Tool::Text: {
            // Pressing on existing text starts a drag. Whether that turns out
            // to be a move or a click to edit is decided on release.
            const size_t existing = FindTextAt(ToImage(client));
            if (existing != static_cast<size_t>(-1) && document_ != nullptr) {
                movingTextIndex_ = existing;
                textDragStart_ = client;
                textDragOriginX_ = document_->Annotations()[existing].text.x;
                textDragOriginY_ = document_->Annotations()[existing].text.y;
                textDragMoved_ = false;
                ::SetCapture(hwnd_);
                return;
            }
            BeginTextAt(client);
            return;
        }
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

    if (movingTextIndex_ != static_cast<size_t>(-1)) {
        const int dx = client.x - textDragStart_.x;
        const int dy = client.y - textDragStart_.y;

        if (!textDragMoved_ && std::abs(dx) <= kClickThreshold &&
            std::abs(dy) <= kClickThreshold) {
            return;
        }
        if (!textDragMoved_ && document_ != nullptr) {
            textDragMoved_ = true;
            history_.Record(document_->Annotations());
        }

        if (document_ != nullptr &&
            movingTextIndex_ < document_->Annotations().size()) {
            const float zoom = view_.Zoom();
            auto& text = document_->Annotations()[movingTextIndex_].text;
            text.x = textDragOriginX_ + static_cast<float>(dx) / zoom;
            text.y = textDragOriginY_ + static_cast<float>(dy) / zoom;
            Draw();
        }
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

    // With the text tool, outline whatever is under the pointer so it is
    // obvious what clicking would open.
    if (tool_.tool == ccl::tool::Tool::Text && editor_ == nullptr) {
        const size_t hovered = FindTextAt(ToImage(client));
        if (hovered != hoveredTextIndex_) {
            hoveredTextIndex_ = hovered;
            UpdateCursor();
            Draw();
        }
        return;
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
    if (movingTextIndex_ != static_cast<size_t>(-1)) {
        const size_t index = movingTextIndex_;
        const bool moved = textDragMoved_;
        movingTextIndex_ = static_cast<size_t>(-1);
        textDragMoved_ = false;
        ::ReleaseCapture();

        // A press that never moved was a click, which opens the text.
        if (!moved) {
            EditTextAt(index);
        }
        return;
    }

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

    // Same reason as the styling menu: the palette takes focus, and that must
    // not end the edit in progress.
    ++suppressCommitDepth_;

    ColorPopup popup;
    const auto chosen = popup.Show(
        hwnd_, screen, original, tool_.RecentColors(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this](const ccl::doc::Color& colour) {
            // Applied without recording it: dragging across a gradient would
            // otherwise fill the recent list with every shade passed over.
            tool_.SetColor(colour);

            // While text is being edited the palette's own swatch is the
            // preview. Pushing every intermediate colour into the control and
            // repainting behind it made dragging crawl and the palette flicker;
            // the colour is applied once, on commit.
            if (editor_ == nullptr) {
                Draw();
            }
        });

    if (chosen.has_value()) {
        tool_.UseColor(*chosen);
        ApplyTextColor();
    } else {
        tool_.SetColor(original);
    }

    --suppressCommitDepth_;
    if (editor_ != nullptr) {
        ::SetFocus(editor_);
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

HMENU ClipWindow::BuildFontMenu() noexcept {
    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr || context_ == nullptr) {
        return menu;
    }

    // Which font the selection currently uses, so it can be ticked.
    std::wstring current;
    if (editor_ != nullptr) {
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_FACE;
        ::SendMessageW(editor_, EM_GETCHARFORMAT, SCF_SELECTION,
                       reinterpret_cast<LPARAM>(&format));
        current = format.szFaceName;
    } else if (settings_ != nullptr) {
        current = settings_->textFontFamily;
    }

    const auto& fonts = InstalledFonts(context_->Text());
    for (size_t i = 0; i < fonts.size(); ++i) {
        UINT flags = MF_STRING;
        if (fonts[i] == current) {
            flags |= MF_CHECKED;
        }
        // Wrapped into columns; the list is long enough to run off screen.
        if (i > 0 && i % kMenuColumnLength == 0) {
            flags |= MF_MENUBARBREAK;
        }
        ::AppendMenuW(menu, flags, kMenuFontBase + static_cast<UINT>(i),
                      fonts[i].c_str());
    }
    return menu;
}

void ClipWindow::ShowTextStyleMenu(POINT screen) noexcept {
    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const UINT checked = MF_STRING | MF_CHECKED;
    const UINT plain = MF_STRING;

    // Reached by right-clicking inside the editor, because while typing the
    // shortcut keys are ordinary characters.
    ::AppendMenuW(menu, plain, kMenuColorPicker, L"色...");
    ::AppendMenuW(menu, MF_POPUP,
                  reinterpret_cast<UINT_PTR>(BuildFontMenu()), L"フォント");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, tool_.textBold ? checked : plain, kMenuBold,
                  L"太字\tCtrl+B");
    ::AppendMenuW(menu, tool_.textItalic ? checked : plain, kMenuItalic,
                  L"斜体\tCtrl+I");
    ::AppendMenuW(menu, tool_.textUnderline ? checked : plain, kMenuUnderline,
                  L"下線\tCtrl+U");
    ::AppendMenuW(menu, tool_.textStrikethrough ? checked : plain,
                  kMenuStrikethrough, L"打ち消し線");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, tool_.textOutline ? checked : plain, kMenuTextOutline,
                  L"縁取り");
    ::AppendMenuW(menu, tool_.textShadow ? checked : plain, kMenuTextShadow,
                  L"影");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, plain, kMenuCommitText, L"確定\tEsc");

    // The menu takes focus off the editor, which must not be mistaken for
    // clicking away and end the edit.
    ++suppressCommitDepth_;

    const int command = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        screen.x, screen.y, 0, hwnd_, nullptr);

    ::DestroyMenu(menu);

    if (command != 0) {
        OnCommand(command);
    }

    --suppressCommitDepth_;
    if (editor_ != nullptr && command != kMenuCommitText) {
        ::SetFocus(editor_);
    }
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

    const HMENU textStyle = ::CreatePopupMenu();
    ::AppendMenuW(textStyle, tool_.textBold ? checked : plain, kMenuBold,
                  L"太字\tCtrl+B");
    ::AppendMenuW(textStyle, tool_.textItalic ? checked : plain, kMenuItalic,
                  L"斜体\tCtrl+I");
    ::AppendMenuW(textStyle, tool_.textUnderline ? checked : plain,
                  kMenuUnderline, L"下線\tCtrl+U");
    ::AppendMenuW(textStyle, tool_.textStrikethrough ? checked : plain,
                  kMenuStrikethrough, L"打ち消し線");
    ::AppendMenuW(textStyle, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(textStyle, tool_.textOutline ? checked : plain,
                  kMenuTextOutline, L"縁取り");
    ::AppendMenuW(textStyle, tool_.textShadow ? checked : plain,
                  kMenuTextShadow, L"影");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(textStyle),
                  L"文字");

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

    if (id >= kMenuFontBase && context_ != nullptr) {
        const auto& fonts = InstalledFonts(context_->Text());
        const size_t index = id - kMenuFontBase;
        if (index < fonts.size()) {
            SetTextFont(fonts[index]);
        }
        return;
    }

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

        case kMenuBold:
            tool_.textBold = !tool_.textBold;
            ApplyTextEffect(CFM_BOLD, CFE_BOLD, tool_.textBold);
            return;
        case kMenuItalic:
            tool_.textItalic = !tool_.textItalic;
            ApplyTextEffect(CFM_ITALIC, CFE_ITALIC, tool_.textItalic);
            return;
        case kMenuUnderline:
            tool_.textUnderline = !tool_.textUnderline;
            ApplyTextEffect(CFM_UNDERLINE, CFE_UNDERLINE, tool_.textUnderline);
            return;
        case kMenuStrikethrough:
            tool_.textStrikethrough = !tool_.textStrikethrough;
            ApplyTextEffect(CFM_STRIKEOUT, CFE_STRIKEOUT,
                            tool_.textStrikethrough);
            return;
        case kMenuTextOutline:
            tool_.textOutline = !tool_.textOutline;
            Draw();
            return;
        case kMenuTextShadow:
            tool_.textShadow = !tool_.textShadow;
            Draw();
            return;
        case kMenuCommitText:
            CommitText();
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

    D2D1_RECT_F highlight{};
    bool hasHighlight = false;
    if (tool_.tool == ccl::tool::Tool::Text && editor_ == nullptr &&
        document_ != nullptr &&
        hoveredTextIndex_ < document_->Annotations().size()) {
        const auto& annotation = document_->Annotations()[hoveredTextIndex_];
        if (annotation.kind == ccl::doc::AnnotationKind::Text) {
            hasHighlight = renderer_.MeasureText(annotation.text, highlight);
        }
    }

    renderer_.Draw(view_, drawing_ ? &activeStroke_ : nullptr,
                   showCursor ? &cursor : nullptr,
                   hasHighlight ? &highlight : nullptr);

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
    tool_.textShadow = settings.textShadow;
    tool_.textOutline = settings.textOutline;
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
    // WS_CLIPCHILDREN keeps this window's own drawing out of the area the text
    // editor occupies. Without it the editor had to be told to repaint after
    // every frame, which is what made it flicker.
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_LAYERED, kClipWindowClass,
        L"CapturaClipA2", WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
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

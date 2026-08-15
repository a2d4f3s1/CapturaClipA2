#include "ui/ClipWindow.h"

#include <commdlg.h>
#include <imm.h>
#include <richedit.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include "doc/Document.h"
#include "io/AutoSave.h"
#include "io/Clipboard.h"
#include "io/ImageCodec.h"
#include "io/ImageOps.h"
#include "render/D2DContext.h"
#include "res/Resources.h"
#include "ui/ColorPopup.h"
#include "ui/SettingsDialog.h"
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

// Drives the eyedropper's magnifier while nothing is being dragged.
constexpr UINT_PTR kColorPreviewTimer = 1;

// Brings the window back after it has been asked to hide.
constexpr UINT_PTR kHideTimer = 2;

// Which button a press came from, for working out its gesture.
enum : int {
    kButtonLeft,
    kButtonMiddle,
    kButtonRight,
};

// Points closer together than this are dropped while drawing, which keeps the
// stroke geometry small without any visible difference.
constexpr float kMinPointSpacing = 0.75f;

// Movement below this counts as a click rather than a drag, so that a slight
// tremor while clicking text does not nudge it.
constexpr int kClickThreshold = 3;

// A pause this long ends a run of wheel notches. Long enough that turning the
// wheel steadily stays one gesture, short enough that stopping to look and
// then zooming somewhere else picks the new place up.
constexpr ULONGLONG kZoomGestureGapMs = 400;


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

// Swings `to` round to the nearest multiple of `degrees` about `from`, keeping
// how far away it is.
//
// The distance is kept rather than dropping a perpendicular onto the snapped
// direction: pulling the pointer further out should still lengthen the line.
// Only its angle is taken over.
//
// Zero degrees is how the setting turns snapping off. A pointer that has not
// left the start comes back unchanged -- atan2(0, 0) is 0 and the distance is
// 0 with it, so the result is the start itself.
D2D1_POINT_2F SnapToAngle(D2D1_POINT_2F from, D2D1_POINT_2F to,
                          float degrees) noexcept {
    if (degrees <= 0.0f) {
        return to;
    }

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float step = degrees * std::numbers::pi_v<float> / 180.0f;
    const float angle = std::round(std::atan2(dy, dx) / step) * step;
    const float distance = std::sqrt(dx * dx + dy * dy);

    return D2D1::Point2F(from.x + std::cos(angle) * distance,
                         from.y + std::sin(angle) * distance);
}

// Context menu command ids. Ranges leave room for the per-entry items that
// follow each base value.
enum MenuId : UINT {
    kMenuSave = 100,
    kMenuOpen,
    kMenuCopy,
    kMenuPaste,
    kMenuUndo,
    kMenuRedo,
    kMenuFit,
    kMenuAntialias,
    kMenuHighlighter,
    kMenuPressure,
    kMenuBold,
    kMenuItalic,
    kMenuUnderline,
    kMenuStrikethrough,
    kMenuTextOutline,
    kMenuTextShadow,
    kMenuMosaic,
    kMenuBlur,
    kMenuClearSelection,
    kMenuCrop,
    kMenuRotateLeft,
    kMenuRotateRight,
    kMenuRotate180,
    kMenuFlipHorizontal,
    kMenuFlipVertical,
    kMenuConcatRight,
    kMenuConcatBottom,
    kMenuCaptureSelf,
    kMenuRecapture,
    kMenuSettings,
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

// Posted by the eyedropper's hook. The hook runs on every mouse event in the
// system, so it does as little as possible and leaves the work to the window.
constexpr UINT kEyedropperMoveMessage = WM_APP + 5;
constexpr UINT kEyedropperPickMessage = WM_APP + 6;
constexpr UINT kEyedropperCancelMessage = WM_APP + 7;
// The button has gone down with the eyedropper armed. Taking a colour starts
// here and is settled by letting go, so the sample can be aimed by dragging
// instead of having to be right on the first press.
constexpr UINT kEyedropperPressMessage = WM_APP + 8;

// The window whose eyedropper is armed, for the hook to reach. Only one can be
// armed at a time; the hook takes the mouse for the whole screen.
ClipWindow* g_eyedropperWindow = nullptr;

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
            // (WS_THICKFRAME) but must not draw a system frame. With a title
            // bar there is a real frame to keep, so this is left alone.
            //
            // Compared against FALSE rather than TRUE because the flag arrives
            // as any non-zero value; testing for exactly 1 let some calls fall
            // through to the default handling, which reserved the resize
            // border and made the frame visibly thicken on activation.
            if (!HasTitleBar() && wParam != FALSE) {
                return 0;
            }
            break;

        case WM_NCACTIVATE:
            // Passing -1 as the region tells the default handler not to repaint
            // the non-client area, which does not exist here.
            if (!HasTitleBar()) {
                return ::DefWindowProcW(hwnd_, msg, wParam, -1);
            }
            break;

        case WM_NCHITTEST: {
            if (HasTitleBar()) {
                break;  // there is a real frame for the system to hit-test
            }
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

        case WM_GETMINMAXINFO: {
            // A capture can legitimately be a few pixels across, and a sizable
            // frame comes with a minimum tracking size that would quietly hand
            // back a window larger than the one asked for. Everything worked
            // out from the requested size -- the viewport, and with it the
            // scroll range and the zoom anchor -- would then be against a
            // window that is not the one on screen.
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = 1;
            limits->ptMinTrackSize.y = 1;
            return 0;
        }

        case WM_SIZE:
            renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
            // While the zoom is being applied the caller repaints once at the
            // end, against the finished state. Painting here as well would
            // draw the whole window twice for every notch of the wheel, the
            // first time against a window that has been resized but not yet
            // scrolled or moved.
            if (applyingZoom_) {
                return 0;
            }
            ClampScroll();
            Draw();
            return 0;

        case WM_MOUSEWHEEL: {
            // The wheel reports where the pointer is in screen coordinates,
            // which is what the zoom needs to know to keep that spot still.
            POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(hwnd_, &client);
            OnWheel(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA,
                    GET_KEYSTATE_WPARAM(wParam), client);
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_SPACE) {
                spaceHeld_ = true;
                UpdateCursor();
                return 0;
            }
            // Shift pressed again part way through a straight line turns a
            // corner: the end it has reached settles, and the next stretch
            // carries on from there at the same width.
            //
            // Bit 30 of lParam is the previous key state. Without it, holding
            // Shift down would lay a corner on every auto-repeat.
            if (wParam == VK_SHIFT && drawing_ && straightLine_ &&
                (lParam & 0x40000000) == 0 &&
                activeStroke_.points.size() > fixedPoints_) {
                fixedPoints_ = activeStroke_.points.size();
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
            BeginDragCommand(kButtonMiddle);
            return 0;

        case WM_RBUTTONDOWN:
            rightButtonDown_ = true;
            // Started rather than acted on: whether this turns out to be a
            // drag or a click that opens the menu is only known on release.
            if (settings_ != nullptr &&
                settings_->mouse.LookupDrag(ccl::app::DragGesture::RightDrag) !=
                    ccl::app::MouseCommand::Count) {
                rightDragging_ = true;
                rightDragMoved_ = false;
                rightDragStart_ =
                    POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            }
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
            if (moving_ || scrolling_) {
                moving_ = false;
                scrolling_ = false;
                ::ReleaseCapture();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kColorPreviewTimer) {
                UpdateColorPreview();
                return 0;
            }
            if (wParam == kHideTimer) {
                StopHiding();
                return 0;
            }
            break;

        case kEyedropperMoveMessage: {
            // Released before anything else. A message that turns up after the
            // eyedropper has been put away still has to clear this, or the
            // hook stops sending movement for the rest of the session.
            previewPending_ = false;
            if (tool_.tool != ccl::tool::Tool::Eyedropper) {
                return 0;
            }
            const POINT screen{static_cast<LONG>(wParam),
                               static_cast<LONG>(lParam)};
            colorPreview_.Update(screen);

            // While the button is down the colour follows the pointer, so the
            // sample can be aimed instead of having to be hit first time. No
            // repaint for the same reason as below: nothing on screen shows it.
            if (sampling_) {
                POINT client = screen;
                ::ScreenToClient(hwnd_, &client);
                PickColorAt(client);
            }
            return 0;
        }

        case kEyedropperPressMessage: {
            if (tool_.tool != ccl::tool::Tool::Eyedropper) {
                return 0;
            }
            sampling_ = true;
            POINT client{static_cast<LONG>(wParam), static_cast<LONG>(lParam)};
            ::ScreenToClient(hwnd_, &client);
            // No repaint: this tool draws nothing, so the colour it has taken
            // is not on screen anywhere until the previous tool comes back.
            PickColorAt(client);
            return 0;
        }

        case kEyedropperPickMessage: {
            // The hook posts its messages, so one can arrive after a right
            // click has already put the eyedropper away.
            if (tool_.tool != ccl::tool::Tool::Eyedropper) {
                return 0;
            }
            POINT screen{static_cast<LONG>(wParam), static_cast<LONG>(lParam)};
            POINT client = screen;
            ::ScreenToClient(hwnd_, &client);
            if (PickColorAt(client)) {
                Draw();
            }
            // Letting go settles the colour, and the tool goes back to
            // whatever was in use before.
            EndEyedropper();
            return 0;
        }

        case kEyedropperCancelMessage:
            EndEyedropper();
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
            const bool pressedHere = rightButtonDown_;
            rightButtonDown_ = false;

            // The eyedropper takes over the whole screen while it is armed, so
            // there has to be a way out that does not involve finding this
            // window again. The menu would be the wrong thing to open here.
            if (tool_.tool == ccl::tool::Tool::Eyedropper) {
                EndEyedropper();
                return 0;
            }

            if (rightDragging_) {
                const bool dragged = rightDragMoved_;
                rightDragging_ = false;
                rightDragMoved_ = false;
                if (moving_ || scrolling_) {
                    moving_ = false;
                    scrolling_ = false;
                    ::ReleaseCapture();
                }
                // Only a press that actually moved was a drag. Anything else
                // is a click, and a click has to reach the menu.
                if (dragged) {
                    return 0;
                }
            }

            // A release with no press of ours behind it is not a click here.
            // It turns up when the eyedropper's hook took the press and then
            // let the mouse go, and when a press that began in another window
            // is released over this one. Treating either as a click is what
            // flashed the menu up on the way out of the eyedropper.
            if (!pressedHere) {
                return 0;
            }

            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ClientToScreen(hwnd_, &screen);
            ShowContextMenu(screen);
            return 0;
        }

        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wParam);
            wchar_t path[MAX_PATH]{};
            if (::DragQueryFileW(drop, 0, path, ARRAYSIZE(path)) > 0 &&
                context_ != nullptr) {
                ccl::capture::DibBuffer image =
                    ccl::io::LoadImageFile(*context_, path);
                if (image.IsValid()) {
                    const wchar_t* name = ::wcsrchr(path, L'\\');
                    ReplaceImage(std::move(image),
                                 name != nullptr ? name + 1 : path);
                }
            }
            ::DragFinish(drop);
            return 0;
        }

        case WM_CLOSE:
            // Only the timer is dropped, not the hiding: bringing the window
            // back for the instant before it is destroyed would just flash it.
            if (hideTimer_ != 0) {
                ::KillTimer(hwnd_, kHideTimer);
                hideTimer_ = 0;
            }
            // A hook left installed would outlive the window it posts to.
            RemoveEyedropperHook();
            SetColorPreviewActive(false);
            // Text still being typed is kept rather than discarded.
            CommitText();
            ccl::timing::ReportFrames(L"clip window draw", drawStats_);
            renderer_.ReportStats();
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

bool ClipWindow::HasSelection() const noexcept {
    if (!hasSelection_) {
        return false;
    }
    const D2D1_RECT_F area = SelectionRect();
    return area.right - area.left >= 1.0f && area.bottom - area.top >= 1.0f;
}

D2D1_RECT_F ClipWindow::SelectionRect() const noexcept {
    return D2D1::RectF(std::min(selectionAnchor_.x, selectionCursor_.x),
                       std::min(selectionAnchor_.y, selectionCursor_.y),
                       std::max(selectionAnchor_.x, selectionCursor_.x),
                       std::max(selectionAnchor_.y, selectionCursor_.y));
}

void ClipWindow::ApplyEffectToSelection(ccl::doc::EffectKind kind) noexcept {
    if (!HasSelection() || document_ == nullptr) {
        return;
    }

    history_.Record(document_->Annotations());

    const D2D1_RECT_F area = SelectionRect();

    ccl::doc::Annotation annotation;
    annotation.id = ccl::doc::NextAnnotationId();
    annotation.kind = ccl::doc::AnnotationKind::Effect;
    annotation.effect.kind = kind;
    annotation.effect.left = area.left;
    annotation.effect.top = area.top;
    annotation.effect.right = area.right;
    annotation.effect.bottom = area.bottom;

    // Reuses the strength last chosen for this effect. The first time round
    // there is none, so it is scaled to the area instead -- a small region
    // must not collapse into a single block.
    const bool mosaic = kind == ccl::doc::EffectKind::Mosaic;
    const float remembered = mosaic ? tool_.mosaicStrength : tool_.blurStrength;

    if (remembered >= 0.0f) {
        annotation.effect.strength = remembered;
    } else {
        const float span =
            std::min(area.right - area.left, area.bottom - area.top);
        annotation.effect.strength = mosaic
                                         ? std::clamp(span / 12.0f, 3.0f, 48.0f)
                                         : std::clamp(span / 16.0f, 2.0f, 24.0f);
    }

    if (mosaic) {
        tool_.mosaicStrength = annotation.effect.strength;
    } else {
        tool_.blurStrength = annotation.effect.strength;
    }

    document_->Annotations().push_back(std::move(annotation));

    // Stays adjustable so the strength can be dialled in against the result
    // rather than guessed at beforehand.
    adjustingEffectIndex_ = document_->Annotations().size() - 1;

    // The selection stays. Obscuring an area is rarely the end of what is
    // wanted there -- a mosaic gets swapped for a blur, or a second pass goes
    // on top -- and redrawing the same rectangle each time to find out is
    // work the program can save.
    UpdateTitle();
    Draw();
}

void ClipWindow::StepEffectStrength(int steps) noexcept {
    if (document_ == nullptr ||
        adjustingEffectIndex_ >= document_->Annotations().size()) {
        return;
    }

    auto& annotation = document_->Annotations()[adjustingEffectIndex_];
    if (annotation.kind != ccl::doc::AnnotationKind::Effect) {
        adjustingEffectIndex_ = static_cast<size_t>(-1);
        return;
    }

    auto& effect = annotation.effect;
    const bool mosaic = effect.kind == ccl::doc::EffectKind::Mosaic;
    const float maximum = mosaic ? 64.0f : 32.0f;

    // Zero is a valid strength meaning "leave the area alone", so an effect
    // placed by mistake can be turned off with [ instead of undone -- undo
    // would also take the selection with it.
    constexpr float kWeakest = 2.0f;

    float strength = effect.strength;
    for (int i = 0; i < steps; ++i) {
        strength = strength <= 0.0f ? kWeakest
                                    : std::min(maximum, strength * 1.3f + 0.5f);
    }
    for (int i = 0; i > steps; --i) {
        strength = (strength - 0.5f) / 1.3f;
        if (strength < kWeakest) {
            strength = 0.0f;
            break;
        }
    }

    if (strength == effect.strength) {
        return;
    }
    effect.strength = strength;

    // The cached pixels were produced at the old strength.
    renderer_.InvalidateEffect(annotation.id);

    if (mosaic) {
        tool_.mosaicStrength = strength;
    } else {
        tool_.blurStrength = strength;
    }

    UpdateTitle();
    Draw();
}

bool ClipWindow::ScrollingWithLeftButton() const noexcept {
    return spaceHeld_ || tool_.tool == ccl::tool::Tool::View;
}

ccl::app::DragGesture ClipWindow::GestureFor(int button) const noexcept {
    using ccl::app::DragGesture;

    const bool ctrl = IsKeyDown(VK_CONTROL);
    const bool shift = IsKeyDown(VK_SHIFT);
    const bool alt = IsKeyDown(VK_MENU);

    // Alt is not part of any gesture: it already means "sample the colour
    // without leaving the tool", which has to keep working over every button.
    if (alt) {
        return DragGesture::None;
    }

    switch (button) {
        case kButtonLeft:
            if (ctrl && !shift) return DragGesture::CtrlLeftDrag;
            // Shift with the left button draws a straight line, so it is not
            // offered as a gesture and must not be read as the bare one.
            if (!ctrl && !shift) return DragGesture::LeftDrag;
            return DragGesture::None;

        case kButtonMiddle:
            if (ctrl && !shift) return DragGesture::CtrlMiddleDrag;
            if (shift && !ctrl) return DragGesture::ShiftMiddleDrag;
            if (!ctrl && !shift) return DragGesture::MiddleDrag;
            return DragGesture::None;

        case kButtonRight:
            if (!ctrl && !shift) return DragGesture::RightDrag;
            return DragGesture::None;

        default:
            return DragGesture::None;
    }
}

void ClipWindow::BeginScrollDrag() noexcept {
    scrolling_ = true;
    ::GetCursorPos(&scrollOrigin_);
    scrollStart_ = view_.Scroll();
    ::SetCapture(hwnd_);
}

void ClipWindow::BeginWindowMove() noexcept {
    moving_ = true;
    ::GetCursorPos(&dragOrigin_);
    ::GetWindowRect(hwnd_, &windowOrigin_);
    ::SetCapture(hwnd_);
}

bool ClipWindow::BeginDragCommand(int button) noexcept {
    using ccl::app::DragGesture;
    using ccl::app::MouseCommand;

    if (settings_ == nullptr) {
        return false;
    }

    // Only the combinations that are gestures in their own right do anything.
    // A modifier that is not part of the assignment makes the press mean
    // nothing, the same way it does on the wheel.
    const DragGesture gesture = GestureFor(button);
    const MouseCommand command = settings_->mouse.LookupDrag(gesture);

    // The bare left button is never dispatched from here. It belongs to the
    // tool, and the view tool and space are how it comes free -- which is
    // handled where the tools are, so that assigning scrolling elsewhere
    // cannot take the view tool's whole reason for existing away.
    if (gesture == DragGesture::LeftDrag) {
        return false;
    }

    switch (command) {
        case MouseCommand::Scroll:
            BeginScrollDrag();
            return true;
        case MouseCommand::MoveWindow:
            BeginWindowMove();
            return true;
        default:
            return false;
    }
}

void ClipWindow::HideTemporarily() noexcept {
    if (settings_ == nullptr || hwnd_ == nullptr) {
        return;
    }
    // Nothing is drawn or moved while it is away, so anything mid-gesture is
    // let go of first rather than resumed against a window that has moved on.
    if (::GetCapture() == hwnd_) {
        ::ReleaseCapture();
    }

    ::ShowWindow(hwnd_, SW_HIDE);
    hideTimer_ = ::SetTimer(hwnd_, kHideTimer, settings_->hideDurationMs,
                            nullptr);
    // Without a timer the window would be gone for good, so it comes straight
    // back rather than being left somewhere it cannot be found.
    if (hideTimer_ == 0) {
        ::ShowWindow(hwnd_, SW_SHOWNA);
    }
}

void ClipWindow::StopHiding() noexcept {
    if (hideTimer_ == 0) {
        return;
    }
    ::KillTimer(hwnd_, kHideTimer);
    hideTimer_ = 0;
    // Shown without being activated: whatever the window was hiding from is
    // being worked in, and taking the focus back would interrupt it.
    ::ShowWindow(hwnd_, SW_SHOWNA);
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

    if (tool_.tool == ccl::tool::Tool::Select) {
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
    const auto border = static_cast<float>(BorderWidth());

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
    const auto border = static_cast<float>(BorderWidth());
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
    // A re-edit is a new annotation rather than a changed one, so it gets an
    // id of its own and nothing worked out for the old wording is reused.
    annotation.id = ccl::doc::NextAnnotationId();
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
    activeStroke_.highlighter = tool_.highlighter;

    const D2D1_POINT_2F point = ToImage(client);
    activeStroke_.points.push_back({point.x, point.y, WidthForPressure(pressure)});

    drawing_ = true;
    // Decided at press time and held for the whole stroke, so the line does not
    // flip between freehand and straight midway through.
    straightLine_ = IsKeyDown(VK_SHIFT);
    // Only the start is settled; no corners have been laid down yet.
    fixedPoints_ = 1;
    ::SetCapture(hwnd_);
    Draw();
}

void ClipWindow::ContinueStroke(POINT client, float pressure) noexcept {
    const D2D1_POINT_2F point = ToImage(client);

    if (straightLine_) {
        // Only the stretch past the settled points moves. Both ends take a
        // brush size rather than a pressure: the force of putting a pen down is
        // hard to aim, whereas [ and ] can be nudged while the line is
        // previewed.
        //
        // Where the moving end's width comes from:
        //  - a stretch already being aimed keeps the width it was given, so
        //    moving the pointer does not undo an adjustment
        //  - the first frame after a corner takes the corner's own width, so
        //    the two stretches meet without a step in the line
        //  - a line that has no corners yet starts from the brush size
        const float endWidth =
            (activeStroke_.points.size() > fixedPoints_ || fixedPoints_ > 1)
                ? activeStroke_.points.back().width
                : tool_.Width();
        activeStroke_.points.resize(fixedPoints_);

        // Alt swings the line onto a fixed angle. Read afresh every time
        // rather than latched at the press, so it can be reached for -- and
        // let go of -- while the line is still being aimed.
        const D2D1_POINT_2F start = D2D1::Point2F(
            activeStroke_.points.back().x, activeStroke_.points.back().y);
        const D2D1_POINT_2F end =
            IsKeyDown(VK_MENU) && settings_ != nullptr
                ? SnapToAngle(start, point, settings_->lineSnapDegrees)
                : point;

        activeStroke_.points.push_back({end.x, end.y, endWidth});
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
    fixedPoints_ = 1;
    ::ReleaseCapture();

    if (document_ != nullptr && !activeStroke_.points.empty()) {
        history_.Record(document_->Annotations());

        ccl::doc::Annotation annotation;
        annotation.id = ccl::doc::NextAnnotationId();
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
    // Alt reaches for the eyedropper without leaving the current tool, matching
    // the shortcut image editors use. It is the same eyedropper the key opens
    // -- magnifier, the whole screen to sample from, all of it -- and letting
    // go of the button hands the tool back.
    //
    // The press has already happened by the time the hook goes up, so the hook
    // only ever sees the movement and the release. That is enough: the release
    // is what settles the colour.
    if (IsKeyDown(VK_MENU) && tool_.tool != ccl::tool::Tool::Eyedropper) {
        SelectTool(ccl::tool::Tool::Eyedropper);
        sampling_ = true;
        PickColorAt(client);
        return;
    }

    // The tool has first claim on the bare left button; the view tool and
    // space are what free it up. Checked before the assignments below so that
    // moving scrolling onto another gesture cannot take this away.
    if (ScrollingWithLeftButton()) {
        BeginScrollDrag();
        return;
    }

    // Ctrl with the left button is assignable; the bare one is not, and
    // BeginDragCommand refuses it.
    if (BeginDragCommand(kButtonLeft)) {
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
            // The capture is already held; arming the tool took it.
            sampling_ = true;
            // Taken on the press as well as on every move: a click that does
            // not move at all is the ordinary way to use an eyedropper, and it
            // used to pick up nothing.
            if (PickColorAt(client)) {
                Draw();
            }
            return;

        case ccl::tool::Tool::Select:
            // Placing a new selection ends adjustment of the previous effect.
            adjustingEffectIndex_ = static_cast<size_t>(-1);
            selecting_ = true;
            hasSelection_ = true;
            selectionAnchor_ = ToImage(client);
            selectionCursor_ = selectionAnchor_;
            ::SetCapture(hwnd_);
            Draw();
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

    if (tool_.tool == ccl::tool::Tool::Eyedropper) {
        // Only while the cursor is over this window. Elsewhere the cursor
        // belongs to whichever window is under it, and the magnifier is what
        // says the eyedropper is armed.
        ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
    }

    // A right press only becomes a drag once it has moved far enough that it
    // cannot have been meant as a click on the menu.
    if (rightDragging_ && !rightDragMoved_) {
        if (std::abs(client.x - rightDragStart_.x) > kClickThreshold ||
            std::abs(client.y - rightDragStart_.y) > kClickThreshold) {
            rightDragMoved_ = true;
            BeginDragCommand(kButtonRight);
        }
    }

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

    if (selecting_) {
        selectionCursor_ = ToImage(client);
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
        // Only read the colour here when the hook is not up. The hook passes
        // movement through to this window as well as reporting it, so with one
        // installed both paths would fire and the pixel would be read twice.
        if (eyedropperHook_ == nullptr && PickColorAt(client)) {
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
    if (selecting_) {
        selecting_ = false;
        ::ReleaseCapture();

        // A click without a drag clears the selection rather than leaving a
        // zero-sized one behind.
        const D2D1_RECT_F area = SelectionRect();
        if (area.right - area.left < 1.0f || area.bottom - area.top < 1.0f) {
            hasSelection_ = false;
        }
        UpdateTitle();
        Draw();
        return;
    }

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
        // Where the button came up is the colour that was meant, which is not
        // necessarily where the last mouse message landed.
        POINT cursor{};
        if (::GetCursorPos(&cursor)) {
            ::ScreenToClient(hwnd_, &cursor);
            PickColorAt(cursor);
        }
        // One sample, then back to whatever tool was in use before.
        EndEyedropper();
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
    const LONG border = 2 * BorderWidth();
    return SIZE{std::max(0L, client.right - client.left - border),
                std::max(0L, client.bottom - client.top - border)};
}

bool ClipWindow::HasTitleBar() const noexcept {
    if (settings_ == nullptr) {
        return false;
    }
    return settings_->windowFrame == ccl::app::WindowFrame::Normal ||
           settings_->windowFrame == ccl::app::WindowFrame::ThinTitleBar;
}

bool ClipWindow::HasWindowBorder() const noexcept {
    // Only the bare styles draw their own outline. With a title bar the system
    // already draws a frame, and the frameless style is frameless on purpose.
    return settings_ != nullptr &&
           settings_->windowFrame == ccl::app::WindowFrame::NoTitleBar;
}

int ClipWindow::BorderWidth() const noexcept {
    return HasWindowBorder() ? ccl::render::kWindowBorder : 0;
}

SIZE ClipWindow::WindowSizeFor(SIZE content) const noexcept {
    RECT wanted{0, 0, content.cx, content.cy};
    if (HasTitleBar()) {
        // The caption and frame are the system's, so it decides how much room
        // they need.
        ::AdjustWindowRectEx(
            &wanted, static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)),
            FALSE,
            static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    } else {
        ::InflateRect(&wanted, BorderWidth(), BorderWidth());
    }
    return SIZE{wanted.right - wanted.left, wanted.bottom - wanted.top};
}

void ClipWindow::ClampScroll() noexcept {
    view_.SetScroll(view_.Scroll(), ContentSize(), ViewportSize());
}

void ClipWindow::OnWheel(int notches, WPARAM keys, POINT client) noexcept {
    using ccl::app::MouseCommand;
    using ccl::app::WheelGesture;

    if (notches == 0 || settings_ == nullptr) {
        return;
    }

    const ccl::app::MouseBindings& mouse = settings_->mouse;

    const bool ctrl = (keys & MK_CONTROL) != 0;
    const bool shift = (keys & MK_SHIFT) != 0;
    // The wheel message carries no flag for Alt, so it is read from the
    // keyboard instead.
    const bool alt = IsKeyDown(VK_MENU);

    // Only the combinations that are gestures in their own right do anything.
    // A modifier that is not part of the assignment makes the turn mean
    // nothing, rather than being ignored so that it still acts: holding a key
    // is how you say "not the usual thing".
    const auto exact = [&]() -> WheelGesture {
        if (!ctrl && !shift && !alt) return WheelGesture::Wheel;
        if (ctrl && !shift && !alt) return WheelGesture::CtrlWheel;
        if (!ctrl && shift && !alt) return WheelGesture::ShiftWheel;
        if (!ctrl && !shift && alt) return WheelGesture::AltWheel;
        return WheelGesture::None;
    }();

    MouseCommand command = mouse.LookupWheel(exact);
    bool fine = false;

    // Shift is the finer-step modifier for the zoom, wherever the zoom has
    // been put. It is not a gesture of its own, so the combination is looked
    // up again without it, and only the zoom answers to it.
    if (command == MouseCommand::Count && shift && !alt) {
        const WheelGesture withoutShift =
            ctrl ? WheelGesture::CtrlWheel : WheelGesture::Wheel;
        if (mouse.LookupWheel(withoutShift) == MouseCommand::Zoom) {
            command = MouseCommand::Zoom;
            fine = true;
        }
    }

    switch (command) {
        case MouseCommand::Zoom: {
            // Taken before the zoom changes: the anchor is a point of the
            // picture, and which point is under the pointer depends on the
            // zoom it was read at.
            const ZoomAnchor anchor = WheelZoomAnchor(client);
            view_.StepZoom(notches, fine);
            ApplyZoom(&anchor);
            return;
        }
        case MouseCommand::Opacity:
            view_.StepOpacity(notches);
            ApplyOpacity();
            return;
        default:
            return;
    }
}

bool ClipWindow::RunShortcut(WPARAM key) noexcept {
    if (settings_ == nullptr) {
        return false;
    }

    const ccl::app::Binding pressed{static_cast<UINT>(key), IsKeyDown(VK_CONTROL),
                                    IsKeyDown(VK_SHIFT), IsKeyDown(VK_MENU)};

    switch (settings_->shortcuts.Lookup(pressed)) {
        case ccl::app::Command::Undo: Undo(); return true;
        case ccl::app::Command::Redo: Redo(); return true;
        case ccl::app::Command::Save: SaveAs(); return true;
        case ccl::app::Command::Copy: CopyImage(); return true;
        case ccl::app::Command::Open: OpenFile(); return true;
        case ccl::app::Command::Paste: PasteImage(); return true;
        case ccl::app::Command::ToolView:
            SelectTool(ccl::tool::Tool::View);
            return true;
        case ccl::app::Command::ToolPen:
            SelectTool(ccl::tool::Tool::Pen);
            return true;
        case ccl::app::Command::ToolEraser:
            SelectTool(ccl::tool::Tool::Eraser);
            return true;
        case ccl::app::Command::ToolText:
            SelectTool(ccl::tool::Tool::Text);
            return true;
        case ccl::app::Command::ToolSelect:
            SelectTool(ccl::tool::Tool::Select);
            return true;
        case ccl::app::Command::Eyedropper:
            OnCommand(kMenuEyedropper);
            return true;
        case ccl::app::Command::ColorPicker:
            ChooseColorFromPicker();
            return true;
        case ccl::app::Command::Highlighter:
            // A mode of the brush, not a tool: switching it on selects the
            // brush as well, since that is plainly what was meant.
            tool_.highlighter = !tool_.highlighter;
            SelectTool(ccl::tool::Tool::Pen);
            return true;
        case ccl::app::Command::Antialias:
            tool_.antialias = !tool_.antialias;
            UpdateTitle();
            Draw();
            return true;
        case ccl::app::Command::FitToImage:
            FitToImage();
            return true;
        case ccl::app::Command::HideWindow:
            HideTemporarily();
            return true;
        default:
            return false;
    }
}

void ClipWindow::OnKeyDown(WPARAM key) noexcept {
    if (RunShortcut(key)) {
        return;
    }

    // Shift+digit picks a quick colour; the digits alone are zoom presets.
    if (IsKeyDown(VK_SHIFT) && key >= '1' && key <= '8') {
        tool_.UseColor(tool_.quickColors[key - '1']);
        return;
    }

    switch (key) {
        case VK_OEM_4:  // [
        case VK_OEM_6: {  // ]
            const int steps = key == VK_OEM_6 ? 1 : -1;

            // An effect that was just placed takes the size keys, so its
            // strength can be tuned while looking at it.
            if (adjustingEffectIndex_ != static_cast<size_t>(-1)) {
                StepEffectStrength(steps);
                return;
            }

            // While a straight line is being previewed, the size keys retarget
            // one of its ends -- the far end normally, the starting end with
            // Ctrl -- which is how a taper is dialled in without having to aim
            // the pen pressure.
            //
            // Each end steps from its own current width rather than from the
            // brush size, so adjusting one end leaves the other alone.
            if (drawing_ && straightLine_ && activeStroke_.points.size() >= 2) {
                // The stretch being aimed, not the line as a whole: past a
                // corner, its near end is the corner itself.
                const size_t last = activeStroke_.points.size() - 1;
                float& target = IsKeyDown(VK_CONTROL)
                                    ? activeStroke_.points[last - 1].width
                                    : activeStroke_.points[last].width;
                target = ccl::tool::ToolState::SteppedWidth(target, steps);
            } else {
                tool_.StepWidth(steps);
            }

            UpdateTitle();
            Draw();
            return;
        }

        case '1':
        case '2':
        case '3':
        case '4':
        case '5': {
            const ZoomAnchor anchor = KeyZoomAnchor();
            view_.SetZoom(static_cast<float>(key - '0'));
            ApplyZoom(&anchor);
            return;
        }

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

ClipWindow::ZoomAnchor ClipWindow::AnchorAt(POINT client) const noexcept {
    ZoomAnchor anchor;
    anchor.image = ToImage(client);
    anchor.screen = client;
    ::ClientToScreen(hwnd_, &anchor.screen);
    return anchor;
}

ClipWindow::ZoomAnchor ClipWindow::CornerZoomAnchor() const noexcept {
    // The corner of the visible area. Holding it means the picture grows away
    // from where it already is, and the window never moves.
    const int border = BorderWidth();
    return AnchorAt(POINT{border, border});
}

ClipWindow::ZoomAnchor ClipWindow::CenterZoomAnchor() const noexcept {
    const SIZE viewport = ViewportSize();
    const int border = BorderWidth();
    return AnchorAt(POINT{border + static_cast<int>(viewport.cx / 2),
                          border + static_cast<int>(viewport.cy / 2)});
}

// The anchor a zoom that came from the keyboard or the menu uses. There is no
// pointer behind those, so the cursor setting has to mean something else for
// them; the middle of the view is the nearest thing to "where I am looking".
ClipWindow::ZoomAnchor ClipWindow::KeyZoomAnchor() const noexcept {
    if (settings_ != nullptr &&
        settings_->zoomAnchor == ccl::app::ZoomAnchor::TopLeft) {
        return CornerZoomAnchor();
    }
    return CenterZoomAnchor();
}

ClipWindow::ZoomAnchor ClipWindow::WheelZoomAnchor(POINT client) noexcept {
    if (settings_ != nullptr &&
        settings_->zoomAnchor != ccl::app::ZoomAnchor::Cursor) {
        // Not following the pointer, so there is no gesture to hold on to.
        zoomAnchorValid_ = false;
        return settings_->zoomAnchor == ccl::app::ZoomAnchor::TopLeft
                   ? CornerZoomAnchor()
                   : CenterZoomAnchor();
    }

    const ULONGLONG now = ::GetTickCount64();
    if (!zoomAnchorValid_ || now - lastZoomTick_ > kZoomGestureGapMs) {
        // The wheel reaches the window that has the focus, which is not always
        // the window the pointer is over. A point outside the window is a
        // point outside the picture, and zooming around one sends the window
        // off after it, so the pointer is pulled back onto the picture first.
        RECT area{};
        ::GetClientRect(hwnd_, &area);
        const POINT inside{
            std::clamp(client.x, area.left, std::max(area.left, area.right - 1)),
            std::clamp(client.y, area.top, std::max(area.top, area.bottom - 1))};

        zoomAnchor_ = AnchorAt(inside);
        zoomAnchorValid_ = true;
    }
    lastZoomTick_ = now;
    return zoomAnchor_;
}

void ClipWindow::ApplyZoom(const ZoomAnchor* anchor) noexcept {
    const SIZE content = ContentSize();

    // The window itself never grows past the monitor: beyond that the image is
    // scrolled instead. Only the size is bounded this way -- where the window
    // sits is settled by the anchor below, which is allowed to push it off the
    // edge rather than give up holding the point still.
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

    // What WindowSizeFor is given is the area the picture gets, which is also
    // what the viewport will be once the window has been resized.
    const SIZE viewport{std::min(content.cx, maxWidth),
                        std::min(content.cy, maxHeight)};
    const SIZE outer = WindowSizeFor(viewport);

    RECT bounds{};
    ::GetWindowRect(hwnd_, &bounds);

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE;
    int x = bounds.left;
    int y = bounds.top;

    if (anchor != nullptr) {
        const float zoom = view_.Zoom();
        const auto border = static_cast<float>(BorderWidth());

        // With a title bar the client area does not start at the corner of the
        // window, and the picture is placed against the client area. The gap
        // is the same before and after the resize, so it is measured once and
        // taken back off when the window position is worked out.
        POINT clientOrigin{0, 0};
        ::ClientToScreen(hwnd_, &clientOrigin);
        const int frameX = clientOrigin.x - bounds.left;
        const int frameY = clientOrigin.y - bounds.top;

        // A point of the picture sits at, in screen coordinates:
        //   client origin + border + image * zoom - scroll
        // Scrolling is tried first, so the amount asked of it is whatever
        // would hold the point still with the window left where it is.
        const float wantX = static_cast<float>(clientOrigin.x) + border +
                            anchor->image.x * zoom -
                            static_cast<float>(anchor->screen.x);
        const float wantY = static_cast<float>(clientOrigin.y) + border +
                            anchor->image.y * zoom -
                            static_cast<float>(anchor->screen.y);

        view_.SetScroll(POINT{std::lround(wantX), std::lround(wantY)}, content,
                        viewport);

        // Whatever the scroll could not take -- because the picture is already
        // against its edge, or is smaller than the window and cannot slide at
        // all -- the window position takes instead.
        const POINT scroll = view_.Scroll();
        x = static_cast<int>(std::lround(static_cast<float>(anchor->screen.x) -
                                         border - anchor->image.x * zoom +
                                         static_cast<float>(scroll.x))) -
            frameX;
        y = static_cast<int>(std::lround(static_cast<float>(anchor->screen.y) -
                                         border - anchor->image.y * zoom +
                                         static_cast<float>(scroll.y))) -
            frameY;
        // Without this the system carries the old pixels across to the new
        // position before anything is repainted, which shows the previous
        // frame slid sideways for an instant. Every pixel is about to be
        // drawn again at a different scale, so there is nothing worth
        // carrying.
        // Hanging off the edge is allowed -- holding the point still is worth
        // more than staying tidy -- but going off it altogether is not. The
        // anchored pixel is the one thing that must remain reachable, and the
        // pointer it sits under is on a monitor by definition, so the window
        // is kept far enough over to still cover it.
        const int anchorX = static_cast<int>(anchor->screen.x);
        const int anchorY = static_cast<int>(anchor->screen.y);
        x = std::clamp(x, anchorX - static_cast<int>(outer.cx) + 1, anchorX);
        y = std::clamp(y, anchorY - static_cast<int>(outer.cy) + 1, anchorY);

        flags &= ~static_cast<UINT>(SWP_NOMOVE);
        flags |= SWP_NOCOPYBITS;
    }

    // One repaint per change, at the end, against the finished state: the
    // resize below sends a WM_SIZE, which would otherwise paint a half-applied
    // frame first.
    applyingZoom_ = true;
    ::SetWindowPos(hwnd_, nullptr, x, y, outer.cx, outer.cy, flags);
    applyingZoom_ = false;

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
            if (drawing_ && straightLine_ && activeStroke_.points.size() >= 2) {
                const size_t last = activeStroke_.points.size() - 1;
                ::swprintf_s(title, L"%s  %d%%  Line %.0f → %.0fpx", name.c_str(),
                             zoom, activeStroke_.points[last - 1].width,
                             activeStroke_.points[last].width);
                break;
            }
            ::swprintf_s(title, L"%s  %d%%  %s %.0fpx%s", name.c_str(), zoom,
                         tool_.highlighter ? L"Marker" : L"Pen", tool_.Width(),
                         tool_.antialias ? L"" : L" (aliased)");
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
        case ccl::tool::Tool::Select:
            // While an effect is adjustable its strength is what the size keys
            // act on, so that is what gets reported.
            if (document_ != nullptr &&
                adjustingEffectIndex_ < document_->Annotations().size()) {
                const auto& annotation =
                    document_->Annotations()[adjustingEffectIndex_];
                const wchar_t* effectName =
                    annotation.effect.kind == ccl::doc::EffectKind::Mosaic
                        ? L"モザイク"
                        : L"ぼかし";
                if (annotation.effect.strength <= 0.0f) {
                    ::swprintf_s(title, L"%s  %d%%  %s なし  ([ ] で調整)",
                                 name.c_str(), zoom, effectName);
                } else {
                    ::swprintf_s(title, L"%s  %d%%  %s %.0f  ([ ] で調整)",
                                 name.c_str(), zoom, effectName,
                                 annotation.effect.strength);
                }
            } else if (HasSelection()) {
                const D2D1_RECT_F area = SelectionRect();
                ::swprintf_s(title, L"%s  %d%%  Select %.0f x %.0f",
                             name.c_str(), zoom, area.right - area.left,
                             area.bottom - area.top);
            } else {
                ::swprintf_s(title, L"%s  %d%%  Select", name.c_str(), zoom);
            }
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
        hwnd_, screen, original, tool_.quickColors, tool_.RecentColors(),
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
    // Reaching for the eyedropper is not leaving what you were doing: it reads
    // a colour and hands the tool straight back. Whatever was set up before it
    // -- an area selected, an effect being tuned -- is still meant afterwards.
    // Both directions count, since the way back is another call through here.
    const bool eyedropperAside = tool == ccl::tool::Tool::Eyedropper ||
                                 tool_.tool == ccl::tool::Tool::Eyedropper;

    if (!eyedropperAside) {
        adjustingEffectIndex_ = static_cast<size_t>(-1);
    }

    // A selection belongs to the tool that draws it. Left behind, it would sit
    // there through a session of drawing and then act on whatever the next
    // command was, long after there was any reason to expect it.
    if (tool != ccl::tool::Tool::Select && !eyedropperAside) {
        hasSelection_ = false;
    }

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
    if (tool == ccl::tool::Tool::Eyedropper) {
        InstallEyedropperHook();
    }
    SetColorPreviewActive(tool == ccl::tool::Tool::Eyedropper);
    UpdateCursor();
    UpdateTitle();
    Draw();
}

void ClipWindow::EndEyedropper() noexcept {
    if (tool_.tool != ccl::tool::Tool::Eyedropper) {
        return;
    }
    sampling_ = false;
    RemoveEyedropperHook();
    SelectTool(toolBeforeEyedropper_);
}

bool ClipWindow::InstallEyedropperHook() noexcept {
    if (eyedropperHook_ != nullptr) {
        return true;
    }
    g_eyedropperWindow = this;
    eyedropperHook_ = ::SetWindowsHookExW(WH_MOUSE_LL, &EyedropperHookProc,
                                          ::GetModuleHandleW(nullptr), 0);
    if (eyedropperHook_ == nullptr) {
        g_eyedropperWindow = nullptr;
        return false;
    }
    return true;
}

void ClipWindow::RemoveEyedropperHook() noexcept {
    if (eyedropperHook_ != nullptr) {
        ::UnhookWindowsHookEx(eyedropperHook_);
        eyedropperHook_ = nullptr;
    }
    if (g_eyedropperWindow == this) {
        g_eyedropperWindow = nullptr;
    }
}

LRESULT CALLBACK ClipWindow::EyedropperHookProc(int code, WPARAM wParam,
                                                LPARAM lParam) {
    ClipWindow* window = g_eyedropperWindow;
    if (code != HC_ACTION || window == nullptr || window->hwnd_ == nullptr) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    // Sent as two whole values rather than packed into one: a point on a
    // monitor left of or above the primary one has negative coordinates.
    const auto x = static_cast<WPARAM>(mouse->pt.x);
    const auto y = static_cast<LPARAM>(mouse->pt.y);

    switch (wParam) {
        case WM_MOUSEMOVE:
            // Posted rather than handled here: a low-level hook runs on every
            // mouse event in the system and is dropped if it dawdles. Only one
            // is in flight at a time, so a fast mouse cannot flood the queue.
            if (!window->previewPending_) {
                window->previewPending_ = true;
                ::PostMessageW(window->hwnd_, kEyedropperMoveMessage, x, y);
            }
            return ::CallNextHookEx(nullptr, code, wParam, lParam);

        case WM_LBUTTONDOWN:
            // The start of taking a colour rather than the whole of it. What
            // settles it is letting go, which is what allows the sample to be
            // aimed by dragging.
            ::PostMessageW(window->hwnd_, kEyedropperPressMessage, x, y);
            return 1;  // swallowed, so the window underneath never sees it

        case WM_LBUTTONUP:
            ::PostMessageW(window->hwnd_, kEyedropperPickMessage, x, y);
            return 1;

        case WM_RBUTTONDOWN:
            ::PostMessageW(window->hwnd_, kEyedropperCancelMessage, 0, 0);
            return 1;

        // The presses above were taken, so their releases have to go too, or
        // the window underneath gets a button-up it never saw the down for.
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 1;

        default:
            break;
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void ClipWindow::UpdateColorPreview() noexcept {
    POINT cursor{};
    if (::GetCursorPos(&cursor)) {
        colorPreview_.Update(cursor);
    }
}

void ClipWindow::SetColorPreviewActive(bool active) noexcept {
    if (!active) {
        if (previewTimer_ != 0) {
            ::KillTimer(hwnd_, previewTimer_);
            previewTimer_ = 0;
        }
        colorPreview_.Hide();
        return;
    }

    UpdateColorPreview();
    if (previewTimer_ == 0) {
        // Polled rather than driven by mouse messages: until the button goes
        // down there is no capture, so moving the cursor outside this window
        // sends nothing here, and outside is exactly where the eyedropper is
        // most useful.
        previewTimer_ = ::SetTimer(hwnd_, kColorPreviewTimer, 16, nullptr);
    }
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
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN |
            TPM_NOANIMATION,
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

    // Keys come from the bindings rather than being written into the labels, so
    // that the menu still tells the truth after they have been reassigned.
    const ccl::app::Shortcuts& keys = settings_->shortcuts;
    const auto withKey = [&keys](const wchar_t* label,
                                 ccl::app::Command command) {
        return label + keys.MenuSuffix(command);
    };

    // A submenu shows no keys until it is opened, so the ones inside it are
    // gathered onto the parent. Without this the shortcuts that matter most --
    // the tools -- were the ones hardest to discover.
    const auto keyList = [&keys](std::initializer_list<ccl::app::Command> list) {
        std::wstring text;
        for (ccl::app::Command command : list) {
            const std::wstring key = ccl::app::BindingText(keys.For(command));
            if (key.empty()) {
                continue;
            }
            if (!text.empty()) {
                text += L" ";
            }
            text += key;
        }
        return text.empty() ? std::wstring{} : L"\t" + text;
    };

    // Ordered by what the item acts on, nearest first: the last edit, then the
    // drawing about to be done, then the picture, then getting the picture in
    // and out, and finally the program itself.
    ::AppendMenuW(menu, plain | (history_.CanUndo() ? 0u : MF_GRAYED), kMenuUndo,
                  withKey(L"元に戻す", ccl::app::Command::Undo).c_str());
    ::AppendMenuW(menu, plain | (history_.CanRedo() ? 0u : MF_GRAYED), kMenuRedo,
                  withKey(L"やり直し", ccl::app::Command::Redo).c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const HMENU tools = ::CreatePopupMenu();
    const auto toolEntry = [&](ccl::tool::Tool tool, const wchar_t* label,
                               ccl::app::Command command) {
        ::AppendMenuW(tools, tool_.tool == tool ? checked : plain,
                      kMenuToolBase + static_cast<UINT>(tool),
                      withKey(label, command).c_str());
    };
    toolEntry(ccl::tool::Tool::View, L"ビュー", ccl::app::Command::ToolView);
    toolEntry(ccl::tool::Tool::Pen, L"ペン", ccl::app::Command::ToolPen);
    toolEntry(ccl::tool::Tool::Eraser, L"消しゴム",
              ccl::app::Command::ToolEraser);
    toolEntry(ccl::tool::Tool::Text, L"テキスト", ccl::app::Command::ToolText);
    toolEntry(ccl::tool::Tool::Select, L"範囲選択",
              ccl::app::Command::ToolSelect);
    ::AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(tools, tool_.tool == ccl::tool::Tool::Eyedropper ? checked
                                                                  : plain,
                  kMenuEyedropper,
                  withKey(L"スポイト", ccl::app::Command::Eyedropper).c_str());
    ::AppendMenuW(
        menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools),
        (L"ツール" + keyList({ccl::app::Command::ToolView,
                              ccl::app::Command::ToolPen,
                              ccl::app::Command::ToolEraser,
                              ccl::app::Command::ToolText,
                              ccl::app::Command::ToolSelect,
                              ccl::app::Command::Eyedropper}))
            .c_str());

    ::AppendMenuW(
        menu, plain, kMenuColorPicker,
        withKey(L"色を選ぶ...", ccl::app::Command::ColorPicker).c_str());

    const HMENU widths = ::CreatePopupMenu();
    for (size_t i = 0; i < ARRAYSIZE(kWidthPresets); ++i) {
        wchar_t label[32];
        ::swprintf_s(label, L"%.0f px", kWidthPresets[i]);
        ::AppendMenuW(widths,
                      tool_.Width() == kWidthPresets[i] ? checked : plain,
                      kMenuWidthBase + static_cast<UINT>(i), label);
    }
    ::AppendMenuW(widths, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(
        widths, tool_.highlighter ? checked : plain, kMenuHighlighter,
        withKey(L"蛍光マーカー", ccl::app::Command::Highlighter).c_str());
    ::AppendMenuW(widths, tool_.antialias ? checked : plain, kMenuAntialias,
                  withKey(L"なめらかにする", ccl::app::Command::Antialias).c_str());
    ::AppendMenuW(widths, tool_.usePressure ? checked : plain, kMenuPressure,
                  L"筆圧を使う");
    // The size keys are not in the bindings: while a straight line is being
    // drawn they retarget one of its ends instead, so they are not a command
    // that could be pointed at something else.
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widths),
                  (L"線\t[ ]" +
                   [&] {
                       const std::wstring rest =
                           keyList({ccl::app::Command::Highlighter,
                                    ccl::app::Command::Antialias});
                       return rest.empty() ? rest : L" " + rest.substr(1);
                   }())
                      .c_str());

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
                  L"文字\tCtrl+B I U");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Only offered when there is something selected to obscure.
    const HMENU selection = ::CreatePopupMenu();
    const UINT selectionState = HasSelection() ? plain : (plain | MF_GRAYED);
    ::AppendMenuW(selection, selectionState, kMenuMosaic, L"モザイク");
    ::AppendMenuW(selection, selectionState, kMenuBlur, L"ぼかし");
    ::AppendMenuW(selection, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(selection, selectionState, kMenuCrop,
                  L"この範囲で切り抜く");
    ::AppendMenuW(selection, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(selection, selectionState, kMenuClearSelection,
                  L"選択を解除");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(selection),
                  L"選択範囲");

    // Reshaping the picture. Every one of these burns the annotations in, so
    // they are kept together and away from the tools.
    const HMENU image = ::CreatePopupMenu();
    ::AppendMenuW(image, plain, kMenuRotateLeft, L"左に 90 度回転");
    ::AppendMenuW(image, plain, kMenuRotateRight, L"右に 90 度回転");
    ::AppendMenuW(image, plain, kMenuRotate180, L"180 度回転");
    ::AppendMenuW(image, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(image, plain, kMenuFlipHorizontal, L"左右反転");
    ::AppendMenuW(image, plain, kMenuFlipVertical, L"上下反転");
    ::AppendMenuW(image, MF_SEPARATOR, 0, nullptr);
    const UINT pasteState =
        ccl::io::ClipboardHasImage() ? plain : (plain | MF_GRAYED);
    ::AppendMenuW(image, pasteState, kMenuConcatRight,
                  L"クリップボードの画像を右に連結");
    ::AppendMenuW(image, pasteState, kMenuConcatBottom,
                  L"クリップボードの画像を下に連結");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image), L"画像");

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
    ::AppendMenuW(
        zoom, plain, kMenuFit,
        withKey(L"画像サイズに合わせる", ccl::app::Command::FitToImage).c_str());
    ::AppendMenuW(
        menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom),
        (L"表示\t1-5" + [&] {
            const std::wstring rest = keyList({ccl::app::Command::FitToImage});
            return rest.empty() ? rest : L" " + rest.substr(1);
        }()).c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Sending the capture somewhere.
    ::AppendMenuW(menu, plain, kMenuSave,
                  withKey(L"保存...", ccl::app::Command::Save).c_str());
    ::AppendMenuW(
        menu, plain, kMenuCopy,
        withKey(L"クリップボードにコピー", ccl::app::Command::Copy).c_str());
    // Sits with the other ways of getting the capture out rather than with the
    // transforms: it produces the picture as it is being shown, which is what
    // saving and copying do too.
    ::AppendMenuW(menu, plain, kMenuCaptureSelf,
                  L"現在の状態でキャプチャ(更新)する");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Replacing it with a different one. Recapturing belongs here rather than
    // beside the program commands: it is another way of getting a picture, not
    // a way of leaving.
    ::AppendMenuW(menu, plain, kMenuOpen,
                  withKey(L"開く...", ccl::app::Command::Open).c_str());
    ::AppendMenuW(menu,
                  ccl::io::ClipboardHasImage() ? plain : (plain | MF_GRAYED),
                  kMenuPaste,
                  withKey(L"貼り付け", ccl::app::Command::Paste).c_str());
    ::AppendMenuW(menu, plain, kMenuRecapture, L"撮り直す");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(menu, plain, kMenuSettings, L"設定...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, plain, kMenuExit, L"終了");

    // TPM_RETURNCMD hands the choice back directly, which avoids routing it
    // through WM_COMMAND for a menu that only exists for the duration of the
    // call. TPM_NOANIMATION drops the fade: it fits a tool built around
    // responsiveness, and it is what left a ghost of the menu on screen when
    // recapturing, since the fade outlives the process that started it.
    const int command = ::TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN |
            TPM_NOANIMATION,
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
        const ZoomAnchor anchor = KeyZoomAnchor();
        view_.SetZoom(static_cast<float>(id - kMenuZoomBase) / 100.0f);
        ApplyZoom(&anchor);
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
        case kMenuOpen:
            OpenFile();
            return;
        case kMenuPaste:
            PasteImage();
            return;
        case kMenuUndo:
            Undo();
            return;
        case kMenuRedo:
            Redo();
            return;
        case kMenuCrop:
            CropToSelection();
            return;
        case kMenuRotateLeft:
            ApplyTransform(ccl::io::RotateLeft(FlattenForTransform()));
            return;
        case kMenuRotateRight:
            ApplyTransform(ccl::io::RotateRight(FlattenForTransform()));
            return;
        case kMenuRotate180:
            ApplyTransform(ccl::io::Rotate180(FlattenForTransform()));
            return;
        case kMenuFlipHorizontal:
            ApplyTransform(ccl::io::FlipHorizontal(FlattenForTransform()));
            return;
        case kMenuFlipVertical:
            ApplyTransform(ccl::io::FlipVertical(FlattenForTransform()));
            return;
        case kMenuConcatRight:
            ConcatenateClipboard(true);
            return;
        case kMenuConcatBottom:
            ConcatenateClipboard(false);
            return;
        case kMenuCaptureSelf:
            CaptureSelf();
            return;
        case kMenuRecapture:
            Recapture();
            return;
        case kMenuSettings:
            OpenSettings();
            return;
        case kMenuFit:
            FitToImage();
            return;
        case kMenuAntialias:
            tool_.antialias = !tool_.antialias;
            UpdateTitle();
            Draw();
            return;
        case kMenuHighlighter:
            tool_.highlighter = !tool_.highlighter;
            SelectTool(ccl::tool::Tool::Pen);
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
        case kMenuMosaic:
            ApplyEffectToSelection(ccl::doc::EffectKind::Mosaic);
            return;
        case kMenuBlur:
            ApplyEffectToSelection(ccl::doc::EffectKind::Blur);
            return;
        case kMenuClearSelection:
            hasSelection_ = false;
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

    // Text still in the editor is not part of the document yet, so it would be
    // missing from the file. The same applies to copying and auto-saving.
    if (EditingText()) {
        CommitText();
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

    const ccl::capture::DibBuffer flat = renderer_.Flatten();
    if (ccl::io::SaveImage(*context_,
                           flat.IsValid() ? flat : document_->Image(), path,
                           format, settings_->jpegQuality)) {
        saved_ = true;
    } else {
        ::MessageBoxW(hwnd_, L"Failed to save the image.", L"CapturaClipA2",
                      MB_ICONERROR | MB_OK);
    }
}

void ClipWindow::ResizeToImage() noexcept {
    if (document_ == nullptr) {
        return;
    }

    const SIZE content = ContentSize();

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

    const SIZE outer = WindowSizeFor(SIZE{std::min(content.cx, maxWidth),
                                          std::min(content.cy, maxHeight)});
    ::SetWindowPos(hwnd_, nullptr, 0, 0, outer.cx, outer.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ClipWindow::ReplaceImage(ccl::capture::DibBuffer image,
                              const std::wstring& title) noexcept {
    if (!image.IsValid() || document_ == nullptr) {
        return;
    }

    // The capture on screen is written out first if auto-saving is on, since
    // replacing it discards it.
    AutoSaveBeforeClosing();

    *document_ = ccl::doc::Document(std::move(image));

    // Everything tied to the old picture goes with it: its annotations are
    // gone, so its history would restore edits onto a different image.
    history_ = ccl::doc::History{};
    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hasSelection_ = false;
    saved_ = false;
    sourceTitle_ = title;
    // The anchor names a point of the old picture, which the new one has no
    // reason to share.
    zoomAnchorValid_ = false;

    view_.SetZoom(1.0f);
    view_.SetScroll(POINT{0, 0}, ContentSize(), ViewportSize());

    renderer_.SetDocument(document_);
    ResizeToImage();
    UpdateTitle();
    Draw();
}

void ClipWindow::Undo() noexcept {
    if (document_ == nullptr) {
        return;
    }
    // Asked before the step is consumed: afterwards there is no way to tell
    // whether the picture was among what came back.
    const bool reshaped = history_.NextUndoChangesImage();
    if (!history_.Undo(document_->Annotations(), document_->Image())) {
        return;
    }

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    if (reshaped) {
        hasSelection_ = false;
        renderer_.SetDocument(document_);
        ResizeToImage();
        ClampScroll();
        UpdateTitle();
    }
    Draw();
}

void ClipWindow::Redo() noexcept {
    if (document_ == nullptr) {
        return;
    }
    const bool reshaped = history_.NextRedoChangesImage();
    if (!history_.Redo(document_->Annotations(), document_->Image())) {
        return;
    }

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    if (reshaped) {
        hasSelection_ = false;
        renderer_.SetDocument(document_);
        ResizeToImage();
        ClampScroll();
        UpdateTitle();
    }
    Draw();
}

ccl::capture::DibBuffer ClipWindow::FlattenForTransform() noexcept {
    if (EditingText()) {
        CommitText();
    }
    return renderer_.Flatten();
}

void ClipWindow::ApplyTransform(ccl::capture::DibBuffer transformed) noexcept {
    if (!transformed.IsValid() || document_ == nullptr) {
        return;
    }

    history_.RecordWithImage(document_->Annotations(), document_->Image());

    document_->Image() = std::move(transformed);
    document_->Annotations().clear();

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    hasSelection_ = false;
    saved_ = false;

    renderer_.SetDocument(document_);
    ResizeToImage();
    ClampScroll();
    UpdateTitle();
    Draw();
}

void ClipWindow::CropToSelection() noexcept {
    if (!HasSelection()) {
        return;
    }

    const D2D1_RECT_F area = SelectionRect();
    const ccl::capture::DibBuffer flat = FlattenForTransform();
    if (!flat.IsValid()) {
        return;
    }

    const RECT bounds{static_cast<LONG>(std::lround(area.left)),
                      static_cast<LONG>(std::lround(area.top)),
                      static_cast<LONG>(std::lround(area.right)),
                      static_cast<LONG>(std::lround(area.bottom))};
    ApplyTransform(flat.Crop(bounds));
}

void ClipWindow::OpenSettings() noexcept {
    if (settings_ == nullptr) {
        return;
    }
    if (EditingText()) {
        CommitText();
    }

    // The window takes focus while it is up, which the editor would otherwise
    // read as clicking away.
    ++suppressCommitDepth_;
    const bool changed = ccl::ui::ShowSettingsDialog(hwnd_, *settings_);
    --suppressCommitDepth_;

    if (!changed) {
        return;
    }

    // Applied where it costs nothing to do so. What is left -- the capture
    // settings, and the text defaults, which only apply to text placed from
    // here on -- takes effect the next time it is used.
    renderer_.SetSmoothScaling(settings_->smoothScaling);
    view_.SetZoomStepPercent(settings_->zoomStepPercent);
    tool_.usePressure = settings_->usePenPressure;
    tool_.quickColors = settings_->quickColors;

    UpdateTitle();
    Draw();
}

void ClipWindow::CaptureSelf() noexcept {
    if (document_ == nullptr) {
        return;
    }
    if (EditingText()) {
        CommitText();
    }

    // The viewport, not the whole client area: the border around it belongs to
    // the window rather than to the picture.
    const SIZE viewport = ViewportSize();
    if (viewport.cx <= 0 || viewport.cy <= 0) {
        return;
    }

    ccl::capture::DibBuffer view = renderer_.CaptureView(
        static_cast<UINT>(viewport.cx), static_cast<UINT>(viewport.cy), view_);
    if (!view.IsValid()) {
        return;
    }

    // The new picture already has the zoom baked into it, so the view goes back
    // to showing it one to one.
    view_.SetZoom(1.0f);
    view_.SetScroll(POINT{0, 0}, ContentSize(), ViewportSize());
    ApplyTransform(std::move(view));
}

void ClipWindow::Recapture() noexcept {
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) {
        return;
    }

    // The copy is told to wait for this process to exit before it reads the
    // screen. Hiding the window here is not enough on its own: the menu the
    // command came from fades out afterwards, and that fade is drawn by this
    // process, so it is only gone once the process is.
    ::ShowWindow(hwnd_, SW_HIDE);

    wchar_t arguments[64];
    // Long enough for the desktop to finish repainting what this window was
    // covering, on top of waiting for the process itself to go.
    ::swprintf_s(arguments, L"--wait=%lu --prepare=250",
                 ::GetCurrentProcessId());

    // A second copy of the program, which starts at the area selection the way
    // it always does. One window per process, so this one simply goes away.
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", path, arguments, nullptr,
                        SW_SHOWNORMAL));
    if (result <= 32) {
        ::ShowWindow(hwnd_, SW_SHOW);
        return;
    }

    // Starting over means throwing this capture away, so it is not written out
    // on the way past the way an ordinary close would.
    discarding_ = true;
    ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void ClipWindow::ConcatenateClipboard(bool toRight) noexcept {
    ccl::capture::DibBuffer addition = ccl::io::PasteFromClipboard(hwnd_);
    if (!addition.IsValid()) {
        return;
    }

    const ccl::capture::DibBuffer flat = FlattenForTransform();
    if (!flat.IsValid()) {
        return;
    }

    // Black padding for the shorter side. Screenshots of dark interfaces are
    // the common case, and black is what the original filled with.
    constexpr ccl::doc::Color fill{0.0f, 0.0f, 0.0f, 1.0f};

    ApplyTransform(ccl::io::Concatenate(
        flat, addition,
        toRight ? ccl::io::ConcatSide::Right : ccl::io::ConcatSide::Bottom,
        fill));
}

void ClipWindow::OpenFile() noexcept {
    if (context_ == nullptr) {
        return;
    }

    wchar_t path[MAX_PATH]{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = ccl::io::kOpenFilter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    ++suppressCommitDepth_;
    const BOOL chosen = ::GetOpenFileNameW(&dialog);
    --suppressCommitDepth_;

    if (!chosen) {
        return;
    }

    ccl::capture::DibBuffer image = ccl::io::LoadImageFile(*context_, path);
    if (!image.IsValid()) {
        ::MessageBoxW(hwnd_, L"画像を読み込めませんでした。", L"CapturaClipA2",
                      MB_ICONERROR | MB_OK);
        return;
    }

    // Named after the file, which is more useful than the window it came from.
    const wchar_t* name = ::wcsrchr(path, L'\\');
    ReplaceImage(std::move(image), name != nullptr ? name + 1 : path);
}

void ClipWindow::PasteImage() noexcept {
    ccl::capture::DibBuffer image = ccl::io::PasteFromClipboard(hwnd_);
    if (!image.IsValid()) {
        return;
    }
    ReplaceImage(std::move(image), L"");
}

void ClipWindow::CopyImage() noexcept {
    if (document_ == nullptr) {
        return;
    }
    if (EditingText()) {
        CommitText();
    }
    const ccl::capture::DibBuffer flat = renderer_.Flatten();
    ccl::io::CopyToClipboard(hwnd_, flat.IsValid() ? flat : document_->Image());
}

void ClipWindow::AutoSaveBeforeClosing() noexcept {
    if (saved_ || discarding_ || context_ == nullptr || document_ == nullptr ||
        settings_ == nullptr) {
        return;
    }
    // Holding Shift while closing skips the automatic save.
    if (IsKeyDown(VK_SHIFT)) {
        return;
    }

    if (EditingText()) {
        CommitText();
    }

    const ccl::capture::DibBuffer flat = renderer_.Flatten();
    ccl::io::AutoSaveImage(*context_,
                           flat.IsValid() ? flat : document_->Image(),
                           *settings_, sourceTitle_);
}

void ClipWindow::Draw() noexcept {
    const LONGLONG frameStart = ccl::timing::Mark();

    ccl::render::BrushCursor cursor{};
    const bool showCursor = ShowsBrushCursor();
    if (showCursor) {
        cursor.position = ToImage(lastCursor_);
        cursor.radius = tool_.Width() * 0.5f;
        cursor.antialias = tool_.antialias;
    }

    D2D1_RECT_F highlight{};
    bool hasHighlight = false;

    // The selection uses the same outline as the "what would this click act
    // on" marker, which is the same thing it means here.
    if (tool_.tool == ccl::tool::Tool::Select && HasSelection()) {
        highlight = SelectionRect();
        hasHighlight = true;
    }

    if (!hasHighlight && tool_.tool == ccl::tool::Tool::Text &&
        editor_ == nullptr &&
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
        ccl::timing::AddSince(drawStats_, frameStart);
    }
}

bool ClipWindow::Create(ccl::render::D2DContext& context,
                        ccl::doc::Document& document,
                        ccl::app::Settings& settings, POINT position,
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
    renderer_.SetBorderWidth(BorderWidth());
    tool_.usePressure = settings.usePenPressure;
    tool_.textShadow = settings.textShadow;
    tool_.textOutline = settings.textOutline;
    tool_.SeedDefaults(settings.penColor, settings.penWidth,
                       settings.eraserWidth, settings.quickColors);
    ccl::timing::Stopwatch watch;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ClipWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClipWindowClass;
    // Carried into the taskbar button and Alt+Tab. Loaded at both sizes so
    // Windows does not scale the large one down for the small slot, which on
    // pixel art turns to mush.
    wc.hIcon = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                     ::GetSystemMetrics(SM_CXICON),
                     ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                     ::GetSystemMetrics(SM_CXSMICON),
                     ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_EX_LAYERED is what makes the window translucent; at full opacity it
    // costs nothing visible.
    //
    // WS_THICKFRAME makes the window sizable. Without a caption WM_NCCALCSIZE
    // hides the frame it would otherwise draw and WM_NCHITTEST supplies the
    // grips, so the window is the image plus a one pixel outline; with one the
    // frame is left alone and Windows draws it as usual.
    //
    // WS_CLIPCHILDREN keeps this window's own drawing out of the area the text
    // editor occupies. Without it the editor had to be told to repaint after
    // every frame, which is what made it flicker.
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN;
    // WS_EX_APPWINDOW forces a taskbar button even with no title bar, so it is
    // obvious whether a capture is still alive.
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_APPWINDOW;

    switch (settings.windowFrame) {
        case ccl::app::WindowFrame::Normal:
            style |= WS_CAPTION | WS_SYSMENU;
            break;
        case ccl::app::WindowFrame::ThinTitleBar:
            // A tool window's caption is the narrow one, and it comes with no
            // taskbar button, so WS_EX_APPWINDOW has to go with it.
            style |= WS_CAPTION | WS_SYSMENU;
            exStyle = (exStyle & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW;
            break;
        case ccl::app::WindowFrame::NoTitleBar:
        case ccl::app::WindowFrame::NoFrame:
        default:
            break;
    }

    // The window is the image plus whatever frame it has, placed so that the
    // image itself lands exactly where the selection was.
    const int inset = HasWindowBorder() ? ccl::render::kWindowBorder : 0;
    RECT wanted{position.x, position.y, position.x + document.Width(),
                position.y + document.Height()};
    if (HasTitleBar()) {
        ::AdjustWindowRectEx(&wanted, style, FALSE, exStyle);
    } else {
        ::InflateRect(&wanted, inset, inset);
    }

    hwnd_ = ::CreateWindowExW(exStyle, kClipWindowClass, L"CapturaClipA2", style,
                              wanted.left, wanted.top, wanted.right - wanted.left,
                              wanted.bottom - wanted.top, nullptr, nullptr,
                              instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    watch.Lap(L"  clip window create");

    // Fully transparent until the first frame is on it. Otherwise the window
    // is briefly visible as an undrawn white rectangle between being shown and
    // being painted.
    ::SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    // Dropping a picture on the window opens it, the same as the open command.
    ::DragAcceptFiles(hwnd_, TRUE);
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

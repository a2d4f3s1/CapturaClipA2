#include "ui/ClipWindow.h"

#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <richedit.h>
#include <richole.h>
#include <shellapi.h>
#include <tom.h>
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
#include "ui/ConcatDialog.h"
#include "ui/DecorPanel.h"
#include "ui/FontPicker.h"
#include "ui/RotateDialog.h"
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

// The Text Object Model works in points where the messages work in twips.
float TwipsToPoints(LONG twips) noexcept {
    return static_cast<float>(twips) / 20.0f;
}

float TwipsToPixels(LONG twips, UINT dpi) noexcept {
    return static_cast<float>(twips) * static_cast<float>(dpi) / 1440.0f;
}

// Font size steps, in image pixels.
constexpr float kMinFontSize = 6.0f;
constexpr float kMaxFontSize = 400.0f;

// One press of a size key. Proportional, so the same key feels the same at any
// size, with half a pixel added so that the smallest sizes still move.
float SteppedFontSize(float size, int steps) noexcept {
    for (int i = 0; i < steps; ++i) {
        size = std::min(kMaxFontSize, size * 1.15f + 0.5f);
    }
    for (int i = 0; i > steps; --i) {
        size = std::max(kMinFontSize, (size - 0.5f) / 1.15f);
    }
    return size;
}

// Where a lasso stops gathering points and starts spacing them further apart
// instead. Measured, not guessed: folding and outlining both grow with the
// count, and at 1024 points an outline costs 0.42ms a frame against 9.1ms at
// 65536 -- which the responsiveness this program is built around cannot pay.
constexpr size_t kMaxLassoPoints = 4096;

// How far apart a lasso's points start out, in client pixels.
constexpr float kLassoSpacing = 2.0f;
// How far past a piece a press still takes hold of it, in screen pixels. A pen
// line three pixels wide is not something a hand can land on exactly, and the
// frame drawn round a picked piece is no help -- it is the ink that is being
// reached for, not the box.
// Fallback for the reach past a piece that a press still takes hold of. The
// figure that counts is the one in the settings; this stands in only for the
// moment before they have been read.
constexpr float kGrabSlackFallback = 3.0f;

// Menu entries per column before starting a new one, so a long font list stays
// on screen instead of running off the bottom.
constexpr int kMenuColumnLength = 30;

// Installed font families, in the user's locale, sorted for browsing.
//
// The en-us name is carried alongside so the picker can match on either: most
// of the families whose name is written in Japanese answer to an English one
// too, and matching both is what lets them be typed without the IME.
const std::vector<ccl::ui::FontEntry>& InstalledFonts(IDWriteFactory* writer) {
    static std::vector<ccl::ui::FontEntry> fonts = [writer] {
        std::vector<ccl::ui::FontEntry> names;
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

            const auto nameAt = [&familyNames](UINT32 index,
                                               std::wstring& out) {
                UINT32 length = 0;
                if (FAILED(familyNames->GetStringLength(index, &length)) ||
                    length == 0) {
                    return false;
                }
                out.assign(length + 1, L'\0');
                if (FAILED(familyNames->GetString(index, out.data(),
                                                  length + 1))) {
                    return false;
                }
                out.resize(length);
                return true;
            };

            // Prefer the name in the user's language, falling back to the
            // first one the font offers.
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(familyNames->FindLocaleName(locale, &index, &exists)) ||
                !exists) {
                index = 0;
            }

            ccl::ui::FontEntry entry;
            if (!nameAt(index, entry.shown)) {
                continue;
            }

            // Absent for a handful of families, which are then reachable by
            // their own name alone.
            UINT32 englishIndex = 0;
            BOOL hasEnglish = FALSE;
            if (SUCCEEDED(familyNames->FindLocaleName(L"en-us", &englishIndex,
                                                      &hasEnglish)) &&
                hasEnglish && englishIndex != index) {
                if (!nameAt(englishIndex, entry.english)) {
                    entry.english.clear();
                }
            }

            names.push_back(std::move(entry));
        }

        std::sort(names.begin(), names.end(),
                  [](const ccl::ui::FontEntry& a, const ccl::ui::FontEntry& b) {
                      return a.shown < b.shown;
                  });
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
    kMenuFill,
    kMenuFillMarker,
    kMenuOutline,
    kMenuOutlineMarker,
    kMenuMosaic,
    kMenuBlur,
    kMenuClearSelection,
    kMenuObjectRotate,
    kMenuObjectRaise,
    kMenuObjectLower,
    kMenuObjectToFront,
    kMenuObjectToBack,
    kMenuCrop,
    kMenuRotateLeft,
    kMenuRotateRight,
    kMenuRotate180,
    kMenuRotateFree,
    kMenuFlipHorizontal,
    kMenuFlipVertical,
    kMenuConcat,
    kMenuCaptureSelf,
    kMenuRecapture,
    kMenuSettings,
    kMenuCommitText,
    kMenuEyedropper,
    kMenuColorPicker,
    kMenuExit,
    kMenuTextSize,
    kMenuTextDecor,
    kMenuFontPick,

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

// Where each character of what the control returns ends up once the text is
// stored. A line break comes back as CRLF -- two characters -- and is kept as a
// single LF, so everything past one sits a character earlier than it did.
//
// Styling positions are taken in the control's own coordinates, which is why
// this is needed: without it, a run in the second line of a piece of text was
// written down one character to the right of where it belonged, and two lines
// down it was two characters out.
std::vector<unsigned int> StoredOffsets(const std::wstring& raw) {
    std::vector<unsigned int> map(raw.size() + 1, 0);
    unsigned int stored = 0;
    size_t i = 0;
    while (i < raw.size()) {
        map[i] = stored;
        if (raw[i] == L'\r' && i + 1 < raw.size() && raw[i + 1] == L'\n') {
            // The pair becomes one character, so both halves point at it.
            map[i + 1] = stored;
            i += 2;
        } else {
            ++i;
        }
        ++stored;
    }
    map[raw.size()] = stored;
    return map;
}

// The way back, for text being opened again: every stored line break takes two
// characters once it is in the control.
std::vector<unsigned int> EditorOffsets(const std::wstring& stored) {
    std::vector<unsigned int> map(stored.size() + 1, 0);
    unsigned int editor = 0;
    for (size_t i = 0; i < stored.size(); ++i) {
        map[i] = editor;
        editor += (stored[i] == L'\n') ? 2u : 1u;
    }
    map[stored.size()] = editor;
    return map;
}

std::vector<ccl::doc::TextRun> MoveRuns(
    const std::vector<ccl::doc::TextRun>& runs,
    const std::vector<unsigned int>& map) {
    std::vector<ccl::doc::TextRun> moved;
    moved.reserve(runs.size());
    const auto last = map.empty() ? 0u : static_cast<unsigned int>(map.size() - 1);
    for (const ccl::doc::TextRun& run : runs) {
        const auto from = std::min<unsigned int>(run.start, last);
        const auto to = std::min<unsigned int>(run.start + run.length, last);
        ccl::doc::TextRun shifted = run;
        shifted.start = map[from];
        shifted.length = map[to] - map[from];
        // A run covering only the second half of a line break has nothing left
        // to describe once the pair has become one character.
        if (shifted.length > 0) {
            moved.push_back(shifted);
        }
    }
    return moved;
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

    // Re-measure after the changes a change notification does not cover.
    //
    // Typing, backspace and delete all raise EN_CHANGE -- delete does so
    // without ever sending a WM_CHAR -- so none of them need driving from here,
    // and asking on every key down made moving the caret cost as much as typing
    // a character. Composing with an IME is the case that does need it: the
    // text grows and shrinks all through a composition without a single
    // EN_CHANGE, which only arrives once the composition is committed.
    switch (msg) {
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

// Points that stand for a piece of a selection when asking whether it meets
// something. Corners, middle and edge midpoints for a rectangle; the recorded
// path for a lasso.
//
// Points rather than a folded geometry, for the same reason the eraser works
// this way: the question is asked while a band is being dragged out, and
// building a shape each time to answer it is work out of all proportion.
void ShapePoints(const ccl::doc::SelectionShape& shape,
                 std::vector<D2D1_POINT_2F>& out) {
    if (shape.lasso) {
        for (const ccl::doc::SelectionPoint& point : shape.points) {
            out.push_back(D2D1::Point2F(point.x, point.y));
        }
        return;
    }
    const float left = (std::min)(shape.left, shape.right);
    const float right = (std::max)(shape.left, shape.right);
    const float top = (std::min)(shape.top, shape.bottom);
    const float bottom = (std::max)(shape.top, shape.bottom);
    const float midX = (left + right) * 0.5f;
    const float midY = (top + bottom) * 0.5f;
    out.push_back(D2D1::Point2F(left, top));
    out.push_back(D2D1::Point2F(right, top));
    out.push_back(D2D1::Point2F(right, bottom));
    out.push_back(D2D1::Point2F(left, bottom));
    out.push_back(D2D1::Point2F(midX, midY));
    out.push_back(D2D1::Point2F(midX, top));
    out.push_back(D2D1::Point2F(midX, bottom));
    out.push_back(D2D1::Point2F(left, midY));
    out.push_back(D2D1::Point2F(right, midY));
}

// The box a set of pieces occupies. False when there are no pieces.
bool ShapesBounds(const ccl::doc::SelectionShapes& shapes,
                  D2D1_RECT_F& bounds) noexcept {
    bool any = false;
    for (const ccl::doc::SelectionShape& shape : shapes) {
        std::vector<D2D1_POINT_2F> points;
        ShapePoints(shape, points);
        for (const D2D1_POINT_2F& point : points) {
            if (!any) {
                bounds = D2D1::RectF(point.x, point.y, point.x, point.y);
                any = true;
                continue;
            }
            bounds.left = (std::min)(bounds.left, point.x);
            bounds.top = (std::min)(bounds.top, point.y);
            bounds.right = (std::max)(bounds.right, point.x);
            bounds.bottom = (std::max)(bounds.bottom, point.y);
        }
    }
    return any;
}

bool PointInRect(const D2D1_RECT_F& box, float x, float y) noexcept {
    return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
}

// Slides one piece of annotation. Every kind carries its position differently
// -- a stroke in its points, paint in the shape it was given, text in one
// corner -- so each is moved in its own terms rather than through a transform
// laid over the drawing.
void TranslateAnnotation(ccl::doc::Annotation& annotation, float dx,
                         float dy) noexcept {
    switch (annotation.kind) {
        case ccl::doc::AnnotationKind::Stroke:
            for (ccl::doc::StrokePoint& point : annotation.stroke.points) {
                point.x += dx;
                point.y += dy;
            }
            return;
        case ccl::doc::AnnotationKind::Text:
            annotation.text.x += dx;
            annotation.text.y += dy;
            return;
        case ccl::doc::AnnotationKind::Area:
            ccl::doc::TranslateShapes(annotation.area.shape, dx, dy);
            return;
        case ccl::doc::AnnotationKind::Effect:
            // Never moved: what it shows was cut out of the picture where it
            // was placed, and would arrive somewhere else showing the wrong
            // thing.
            return;
    }
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
            // A number being typed shows on the text as it goes, so that the
            // value is judged against the real thing rather than guessed at.
            if (HIWORD(wParam) == EN_CHANGE &&
                reinterpret_cast<HWND>(lParam) == numberBox_) {
                UpdateNumberEntry();
                return 0;
            }
            break;

        case WM_CTLCOLOREDIT: {
            // The box a number is typed into is an ordinary box, filled and
            // legible. Only the one that previews text belongs over the
            // picture; leaving this one transparent showed the picture through
            // it and left old digits behind, which cannot be typed into.
            if (reinterpret_cast<HWND>(lParam) == numberBox_) {
                break;
            }
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
            // What the selecting tools show depends on which modifier is held,
            // so the pointer is redrawn as one goes down rather than waiting
            // for the next move. Only on the way down, not on every repeat.
            if ((wParam == VK_SHIFT || wParam == VK_MENU) &&
                (lParam & 0x40000000) == 0) {
                UpdateCursor();
            }
            OnKeyDown(wParam);
            return 0;

        case WM_KEYUP:
            if (wParam == VK_SPACE) {
                spaceHeld_ = false;
                UpdateCursor();
            }
            if (wParam == VK_SHIFT || wParam == VK_MENU) {
                UpdateCursor();
            }
            return 0;

        // Alt on its own arrives here rather than as an ordinary key. Taken
        // only far enough to keep the pointer honest about what a press would
        // do; everything else about it is read from the keyboard state when it
        // matters.
        case WM_SYSKEYDOWN:
            if (wParam == VK_MENU && (lParam & 0x40000000) == 0) {
                UpdateCursor();
            }
            break;

        case WM_SYSKEYUP:
            if (wParam == VK_MENU) {
                UpdateCursor();
            }
            break;

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
            // An area being dragged out is dropped rather than kept: the button
            // was never let go of, so nothing here was settled on. Left set,
            // this went on following the pointer with no button held.
            //
            // Recorded all the same. Losing the pointer part way through is not
            // something that was asked for, so getting back what was there has
            // to be possible.
            if (movingPicked_) {
                // Kept where it has got to rather than put back. The step was
                // recorded when the drag became one, so undo returns it.
                movingPicked_ = false;
                pickedDragMoved_ = false;
                pickedOriginals_.clear();
                UpdateTitle();
                Draw();
            } else if (selecting_ && IsObjectTool(tool_.tool)) {
                // Nothing to record: a band that picks out pieces settles no
                // area, and what was picked before the press is untouched
                // until the button comes up.
                bandClickId_ = 0;
                selecting_ = false;
                pending_ = ccl::doc::SelectionShape{};
                RefreshSelection();
                Draw();
            } else if (selecting_) {
                if (document_ != nullptr) {
                    history_.RecordSelection(document_->Annotations(),
                                             selectionBeforeDrag_,
                                             ToolForHistory());
                }
                selectionBeforeDrag_.clear();
                ClearSelection();
                UpdateTitle();
                Draw();
            }
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
            // Before anything is taken apart, since a write that fails leaves
            // the window open and everything has to still work.
            if (!AutoSaveBeforeClosing(Departure::Window)) {
                return 0;
            }
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
            ::DestroyWindow(hwnd_);
            return 0;

        // Ending the session does not close windows: WM_CLOSE never arrives,
        // so without this the capture would go with the shutdown unwritten.
        // Tested against wParam != FALSE, since anything non-zero means the
        // session really is ending; FALSE is a shutdown someone called off.
        case WM_ENDSESSION:
            if (wParam != FALSE) {
                AutoSaveBeforeClosing(Departure::SessionEnd);
            }
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
    // Asked of the area rather than of the box around it: a selection taken
    // apart again still has a box, and it is a box of infinities.
    return selectionGeometry_.Area() >= 1.0f;
}

D2D1_RECT_F ClipWindow::SelectionBounds() const noexcept {
    return selectionGeometry_.Bounds();
}

ccl::doc::SelectionShapes ClipWindow::CurrentShapes() const noexcept {
    ccl::doc::SelectionShapes shapes = selection_;
    if (selecting_) {
        shapes.push_back(pending_);
    }
    return shapes;
}

void ClipWindow::RefreshSelection() noexcept {
    ID2D1Factory* factory = context_ != nullptr ? context_->Factory() : nullptr;
    selectionGeometry_.Rebuild(factory, CurrentShapes());

    if (selecting_ && pending_.op == ccl::doc::SelectionOp::Subtract) {
        // On its own, so it has a shape even where it reaches outside what is
        // selected. Taken as a piece in its own right rather than as a
        // subtraction, which is what "Replace" means to the folding.
        ccl::doc::SelectionShape shown = pending_;
        shown.op = ccl::doc::SelectionOp::Replace;
        removingGeometry_.Rebuild(factory, {shown});
    } else {
        removingGeometry_.Clear();
    }
}

void ClipWindow::ClearSelection() noexcept {
    selection_.clear();
    selecting_ = false;
    pending_ = ccl::doc::SelectionShape{};
    selectionGeometry_.Clear();
    removingGeometry_.Clear();
}

void ClipWindow::ClearPicked() noexcept { pickedIds_.clear(); }

void ClipWindow::PrunePicked() noexcept {
    if (pickedIds_.empty() || document_ == nullptr) {
        pickedIds_.clear();
        return;
    }
    const ccl::doc::AnnotationList& annotations = document_->Annotations();
    std::vector<unsigned int> kept;
    for (unsigned int id : pickedIds_) {
        for (const ccl::doc::Annotation& annotation : annotations) {
            if (annotation.id == id) {
                kept.push_back(id);
                break;
            }
        }
    }
    pickedIds_ = std::move(kept);
}

bool ClipWindow::AnnotationBounds(const ccl::doc::Annotation& annotation,
                                  D2D1_RECT_F& bounds) noexcept {
    switch (annotation.kind) {
        case ccl::doc::AnnotationKind::Stroke: {
            const ccl::doc::Stroke& stroke = annotation.stroke;
            if (stroke.points.empty()) {
                return false;
            }
            bool any = false;
            for (const ccl::doc::StrokePoint& point : stroke.points) {
                // Half the width each way: the line is drawn about its path,
                // not to one side of it.
                const float reach = point.width * 0.5f;
                const D2D1_RECT_F box =
                    D2D1::RectF(point.x - reach, point.y - reach,
                                point.x + reach, point.y + reach);
                if (!any) {
                    bounds = box;
                    any = true;
                    continue;
                }
                bounds.left = (std::min)(bounds.left, box.left);
                bounds.top = (std::min)(bounds.top, box.top);
                bounds.right = (std::max)(bounds.right, box.right);
                bounds.bottom = (std::max)(bounds.bottom, box.bottom);
            }
            return any;
        }
        case ccl::doc::AnnotationKind::Text: {
            if (!renderer_.MeasureText(annotation.text, bounds)) {
                return false;
            }
            if (annotation.text.angle != 0.0f) {
                bounds = ccl::render::TurnedBounds(
                    bounds, annotation.text.angle,
                    D2D1::Point2F(annotation.text.x, annotation.text.y));
            }
            return true;
        }
        case ccl::doc::AnnotationKind::Area: {
            if (!ShapesBounds(annotation.area.shape, bounds)) {
                return false;
            }
            // A traced edge stands half its width outside the shape it
            // follows, the same way a stroke does.
            const float reach = annotation.area.width * 0.5f;
            bounds.left -= reach;
            bounds.top -= reach;
            bounds.right += reach;
            bounds.bottom += reach;
            return true;
        }
        case ccl::doc::AnnotationKind::Effect:
            bounds = D2D1::RectF(annotation.effect.left, annotation.effect.top,
                                 annotation.effect.right,
                                 annotation.effect.bottom);
            return true;
    }
    return false;
}

bool ClipWindow::TextHit(const ccl::doc::TextAnnotation& text,
                         D2D1_POINT_2F at, float slack) noexcept {
    D2D1_RECT_F box{};
    if (!renderer_.MeasureText(text, box)) {
        return false;
    }
    // Widened after the point has been turned back, never before: growing a
    // box and then turning it would put the extra room on the slant.
    box.left -= slack;
    box.top -= slack;
    box.right += slack;
    box.bottom += slack;
    if (text.angle != 0.0f) {
        const D2D1_MATRIX_3X2_F back = D2D1::Matrix3x2F::Rotation(
            -text.angle, D2D1::Point2F(text.x, text.y));
        at = D2D1::Matrix3x2F::ReinterpretBaseType(&back)->TransformPoint(at);
    }
    return PointInRect(box, at.x, at.y);
}

bool ClipWindow::TextOutlinePoints(const ccl::doc::TextAnnotation& text,
                                   std::vector<D2D1_POINT_2F>& out) noexcept {
    D2D1_RECT_F box{};
    if (!renderer_.MeasureText(text, box)) {
        return false;
    }
    ccl::doc::SelectionShape asShape;
    asShape.left = box.left;
    asShape.top = box.top;
    asShape.right = box.right;
    asShape.bottom = box.bottom;
    const size_t first = out.size();
    ShapePoints(asShape, out);
    if (text.angle == 0.0f) {
        return true;
    }
    const D2D1_MATRIX_3X2_F turn =
        D2D1::Matrix3x2F::Rotation(text.angle, D2D1::Point2F(text.x, text.y));
    for (size_t i = first; i < out.size(); ++i) {
        out[i] =
            D2D1::Matrix3x2F::ReinterpretBaseType(&turn)->TransformPoint(out[i]);
    }
    return true;
}

bool ClipWindow::AnnotationTouched(
    const ccl::doc::SelectionShapes& band,
    const ccl::doc::Annotation& annotation, float slack) noexcept {
    // Obscuring effects are left out. What they show was cut out of the
    // picture when they were placed and does not follow them, so moving one
    // would carry the wrong pixels about.
    if (annotation.kind == ccl::doc::AnnotationKind::Effect) {
        return false;
    }

    // Asked both ways round. A band drawn across a stroke meets it at the
    // stroke's own points; a band drawn inside a large patch of paint -- or a
    // click, which is a band with no size at all -- meets nothing of the kind,
    // and is only found by asking whether the band itself lands on the paint.
    const auto bandReaches = [&](float x, float y) {
        return ccl::doc::SelectionContains(band, x, y);
    };

    switch (annotation.kind) {
        case ccl::doc::AnnotationKind::Stroke: {
            const ccl::doc::Stroke& stroke = annotation.stroke;
            for (size_t i = 0; i < stroke.points.size(); ++i) {
                if (bandReaches(stroke.points[i].x, stroke.points[i].y)) {
                    return true;
                }
                // Halfway along each segment as well: a straight line is two
                // points a long way apart, and a band crossing its middle
                // touches neither end.
                if (i > 0) {
                    const float midX =
                        (stroke.points[i - 1].x + stroke.points[i].x) * 0.5f;
                    const float midY =
                        (stroke.points[i - 1].y + stroke.points[i].y) * 0.5f;
                    if (bandReaches(midX, midY)) {
                        return true;
                    }
                }
            }
            break;
        }
        case ccl::doc::AnnotationKind::Area: {
            for (const ccl::doc::SelectionShape& shape : annotation.area.shape) {
                std::vector<D2D1_POINT_2F> points;
                ShapePoints(shape, points);
                for (const D2D1_POINT_2F& point : points) {
                    if (bandReaches(point.x, point.y)) {
                        return true;
                    }
                }
            }
            break;
        }
        case ccl::doc::AnnotationKind::Text: {
            std::vector<D2D1_POINT_2F> points;
            if (!TextOutlinePoints(annotation.text, points)) {
                return false;
            }
            for (const D2D1_POINT_2F& point : points) {
                if (bandReaches(point.x, point.y)) {
                    return true;
                }
            }
            break;
        }
        default:
            return false;
    }

    // The other way round: does the band land on the annotation?
    std::vector<D2D1_POINT_2F> bandPoints;
    for (const ccl::doc::SelectionShape& shape : band) {
        ShapePoints(shape, bandPoints);
    }
    for (const D2D1_POINT_2F& point : bandPoints) {
        switch (annotation.kind) {
            case ccl::doc::AnnotationKind::Stroke:
                if (StrokeHit(annotation.stroke, point, slack)) {
                    return true;
                }
                break;
            case ccl::doc::AnnotationKind::Area:
                if (annotation.area.width > 0.0f) {
                    if (ccl::doc::SelectionNearEdge(
                            annotation.area.shape, point.x, point.y,
                            slack + annotation.area.width * 0.5f)) {
                        return true;
                    }
                } else if (ccl::doc::SelectionContains(annotation.area.shape,
                                                       point.x, point.y)) {
                    return true;
                } else if (slack > 0.0f &&
                           ccl::doc::SelectionNearEdge(annotation.area.shape,
                                                       point.x, point.y,
                                                       slack)) {
                    // Just outside a filled shape counts too, so that its edge
                    // is no harder to take hold of than its middle.
                    return true;
                }
                break;
            case ccl::doc::AnnotationKind::Text:
                if (TextHit(annotation.text, point, slack)) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

void ClipWindow::ObjectsAt(D2D1_POINT_2F image,
                           std::vector<size_t>& out) noexcept {
    out.clear();
    if (document_ == nullptr) {
        return;
    }
    const float zoom = view_.Zoom();
    const float reach = GrabSlack();
    // Held in screen pixels rather than picture ones, so that reaching for a
    // line feels the same however far the picture is zoomed in or out.
    const float slack = zoom > 0.0f ? reach / zoom : reach;
    ccl::doc::SelectionShapes point;
    ccl::doc::SelectionShape dot;
    dot.left = image.x;
    dot.top = image.y;
    dot.right = image.x;
    dot.bottom = image.y;
    point.push_back(dot);

    const ccl::doc::AnnotationList& annotations = document_->Annotations();
    // Front to back, so the list reads from the piece on top downwards, which
    // is the order repeated presses walk through it.
    for (size_t i = annotations.size(); i > 0; --i) {
        if (AnnotationTouched(point, annotations[i - 1], slack)) {
            out.push_back(i - 1);
        }
    }
}

float ClipWindow::GrabSlack() const noexcept {
    return settings_ != nullptr ? settings_->grabSlack : kGrabSlackFallback;
}

size_t ClipWindow::ObjectAt(D2D1_POINT_2F image) noexcept {
    if (document_ == nullptr) {
        return static_cast<size_t>(-1);
    }
    const float zoom = view_.Zoom();
    const float reach = GrabSlack();
    // Held in screen pixels rather than picture ones, so that reaching for a
    // line feels the same however far the picture is zoomed in or out.
    const float slack = zoom > 0.0f ? reach / zoom : reach;
    // A band with no size at all, which is what a press is.
    ccl::doc::SelectionShapes point;
    ccl::doc::SelectionShape dot;
    dot.left = image.x;
    dot.top = image.y;
    dot.right = image.x;
    dot.bottom = image.y;
    point.push_back(dot);

    const ccl::doc::AnnotationList& annotations = document_->Annotations();
    // Front to back, so the piece drawn last -- the one on top -- is the one
    // taken hold of.
    for (size_t i = annotations.size(); i > 0; --i) {
        if (AnnotationTouched(point, annotations[i - 1], slack)) {
            return i - 1;
        }
    }
    return static_cast<size_t>(-1);
}

void ClipWindow::BeginPickedDrag(POINT client) noexcept {
    if (document_ == nullptr || pickedIds_.empty()) {
        return;
    }
    pickedOriginals_.clear();
    for (const ccl::doc::Annotation& annotation : document_->Annotations()) {
        if (std::find(pickedIds_.begin(), pickedIds_.end(), annotation.id) !=
            pickedIds_.end()) {
            pickedOriginals_.push_back(annotation);
        }
    }
    if (pickedOriginals_.empty()) {
        return;
    }
    movingPicked_ = true;
    pickedDragMoved_ = false;
    pickedDragStart_ = client;
    ::SetCapture(hwnd_);
}

void ClipWindow::ContinuePickedDrag(POINT client) noexcept {
    if (!movingPicked_ || document_ == nullptr) {
        return;
    }
    const int dx = client.x - pickedDragStart_.x;
    const int dy = client.y - pickedDragStart_.y;

    // A press that has not gone far enough is still a press, not a move: it
    // is what picks a piece out, and nudging the mouse while clicking should
    // not leave the picture changed.
    if (!pickedDragMoved_ && std::abs(dx) <= kClickThreshold &&
        std::abs(dy) <= kClickThreshold) {
        return;
    }
    if (!pickedDragMoved_) {
        pickedDragMoved_ = true;
        // One step for the whole drag, taken at the moment it becomes one.
        history_.Record(document_->Annotations(), ToolForHistory());
    }

    const float zoom = view_.Zoom();
    const float imageDx = static_cast<float>(dx) / zoom;
    const float imageDy = static_cast<float>(dy) / zoom;

    ccl::doc::AnnotationList& annotations = document_->MutableAnnotations();
    for (ccl::doc::Annotation& annotation : annotations) {
        for (const ccl::doc::Annotation& original : pickedOriginals_) {
            if (original.id != annotation.id) {
                continue;
            }
            // Rebuilt from the copy each time rather than nudged along, so
            // that rounding cannot accumulate over a long drag.
            annotation = original;
            TranslateAnnotation(annotation, imageDx, imageDy);
            renderer_.InvalidateShape(annotation.id);
            break;
        }
    }
    Draw();
}

void ClipWindow::EndPickedDrag() noexcept {
    if (!movingPicked_) {
        return;
    }
    movingPicked_ = false;
    pickedDragMoved_ = false;
    pickedOriginals_.clear();
    ::ReleaseCapture();
    UpdateTitle();
    Draw();
}

bool ClipWindow::PickedBounds(D2D1_RECT_F& bounds) noexcept {
    if (document_ == nullptr || pickedIds_.empty()) {
        return false;
    }
    bool any = false;
    for (const ccl::doc::Annotation& annotation : document_->Annotations()) {
        if (std::find(pickedIds_.begin(), pickedIds_.end(), annotation.id) ==
            pickedIds_.end()) {
            continue;
        }
        D2D1_RECT_F box{};
        if (!AnnotationBounds(annotation, box)) {
            continue;
        }
        if (!any) {
            bounds = box;
            any = true;
            continue;
        }
        bounds.left = (std::min)(bounds.left, box.left);
        bounds.top = (std::min)(bounds.top, box.top);
        bounds.right = (std::max)(bounds.right, box.right);
        bounds.bottom = (std::max)(bounds.bottom, box.bottom);
    }
    return any;
}

void ClipWindow::ApplyPickedTurn(
    const std::vector<ccl::doc::Annotation>& originals, D2D1_POINT_2F about,
    float degrees) noexcept {
    if (document_ == nullptr || originals.empty()) {
        return;
    }
    const D2D1_MATRIX_3X2_F turn =
        D2D1::Matrix3x2F::Rotation(degrees, about);

    ccl::doc::AnnotationList& annotations = document_->MutableAnnotations();
    for (ccl::doc::Annotation& annotation : annotations) {
        for (const ccl::doc::Annotation& original : originals) {
            if (original.id != annotation.id) {
                continue;
            }
            annotation = original;
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke:
                    for (ccl::doc::StrokePoint& point :
                         annotation.stroke.points) {
                        const D2D1_POINT_2F moved =
                            D2D1::Matrix3x2F::ReinterpretBaseType(&turn)
                                ->TransformPoint(
                                    D2D1::Point2F(point.x, point.y));
                        point.x = moved.x;
                        point.y = moved.y;
                    }
                    break;
                case ccl::doc::AnnotationKind::Area:
                    ccl::doc::TurnShapes(annotation.area.shape, degrees,
                                         about.x, about.y);
                    break;
                case ccl::doc::AnnotationKind::Text: {
                    // The corner is carried round, and the piece is told to sit
                    // that much further round itself. Together they come to the
                    // same place a rigid turn would put it.
                    const D2D1_POINT_2F moved =
                        D2D1::Matrix3x2F::ReinterpretBaseType(&turn)
                            ->TransformPoint(D2D1::Point2F(annotation.text.x,
                                                           annotation.text.y));
                    annotation.text.x = moved.x;
                    annotation.text.y = moved.y;
                    annotation.text.angle += degrees;
                    break;
                }
                case ccl::doc::AnnotationKind::Effect:
                    break;
            }
            renderer_.InvalidateShape(annotation.id);
            break;
        }
    }
}

void ClipWindow::RotatePicked() noexcept {
    if (document_ == nullptr || pickedIds_.empty() ||
        !IsObjectTool(tool_.tool)) {
        return;
    }

    D2D1_RECT_F bounds{};
    if (!PickedBounds(bounds)) {
        return;
    }
    // The middle of what is picked, which is the point that stays where it is
    // -- the same choice turning the whole picture makes about the picture.
    const D2D1_POINT_2F about =
        D2D1::Point2F((bounds.left + bounds.right) * 0.5f,
                      (bounds.top + bounds.bottom) * 0.5f);

    std::vector<ccl::doc::Annotation> originals;
    for (const ccl::doc::Annotation& annotation : document_->Annotations()) {
        if (std::find(pickedIds_.begin(), pickedIds_.end(), annotation.id) !=
            pickedIds_.end()) {
            originals.push_back(annotation);
        }
    }
    if (originals.empty()) {
        return;
    }

    // Held rather than recorded now: a turn that is thought better of should
    // leave no step behind, and the pieces are moved about while the angle is
    // being chosen.
    const ccl::doc::AnnotationList before = document_->Annotations();

    const auto degrees =
        ccl::ui::ShowRotateDialog(hwnd_, [this, &originals, about](float angle) {
            // Shown by turning the pieces themselves rather than by laying a
            // transform over the drawing: what is on screen while the angle is
            // chosen is then the result, not a picture of it.
            ApplyPickedTurn(originals, about, angle);
            Draw();
        });

    if (!degrees.has_value()) {
        ApplyPickedTurn(originals, about, 0.0f);
        Draw();
        return;
    }

    ApplyPickedTurn(originals, about, *degrees);
    history_.Record(before, ToolForHistory());
    UpdateTitle();
    Draw();
}

void ClipWindow::ReorderPicked(int toward, bool allTheWay) noexcept {
    if (document_ == nullptr || pickedIds_.empty()) {
        return;
    }
    // Worked out on a copy first. Asking the document for a writable list moves
    // its revision on whether or not anything is written, and a press that
    // cannot move anything -- what is picked is already at the front -- should
    // leave no step behind and no frame to draw.
    ccl::doc::AnnotationList list = document_->Annotations();
    const size_t count = list.size();
    if (count < 2) {
        return;
    }

    // Plain chars rather than a vector of bool: that one hands out a proxy
    // instead of a reference, and the swaps below cannot take hold of it.
    std::vector<char> picked(count, 0);
    for (size_t i = 0; i < count; ++i) {
        picked[i] = std::find(pickedIds_.begin(), pickedIds_.end(),
                              list[i].id) != pickedIds_.end()
                        ? 1
                        : 0;
    }

    if (allTheWay) {
        // Drawn later means drawn on top, so the front of the stack is the end
        // of the list.
        ccl::doc::AnnotationList moved;
        ccl::doc::AnnotationList rest;
        for (size_t i = 0; i < count; ++i) {
            (picked[i] ? moved : rest).push_back(list[i]);
        }
        list.clear();
        if (toward > 0) {
            list.insert(list.end(), rest.begin(), rest.end());
            list.insert(list.end(), moved.begin(), moved.end());
        } else {
            list.insert(list.end(), moved.begin(), moved.end());
            list.insert(list.end(), rest.begin(), rest.end());
        }
    } else if (toward > 0) {
        // From the end, so that a run of picked pieces travels as one block
        // rather than the first of them overtaking the rest.
        for (size_t i = count - 1; i > 0; --i) {
            if (picked[i - 1] && !picked[i]) {
                std::swap(list[i - 1], list[i]);
                std::swap(picked[i - 1], picked[i]);
            }
        }
    } else {
        for (size_t i = 1; i < count; ++i) {
            if (picked[i] && !picked[i - 1]) {
                std::swap(list[i - 1], list[i]);
                std::swap(picked[i - 1], picked[i]);
            }
        }
    }

    bool changed = false;
    const ccl::doc::AnnotationList& before = document_->Annotations();
    for (size_t i = 0; i < count; ++i) {
        if (before[i].id != list[i].id) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    document_->MutableAnnotations() = std::move(list);
    // Nothing worked out for a piece has to be thrown away: what is kept is
    // kept against the id, and the ids have only changed places.
    Draw();
}

void ClipWindow::RecordPickedChange(
    const std::vector<unsigned int>& before) noexcept {
    // Only when it actually came out different. A press that reached what was
    // already picked, or a band that caught nothing new, has changed nothing,
    // and a step that puts back what is already there is a step the user has
    // to press through twice to get anywhere.
    if (document_ == nullptr || pickedIds_ == before) {
        return;
    }
    history_.RecordPicked(document_->Annotations(), before, ToolForHistory());
}

void ClipWindow::PickOne(unsigned int id, ccl::doc::SelectionOp op) noexcept {
    const auto at = std::find(pickedIds_.begin(), pickedIds_.end(), id);
    const bool held = at != pickedIds_.end();

    switch (op) {
        case ccl::doc::SelectionOp::Add:
            if (!held) {
                pickedIds_.push_back(id);
            }
            return;
        case ccl::doc::SelectionOp::Subtract:
            if (held) {
                pickedIds_.erase(at);
            }
            return;
        default:
            // Reached only if the caller widens what is offered here: a press
            // with no modifier takes hold of the piece where it lands and does
            // not come through this.
            pickedIds_.assign(1, id);
            return;
    }
}

void ClipWindow::ApplyObjectBand(ccl::doc::SelectionOp op) noexcept {
    if (document_ == nullptr) {
        return;
    }
    ccl::doc::SelectionShapes band;
    ccl::doc::SelectionShape shape = pending_;
    // Taken as a piece in its own right: what it means for what is already
    // picked is decided here, not by the folding.
    shape.op = ccl::doc::SelectionOp::Replace;
    band.push_back(shape);

    std::vector<unsigned int> hits;
    for (const ccl::doc::Annotation& annotation : document_->Annotations()) {
        // No slack: a band takes in what the eye drew round, exactly.
        if (AnnotationTouched(band, annotation, 0.0f)) {
            hits.push_back(annotation.id);
        }
    }

    const auto holds = [](const std::vector<unsigned int>& list,
                          unsigned int id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    };

    if (op == ccl::doc::SelectionOp::Replace) {
        pickedIds_ = std::move(hits);
        return;
    }
    if (op == ccl::doc::SelectionOp::Add) {
        for (unsigned int id : hits) {
            if (!holds(pickedIds_, id)) {
                pickedIds_.push_back(id);
            }
        }
        return;
    }
    std::vector<unsigned int> kept;
    for (unsigned int id : pickedIds_) {
        if (!holds(hits, id)) {
            kept.push_back(id);
        }
    }
    pickedIds_ = std::move(kept);
}

bool ClipWindow::SelectionIsSingleRect() const noexcept {
    return HasSelection() && ccl::doc::IsSingleRect(CurrentShapes());
}

bool ClipWindow::IsSelectionTool(ccl::tool::Tool tool) noexcept {
    return tool == ccl::tool::Tool::Select || tool == ccl::tool::Tool::Lasso;
}

bool ClipWindow::IsObjectTool(ccl::tool::Tool tool) noexcept {
    return tool == ccl::tool::Tool::ObjectSelect ||
           tool == ccl::tool::Tool::ObjectLasso;
}

ccl::tool::Tool ClipWindow::ToolForHistory() const noexcept {
    return tool_.tool == ccl::tool::Tool::Eyedropper ? toolBeforeEyedropper_
                                                     : tool_.tool;
}

void ClipWindow::ExtendLasso(POINT client) noexcept {
    const D2D1_POINT_2F at = ToImage(client);
    if (pending_.points.empty()) {
        pending_.points.push_back({at.x, at.y});
        return;
    }

    const float zoom = view_.Zoom() > 0.0f ? view_.Zoom() : 1.0f;
    const float spacing = lassoSpacing_ / zoom;

    const ccl::doc::SelectionPoint& last = pending_.points.back();
    const float dx = at.x - last.x;
    const float dy = at.y - last.y;
    if (dx * dx + dy * dy < spacing * spacing) {
        return;
    }
    pending_.points.push_back({at.x, at.y});

    if (pending_.points.size() <= kMaxLassoPoints) {
        return;
    }

    // Coarsened rather than cut off: a lasso that reaches the limit is one
    // still being drawn, and refusing further points would stop it following
    // the hand. What is already there is thinned to match, so the count stops
    // climbing while the shape stays the shape.
    lassoSpacing_ *= 2.0f;
    const float coarse = lassoSpacing_ / zoom;

    std::vector<ccl::doc::SelectionPoint> thinned;
    thinned.reserve(pending_.points.size() / 2 + 2);
    thinned.push_back(pending_.points.front());
    for (size_t i = 1; i + 1 < pending_.points.size(); ++i) {
        const ccl::doc::SelectionPoint& point = pending_.points[i];
        const ccl::doc::SelectionPoint& kept = thinned.back();
        const float ex = point.x - kept.x;
        const float ey = point.y - kept.y;
        if (ex * ex + ey * ey >= coarse * coarse) {
            thinned.push_back(point);
        }
    }
    // The end is where the pointer is, so it is never one of the ones dropped.
    thinned.push_back(pending_.points.back());
    pending_.points = std::move(thinned);
}

void ClipWindow::RestoreTool(ccl::tool::Tool tool) noexcept {
    // Not while the eyedropper is armed: it holds a hook over the whole screen
    // and hands the tool back itself when it is done. Cutting in front of that
    // would leave the hook with nowhere to return to.
    if (tool_.tool == ccl::tool::Tool::Eyedropper || tool_.tool == tool) {
        return;
    }
    tool_.tool = tool;
    UpdateCursor();
}

void ClipWindow::ApplyEffectToSelection(ccl::doc::EffectKind kind) noexcept {
    if (!HasSelection() || document_ == nullptr) {
        return;
    }

    // Held to whole pixels and to the inside of the picture, and the same
    // numbers used from here on. The piece cut out is clamped whether or not
    // this is, so leaving it unclamped means cutting out less than is drawn
    // back -- which arrives stretched, its blocks a different size from
    // everywhere else. Rounded outwards so nothing selected is left out.
    const D2D1_RECT_F bounds = SelectionBounds();
    const auto width = static_cast<float>(document_->Width());
    const auto height = static_cast<float>(document_->Height());

    const D2D1_RECT_F area = D2D1::RectF(
        std::clamp(std::floor(bounds.left), 0.0f, width),
        std::clamp(std::floor(bounds.top), 0.0f, height),
        std::clamp(std::ceil(bounds.right), 0.0f, width),
        std::clamp(std::ceil(bounds.bottom), 0.0f, height));

    if (area.right - area.left < 1.0f || area.bottom - area.top < 1.0f) {
        return;
    }

    // Recorded once it is settled that something will be added, so that a
    // press which comes to nothing does not leave an undo step behind.
    history_.Record(document_->Annotations(), ToolForHistory());

    ccl::doc::Annotation annotation;
    annotation.id = ccl::doc::NextAnnotationId();
    annotation.kind = ccl::doc::AnnotationKind::Effect;
    annotation.effect.kind = kind;
    annotation.effect.left = area.left;
    annotation.effect.top = area.top;
    annotation.effect.right = area.right;
    annotation.effect.bottom = area.bottom;

    // A plain rectangle needs no mask: the box is the shape.
    if (!SelectionIsSingleRect()) {
        annotation.effect.mask = CurrentShapes();
    }

    // Reuses the strength last chosen for this effect. The first time round
    // there is none, so it is scaled to the area instead -- a small region
    // must not collapse into a single block.
    const bool mosaic = kind == ccl::doc::EffectKind::Mosaic;
    const float remembered = mosaic ? tool_.mosaicStrength : tool_.blurStrength;

    if (remembered >= 0.0f) {
        annotation.effect.strength = remembered;
    } else {
        // Taken from how much is covered rather than from the box around it.
        // A thin band drawn corner to corner fills a large box while covering
        // very little, and sizing blocks to the box would hide it under two or
        // three of them. For a square the two agree.
        const float span = std::sqrt(selectionGeometry_.Area());
        annotation.effect.strength = mosaic
                                         ? std::clamp(span / 12.0f, 3.0f, 48.0f)
                                         : std::clamp(span / 16.0f, 2.0f, 24.0f);
    }

    if (mosaic) {
        tool_.mosaicStrength = annotation.effect.strength;
    } else {
        tool_.blurStrength = annotation.effect.strength;
    }

    document_->MutableAnnotations().push_back(std::move(annotation));

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

std::vector<ccl::doc::SelectionShapes> ClipWindow::SelectionPieces()
    const noexcept {
    std::vector<ccl::doc::SelectionShapes> pieces;

    // Each piece added starts a patch of its own, and each piece taken away
    // applies to every patch standing at that moment -- which is how the
    // folding treats them, so the patches together come to the same area.
    for (const ccl::doc::SelectionShape& shape : CurrentShapes()) {
        if (shape.op == ccl::doc::SelectionOp::Subtract) {
            for (ccl::doc::SelectionShapes& piece : pieces) {
                piece.push_back(shape);
            }
            continue;
        }
        if (shape.op == ccl::doc::SelectionOp::Replace) {
            pieces.clear();
        }
        ccl::doc::SelectionShape first = shape;
        first.op = ccl::doc::SelectionOp::Replace;
        pieces.push_back({first});
    }

    ID2D1Factory* factory = context_ != nullptr ? context_->Factory() : nullptr;
    if (factory == nullptr) {
        return pieces;
    }

    const auto shapeOf = [factory](const ccl::doc::SelectionShapes& piece) {
        return ccl::render::BuildSelectionGeometry(factory, piece);
    };

    // Anything left covering nothing -- a patch taken away again -- would only
    // produce an annotation with nothing to draw.
    for (size_t i = pieces.size(); i > 0; --i) {
        const auto geometry = shapeOf(pieces[i - 1]);
        float area = 0.0f;
        if (!geometry ||
            FAILED(geometry->ComputeArea(D2D1::Matrix3x2F::Identity(), &area)) ||
            area <= 0.0f) {
            pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }

    // Join the ones that meet, until none of them do.
    for (bool joined = true; joined;) {
        joined = false;
        for (size_t i = 0; i < pieces.size() && !joined; ++i) {
            for (size_t j = i + 1; j < pieces.size() && !joined; ++j) {
                const auto left = shapeOf(pieces[i]);
                const auto right = shapeOf(pieces[j]);
                if (!left || !right) {
                    continue;
                }
                D2D1_GEOMETRY_RELATION relation =
                    D2D1_GEOMETRY_RELATION_UNKNOWN;
                if (FAILED(left->CompareWithGeometry(
                        right.Get(), D2D1::Matrix3x2F::Identity(),
                        &relation)) ||
                    relation == D2D1_GEOMETRY_RELATION_DISJOINT) {
                    continue;
                }

                for (size_t k = 0; k < pieces[j].size(); ++k) {
                    ccl::doc::SelectionShape shape = pieces[j][k];
                    // Folded onto the end of the other patch rather than
                    // replacing it.
                    if (k == 0) {
                        shape.op = ccl::doc::SelectionOp::Add;
                    }
                    pieces[i].push_back(shape);
                }
                pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(j));
                joined = true;
            }
        }
    }

    return pieces;
}

void ClipWindow::PaintSelection(float opacity, float width) noexcept {
    if (!HasSelection() || document_ == nullptr) {
        return;
    }

    // One annotation per patch, so that paint laid over several places can be
    // rubbed out one place at a time. Undo is unaffected: the state before is
    // recorded once, however many patches go down.
    const std::vector<ccl::doc::SelectionShapes> pieces = SelectionPieces();
    if (pieces.empty()) {
        return;
    }

    history_.Record(document_->Annotations(), ToolForHistory());

    for (const ccl::doc::SelectionShapes& piece : pieces) {
        ccl::doc::Annotation annotation;
        annotation.id = ccl::doc::NextAnnotationId();
        annotation.kind = ccl::doc::AnnotationKind::Area;
        annotation.area.shape = piece;
        annotation.area.color = tool_.Color();
        annotation.area.opacity = opacity;
        annotation.area.antialias = tool_.antialias;
        annotation.area.width = width;

        document_->MutableAnnotations().push_back(std::move(annotation));
    }

    // The size keys have nothing to adjust here, so whatever effect they were
    // pointed at stops being the thing they act on. Left alone, they would go
    // on changing something no longer on top.
    adjustingEffectIndex_ = static_cast<size_t>(-1);

    // The area stays selected, as it does after an effect: one pass of colour
    // is rarely the end of what is wanted there.
    UpdateTitle();
    Draw();
}

void ClipWindow::StepEffectStrength(int steps) noexcept {
    if (document_ == nullptr ||
        adjustingEffectIndex_ >= document_->Annotations().size()) {
        return;
    }

    auto& annotation =
        document_->MutableAnnotations()[adjustingEffectIndex_];
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

    // The four selecting tools each get a pointer of their own. Sharing the
    // crosshair between them meant the only way to find out which one was in
    // hand was to try it, which is the opposite of what a tool of its own is
    // for. Alt turns all four red, matching the outline of a subtraction.
    if (IsSelectionTool(tool_.tool) || IsObjectTool(tool_.tool)) {
        const bool object = IsObjectTool(tool_.tool);
        const bool adding = IsKeyDown(VK_SHIFT);
        const bool removing = IsKeyDown(VK_MENU);

        // With a modifier held, a press on a piece adds it or takes it out
        // rather than taking hold of it, so the four-way arrow would be
        // promising a move that is not on offer. What the pointer shows and
        // what the press does are decided together, here.
        if (object && !adding && !removing) {
            // Over a piece it becomes the four-way arrow, which is what the
            // text tool shows where a press would move something.
            //
            // Asked of `ObjectAt`, the very question the press asks. Anything
            // else drifts: the arrow would appear where nothing can be held.
            //
            // The live position rather than `lastCursor_`. WM_SETCURSOR
            // arrives before the WM_MOUSEMOVE that records it, so the stored
            // one is a move behind -- measured 5 times out of 5 on 2026-08-30,
            // and the arrow failed to appear on arriving at a piece in one
            // jump.
            POINT at = lastCursor_;
            if (::GetCursorPos(&at)) {
                ::ScreenToClient(hwnd_, &at);
            }
            if (ObjectAt(ToImage(at)) != static_cast<size_t>(-1)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return;
            }
        }

        const bool lasso = tool_.tool == ccl::tool::Tool::Lasso ||
                           tool_.tool == ccl::tool::Tool::ObjectLasso;
        const auto which =
            object ? (lasso ? ccl::ui::SelectCursor::ObjectLasso
                            : ccl::ui::SelectCursor::ObjectRect)
                   : (lasso ? ccl::ui::SelectCursor::Lasso
                            : ccl::ui::SelectCursor::Rect);

        toolCursors_.Build();
        // Drawing them can fail, and a window with no pointer at all is worse
        // than one with the crosshair it used to have.
        if (HCURSOR drawn = toolCursors_.Get(which, removing); drawn != nullptr) {
            ::SetCursor(drawn);
        } else {
            ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
        }
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

        if (TextHit(annotation.text, image)) {
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
    history_.Record(document_->Annotations(), ToolForHistory());

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
    // The outline and the shadow are deliberately not taken. They cannot be
    // switched while typing, so carrying them into the current value would only
    // move the setting for the next piece of text about for no reason.

    auto& annotations = document_->MutableAnnotations();
    annotations.erase(annotations.begin() +
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
        editingExisting_ ? editingOriginal_.fontSize : CurrentTextSize();
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

    // Word wrap off, so the box grows with the text and breaks only where a
    // line break was typed.
    //
    // The width given here is what the control formats against, and zero does
    // not mean "no width" -- it means "use the client area", which is wrapping.
    // Measured: with zero a long line came back as two, and with any width at
    // all it stayed as one. Wrapping is what made the editing box disagree with
    // the drawing about where the lines were.
    ::SendMessageW(editor_, EM_SETTARGETDEVICE, 0, 1000000);

    // Left to itself, the control picks a font of its own for each stretch of
    // characters -- one face for kana, another for symbols -- and then reports
    // those faces back as though they had been chosen. Committed text ended up
    // carrying fonts nobody asked for, and reopening it wrote every one of
    // those stretches back in turn: measured, a plain 530-character note came
    // back as 82 stretches and took five seconds to reopen.
    //
    // The font the picture will draw with is the one that was chosen, so the
    // box is told to leave it alone. Characters the chosen font has no glyph
    // for are the drawing's problem to solve, as they already were.
    const LRESULT languageOptions =
        ::SendMessageW(editor_, EM_GETLANGOPTIONS, 0, 0);
    ::SendMessageW(editor_, EM_SETLANGOPTIONS, 0,
                   languageOptions & ~(IMF_AUTOFONT | IMF_DUALFONT));

    // Only changes are of interest. The size the control says it needs is not:
    // measured, it never grew in height and grew a little in width every time
    // it was asked, so the box is fitted from the text instead.
    ::SendMessageW(editor_, EM_SETEVENTMASK, 0, ENM_CHANGE);

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

    // Taken once and kept: reading the styling through this leaves the
    // selection alone, where asking about each character in turn made the
    // control repaint itself once per character on every keystroke.
    {
        IRichEditOle* ole = nullptr;
        ::SendMessageW(editor_, EM_GETOLEINTERFACE, 0,
                       reinterpret_cast<LPARAM>(&ole));
        if (ole != nullptr) {
            if (FAILED(ole->QueryInterface(
                    __uuidof(ITextDocument),
                    reinterpret_cast<void**>(&editorDoc_)))) {
                editorDoc_ = nullptr;
            }
            ole->Release();
        }
    }

    if (editingExisting_) {
        ::SetWindowTextW(editor_,
                         ToEditorLineEndings(editingOriginal_.text).c_str());

        // Baseline styling first, then the ranges that differ from it, so that
        // reopening text shows exactly what was committed.
        ::SendMessageW(editor_, EM_SETSEL, 0, -1);
        ApplyCharFormat(true);

        // Back into the control's coordinates, where each line break takes two
        // characters again.
        const std::vector<ccl::doc::TextRun> restored =
            MoveRuns(editingOriginal_.runs, EditorOffsets(editingOriginal_.text));

        const UINT editorDpi = ccl::dpi::ForWindow(hwnd_);

        // What the whole box was just given. Every write costs the control a
        // fresh layout -- measured at about 8.5ms each -- so a range that only
        // repeats one of these is left alone. Most ranges say nothing new
        // about most attributes, and a piece of text with no styling at all
        // comes back as one range that repeats all of them.
        const LONG baseHeight =
            PixelsToTwips(editingOriginal_.fontSize * zoom, editorDpi);
        const COLORREF baseColour = ToColorRef(tool_.Color());
        const std::wstring& baseFace = CurrentTextFont();

        for (const ccl::doc::TextRun& run : restored) {
            const float runSize = run.fontSize > 0.0f
                                      ? run.fontSize
                                      : editingOriginal_.fontSize;
            const std::wstring& face = run.fontFamily.empty()
                                           ? editingOriginal_.fontFamily
                                           : run.fontFamily;

            const LONG runHeight = PixelsToTwips(runSize * zoom, editorDpi);
            const COLORREF runColour = ToColorRef(run.color);
            const bool sizeDiffers = runHeight != baseHeight;
            const bool colourDiffers = runColour != baseColour;
            const bool faceDiffers = face != baseFace;
            const bool boldDiffers = run.bold != tool_.textBold;
            const bool italicDiffers = run.italic != tool_.textItalic;
            const bool underlineDiffers = run.underline != tool_.textUnderline;
            const bool strikeDiffers =
                run.strikethrough != tool_.textStrikethrough;

            if (!sizeDiffers && !colourDiffers && !faceDiffers &&
                !boldDiffers && !italicDiffers && !underlineDiffers &&
                !strikeDiffers) {
                continue;
            }

            // Written through the Text Object Model, which does not move the
            // selection. Selecting each range in turn to format it made the
            // control repaint once per range, which showed as a flicker every
            // time a piece of text with several styles was opened again.
            ITextRange* range = nullptr;
            if (editorDoc_ != nullptr &&
                SUCCEEDED(editorDoc_->Range(
                    static_cast<long>(run.start),
                    static_cast<long>(run.start + run.length), &range)) &&
                range != nullptr) {
                ITextFont* font = nullptr;
                if (SUCCEEDED(range->GetFont(&font)) && font != nullptr) {
                    if (sizeDiffers) {
                        // Sizes go in as points, the same way they come out.
                        font->SetSize(TwipsToPoints(runHeight));
                    }
                    if (colourDiffers) {
                        font->SetForeColor(static_cast<long>(runColour));
                    }
                    if (boldDiffers) {
                        font->SetBold(run.bold ? tomTrue : tomFalse);
                    }
                    if (italicDiffers) {
                        font->SetItalic(run.italic ? tomTrue : tomFalse);
                    }
                    if (underlineDiffers) {
                        font->SetUnderline(run.underline ? tomSingle : tomNone);
                    }
                    if (strikeDiffers) {
                        font->SetStrikeThrough(run.strikethrough ? tomTrue
                                                                 : tomFalse);
                    }
                    if (faceDiffers) {
                        BSTR name = ::SysAllocString(face.c_str());
                        if (name != nullptr) {
                            font->SetName(name);
                            ::SysFreeString(name);
                        }
                    }
                    font->Release();
                }
                range->Release();
                continue;
            }

            // No Text Object Model to be had: the old way still works, it just
            // repaints as it goes.
            const CHARRANGE selection{
                static_cast<LONG>(run.start),
                static_cast<LONG>(run.start + run.length)};
            ::SendMessageW(editor_, EM_EXSETSEL, 0,
                           reinterpret_cast<LPARAM>(&selection));

            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            // Size and face belong here as much as the rest. Left out, a piece
            // of text with one word made bigger came back all one size, and
            // committing it again wrote that flattening into the picture.
            //
            // Only what this range says differently, for the same reason the
            // path above skips writes: the effects travel together because
            // they share one field.
            format.dwMask = (colourDiffers ? CFM_COLOR : 0) |
                            (sizeDiffers ? CFM_SIZE : 0) |
                            (faceDiffers ? CFM_FACE : 0) |
                            (boldDiffers ? CFM_BOLD : 0) |
                            (italicDiffers ? CFM_ITALIC : 0) |
                            (underlineDiffers ? CFM_UNDERLINE : 0) |
                            (strikeDiffers ? CFM_STRIKEOUT : 0);
            format.crTextColor = ToColorRef(run.color);
            format.yHeight = PixelsToTwips(runSize * zoom, editorDpi);
            ::wcsncpy_s(format.szFaceName, face.c_str(), _TRUNCATE);
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

    // Fitting the box also pins the line pitch, so the paragraph format does
    // not have to be applied separately here.
    ResizeEditor();
    ::SetFocus(editor_);
}

void ClipWindow::SetTextFont(const std::wstring& family) noexcept {
    if (editor_ == nullptr) {
        // Nothing is being typed and nothing is being pointed at, so this is
        // the face the next piece of text will be given. Choosing one here did
        // nothing at all until the session's own values were separated from
        // the settings file: there was nowhere to put it that would not have
        // become the saved default.
        tool_.textFontFamily = family;
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
        size = CurrentTextSize();
    }

    size = SteppedFontSize(size, steps);

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
        editingExisting_ ? editingOriginal_.fontSize : CurrentTextSize();

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
    ::wcsncpy_s(format.szFaceName, CurrentTextFont().c_str(), _TRUNCATE);

    // Applied to the selection so that styling affects the chosen characters,
    // or -- with nothing selected -- whatever is typed next.
    ::SendMessageW(editor_, EM_SETCHARFORMAT,
                   wholeText ? SCF_ALL : SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&format));
}

size_t ClipWindow::HoveredTextTarget() const noexcept {
    if (tool_.tool != ccl::tool::Tool::Text || editor_ != nullptr ||
        document_ == nullptr ||
        hoveredTextIndex_ >= document_->Annotations().size()) {
        return static_cast<size_t>(-1);
    }
    return document_->Annotations()[hoveredTextIndex_].kind ==
                   ccl::doc::AnnotationKind::Text
               ? hoveredTextIndex_
               : static_cast<size_t>(-1);
}

void ClipWindow::RefreshHoveredText() noexcept {
    if (tool_.tool != ccl::tool::Tool::Text || editor_ != nullptr ||
        document_ == nullptr) {
        return;
    }

    const size_t hovered = FindTextAt(ToImage(lastCursor_));
    if (hovered == hoveredTextIndex_) {
        return;
    }

    // Kept in step with the outline drawn around it: what a change is about to
    // land on has to be the thing the picture says it will land on.
    hoveredTextIndex_ = hovered;
    resizingTextId_ = 0;
    UpdateCursor();
    Draw();
}

void ClipWindow::ToggleTextOutline() noexcept {
    const size_t target = HoveredTextTarget();
    if (target == static_cast<size_t>(-1)) {
        // Nothing pointed at: this is the setting the next piece of text will
        // be given.
        tool_.textOutline = !tool_.textOutline;
        return;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[target];
    annotation.text.outline = !annotation.text.outline;
    // The glyphs are unchanged, so what was traced still stands; only the
    // drawn pixels have to be made again.
    renderer_.InvalidateTextPixels(annotation.id);
    Draw();
}

bool ClipWindow::StyleHoveredText(bool ccl::doc::TextAnnotation::* whole,
                                  bool ccl::doc::TextRun::* part) noexcept {
    const size_t target = HoveredTextTarget();
    if (target == static_cast<size_t>(-1)) {
        return false;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[target];
    ccl::doc::TextAnnotation& text = annotation.text;

    const bool wanted = !(text.*whole);
    text.*whole = wanted;
    for (ccl::doc::TextRun& run : text.runs) {
        run.*part = wanted;
    }

    // Bold and the rest change the shapes of the glyphs, so the layout kept
    // under this id is no longer the one being asked for. Colour is the odd one
    // out there: it is put on afresh every frame and needs no such notice.
    renderer_.InvalidateText(annotation.id);
    Draw();
    return true;
}

bool ClipWindow::RefontHoveredText(const std::wstring& family) noexcept {
    const size_t target = HoveredTextTarget();
    if (target == static_cast<size_t>(-1)) {
        return false;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[target];
    ccl::doc::TextAnnotation& text = annotation.text;

    text.fontFamily = family;
    // Ranges carrying a face of their own would otherwise keep it and the piece
    // would come out in two fonts, which is not what choosing one asks for.
    for (ccl::doc::TextRun& run : text.runs) {
        run.fontFamily = family;
    }

    renderer_.InvalidateText(annotation.id);
    Draw();
    return true;
}

void ClipWindow::ToggleTextShadow() noexcept {
    const size_t target = HoveredTextTarget();
    if (target == static_cast<size_t>(-1)) {
        tool_.textShadow = !tool_.textShadow;
        return;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[target];
    annotation.text.shadow = !annotation.text.shadow;
    renderer_.InvalidateTextPixels(annotation.id);
    Draw();
}

void ClipWindow::ChooseOutlineColor(HWND owner) noexcept {
    if (document_ == nullptr) {
        return;
    }
    POINT screen = lastCursor_;
    ::ClientToScreen(hwnd_, &screen);

    // The piece being pointed at takes it; failing that, it is what the next
    // piece will be given.
    const size_t target = HoveredTextTarget();
    const ccl::doc::Color original =
        target != static_cast<size_t>(-1)
            ? document_->Annotations()[target].text.outlineColor
            : tool_.textOutlineColor;

    ccl::doc::TextAnnotation before;
    if (target != static_cast<size_t>(-1)) {
        before = document_->Annotations()[target].text;
    }

    ++suppressCommitDepth_;
    ColorPopup popup;
    const auto chosen = popup.Show(
        owner != nullptr ? owner : hwnd_, screen, original, tool_.quickColors,
        tool_.RecentColors(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this, target](const ccl::doc::Color& colour) {
            // Shown as it is mixed, without recording a step for every shade
            // the pointer passes over.
            if (target != static_cast<size_t>(-1)) {
                ccl::doc::Annotation& annotation =
                    document_->MutableAnnotations()[target];
                annotation.text.outlineColor = colour;
                renderer_.InvalidateTextPixels(annotation.id);
                Draw();
            }
        });

    if (target != static_cast<size_t>(-1) &&
        target < document_->Annotations().size()) {
        // What the preview painted over is put back, so that accepting records
        // one step and walking away records none.
        document_->MutableAnnotations()[target].text = before;
        if (chosen.has_value()) {
            history_.Record(document_->Annotations(), ToolForHistory());
            ccl::doc::Annotation& annotation =
                document_->MutableAnnotations()[target];
            annotation.text.outlineColor = *chosen;
            renderer_.InvalidateTextPixels(annotation.id);
        }
    } else if (chosen.has_value()) {
        tool_.textOutlineColor = *chosen;
    }

    --suppressCommitDepth_;
    Draw();
}

void ClipWindow::ApplyDecorNumber(NumberKind kind, float value) noexcept {
    if (document_ == nullptr) {
        return;
    }
    // The state the little number box would have put up, so that the apply
    // below settles the range, the target and the undo step exactly as it does
    // for a value typed there.
    numberKind_ = kind;
    numberTarget_ = HoveredTextTarget();
    numberId_ = numberTarget_ != static_cast<size_t>(-1)
                    ? document_->Annotations()[numberTarget_].id
                    : 0;
    const NumberField& field = FieldFor(kind);
    const float wanted = std::clamp(value, field.low, field.high);

    // What the row holds now. Leaving a box alone and moving on is not a
    // change: without this, walking the focus round the panel would leave
    // steps behind that undo to the very same picture.
    float now = 0.0f;
    if (numberTarget_ != static_cast<size_t>(-1)) {
        const ccl::doc::TextAnnotation& text =
            document_->Annotations()[numberTarget_].text;
        switch (kind) {
            case NumberKind::FontSize: now = text.fontSize; break;
            case NumberKind::OutlineWidth: now = text.outlineWidth; break;
            case NumberKind::ShadowLength: now = text.shadowLength; break;
            case NumberKind::ShadowOpacity:
                now = text.shadowColor.a * 100.0f;
                break;
            default: break;
        }
    } else {
        switch (kind) {
            case NumberKind::FontSize: now = CurrentTextSize(); break;
            case NumberKind::OutlineWidth: now = tool_.textOutlineWidth; break;
            case NumberKind::ShadowLength: now = tool_.textShadowLength; break;
            case NumberKind::ShadowOpacity:
                now = tool_.textShadowColor.a * 100.0f;
                break;
            default: break;
        }
    }
    if (std::fabs(now - wanted) < 0.05f) {
        return;
    }

    // ApplyNumber puts the value in but records nothing: the box it belongs to
    // records once, when it is dismissed, so that walking a value up and down
    // does not fill the history. A row on the panel is settled the moment it
    // is left, so the step is recorded here instead.
    if (numberTarget_ != static_cast<size_t>(-1)) {
        history_.Record(document_->Annotations(), ToolForHistory());
    }
    ApplyNumber(wanted);
}

void ClipWindow::OpenDecorPanel() noexcept {
    if (document_ == nullptr || hwnd_ == nullptr || editor_ != nullptr) {
        return;
    }
    EndNumberEntry(true);

    POINT screen = lastCursor_;
    ::ClientToScreen(hwnd_, &screen);

    // Read afresh after every change: what a value settled at is decided by
    // the apply, not by what was typed, and a colour is chosen behind the
    // panel's back.
    const auto gather = [this]() {
        ccl::ui::DecorValues values;
        const size_t target = HoveredTextTarget();
        if (target != static_cast<size_t>(-1)) {
            const ccl::doc::TextAnnotation& text =
                document_->Annotations()[target].text;
            values.outline = text.outline;
            values.outlineWidth = text.outlineWidth;
            values.outlineColor = text.outlineColor;
            values.shadow = text.shadow;
            values.shadowLength = text.shadowLength;
            values.shadowDirection = text.shadowDirection;
            values.shadowColor = text.shadowColor;
            values.shadowOpacity = text.shadowColor.a * 100.0f;
        } else {
            values.outline = tool_.textOutline;
            values.outlineWidth = tool_.textOutlineWidth;
            values.outlineColor = tool_.textOutlineColor;
            values.shadow = tool_.textShadow;
            values.shadowLength = tool_.textShadowLength;
            values.shadowDirection = tool_.textShadowDirection;
            values.shadowColor = tool_.textShadowColor;
            values.shadowOpacity = tool_.textShadowColor.a * 100.0f;
        }
        return values;
    };

    // Held still while the panel is up, so that what it acts on cannot change
    // under it, and so that the outline round that piece stays where it is.
    decorOpen_ = true;
    ++suppressCommitDepth_;

    ccl::ui::DecorPanel panel;
    panel.Show(
        hwnd_, screen, gather(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this, &panel, &gather](ccl::ui::DecorPanel::Field field,
                       const ccl::ui::DecorValues& values) {
            using Field = ccl::ui::DecorPanel::Field;
            switch (field) {
                case Field::Outline: ToggleTextOutline(); break;
                case Field::Shadow: ToggleTextShadow(); break;
                case Field::OutlineWidth:
                    ApplyDecorNumber(NumberKind::OutlineWidth,
                                     values.outlineWidth);
                    break;
                case Field::ShadowLength:
                    ApplyDecorNumber(NumberKind::ShadowLength,
                                     values.shadowLength);
                    break;
                case Field::ShadowOpacity:
                    ApplyDecorNumber(NumberKind::ShadowOpacity,
                                     values.shadowOpacity);
                    break;
                case Field::ShadowDirection:
                    SetShadowDirection(values.shadowDirection);
                    break;
                default: break;
            }
            // Toggling and stepping do not redraw when they act on what the
            // next piece will be given, since nothing on the picture changed.
            Draw();
            panel.Refresh(gather());
        },
        [this, &panel, &gather](ccl::ui::DecorPanel::Field field, HWND owner) {
            using Field = ccl::ui::DecorPanel::Field;
            if (field == Field::OutlineColor) {
                ChooseOutlineColor(owner);
            } else {
                ChooseShadowColor(owner);
            }
            panel.Refresh(gather());
        });

    --suppressCommitDepth_;
    decorOpen_ = false;
    UpdateTitle();
    Draw();
}

void ClipWindow::SetShadowDirection(int way) noexcept {
    const size_t target = HoveredTextTarget();
    if (target == static_cast<size_t>(-1)) {
        tool_.textShadowDirection = way;
        return;
    }

    history_.Record(document_->Annotations(), ToolForHistory());
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[target];
    annotation.text.shadowDirection = way;
    // The glyphs are unchanged; only where the shadow lands.
    renderer_.InvalidateTextPixels(annotation.id);
    Draw();
}

void ClipWindow::ChooseShadowColor(HWND owner) noexcept {
    if (document_ == nullptr) {
        return;
    }
    POINT screen = lastCursor_;
    ::ClientToScreen(hwnd_, &screen);

    // The piece being pointed at takes it; failing that, it is what the next
    // piece will be given.
    const size_t target = HoveredTextTarget();
    const ccl::doc::Color original =
        target != static_cast<size_t>(-1)
            ? document_->Annotations()[target].text.shadowColor
            : tool_.textShadowColor;

    ccl::doc::TextAnnotation before;
    if (target != static_cast<size_t>(-1)) {
        before = document_->Annotations()[target].text;
    }

    // How strong the shadow is stays where it was: the palette mixes a colour
    // and has nowhere to show a strength, which is why the two are set apart.
    const auto keepStrength = [original](ccl::doc::Color colour) {
        colour.a = original.a;
        return colour;
    };

    ++suppressCommitDepth_;
    ColorPopup popup;
    const auto chosen = popup.Show(
        owner != nullptr ? owner : hwnd_, screen, original, tool_.quickColors,
        tool_.RecentColors(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this, target, keepStrength](const ccl::doc::Color& colour) {
            // Shown as it is mixed, without recording a step for every shade
            // the pointer passes over.
            if (target != static_cast<size_t>(-1)) {
                ccl::doc::Annotation& annotation =
                    document_->MutableAnnotations()[target];
                annotation.text.shadowColor = keepStrength(colour);
                renderer_.InvalidateTextPixels(annotation.id);
                Draw();
            }
        });

    if (target != static_cast<size_t>(-1) &&
        target < document_->Annotations().size()) {
        // What the preview painted over is put back, so that accepting records
        // one step and walking away records none.
        document_->MutableAnnotations()[target].text = before;
        if (chosen.has_value()) {
            history_.Record(document_->Annotations(), ToolForHistory());
            ccl::doc::Annotation& annotation =
                document_->MutableAnnotations()[target];
            annotation.text.shadowColor = keepStrength(*chosen);
            renderer_.InvalidateTextPixels(annotation.id);
        }
    } else if (chosen.has_value()) {
        tool_.textShadowColor = keepStrength(*chosen);
    }

    --suppressCommitDepth_;
    Draw();
}

void ClipWindow::PaintText(size_t index, const ccl::doc::Color& colour) noexcept {
    if (document_ == nullptr || index >= document_->Annotations().size()) {
        return;
    }
    ccl::doc::Annotation& annotation = document_->MutableAnnotations()[index];
    ccl::doc::TextAnnotation& text = annotation.text;
    text.color = colour;
    // Ranges hold a colour of their own with no way to say "the one above", so
    // they are painted too rather than left behind.
    for (ccl::doc::TextRun& run : text.runs) {
        run.color = colour;
    }
    // The glyphs are unchanged; only the pixels drawn from them.
    renderer_.InvalidateTextPixels(annotation.id);
}

void ClipWindow::ResizeHoveredText(int steps) noexcept {
    if (document_ == nullptr ||
        hoveredTextIndex_ >= document_->Annotations().size()) {
        return;
    }

    ccl::doc::Annotation& annotation =
        document_->MutableAnnotations()[hoveredTextIndex_];
    if (annotation.kind != ccl::doc::AnnotationKind::Text) {
        return;
    }

    const float size = SteppedFontSize(annotation.text.fontSize, steps);
    if (size == annotation.text.fontSize) {
        return;
    }

    // Recorded once for a run of presses, the way an effect's strength is: what
    // undo returns to is the size it had before the fiddling started.
    if (resizingTextId_ != annotation.id) {
        history_.Record(document_->Annotations(), ToolForHistory());
        resizingTextId_ = annotation.id;
    }

    annotation.text.fontSize = size;
    // Ranges with a size of their own are stepped from that size, so a word
    // made bigger than the rest stays bigger by the same proportion. Ranges
    // carrying zero follow the size above and need no stepping.
    for (ccl::doc::TextRun& run : annotation.text.runs) {
        if (run.fontSize > 0.0f) {
            run.fontSize = SteppedFontSize(run.fontSize, steps);
        }
    }

    // The glyphs in the cache were laid out at the old size.
    renderer_.InvalidateText(annotation.id);
    Draw();
}

// A number typed where the menu item was. Not a window of its own: a small box
// on the capture window, the same way the text being typed is a box on it.
//
// What it changes is shown as it is typed, on the text itself at its real size.
// Enter keeps it, Escape puts back what was there, and clicking away keeps it --
// the same as finishing a piece of text.
LRESULT CALLBACK ClipWindow::NumberProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR,
                                        DWORD_PTR reference) {
    auto* self = reinterpret_cast<ClipWindow*>(reference);
    switch (msg) {
        case WM_KEYDOWN:
            if (self != nullptr && wParam == VK_RETURN) {
                self->EndNumberEntry(true);
                return 0;
            }
            if (self != nullptr && wParam == VK_ESCAPE) {
                self->EndNumberEntry(false);
                return 0;
            }
            break;
        // Without this the control beeps at Enter and Escape, which it treats
        // as characters it has no use for.
        case WM_CHAR:
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            if (self != nullptr) {
                self->EndNumberEntry(true);
            }
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

const ClipWindow::NumberField& ClipWindow::FieldFor(NumberKind kind) noexcept {
    // The same bounds the settings file is held to, so a value typed here and
    // one edited there cannot disagree about what is allowed. In the order the
    // kinds are declared.
    static constexpr NumberField kFields[] = {
        {4.0f, 400.0f, true},   // FontSize: different glyphs
        {1.0f, 20.0f, false},   // OutlineWidth
        {2.0f, 20.0f, false},   // ShadowLength
        {1.0f, 100.0f, false},  // ShadowOpacity, in percent
    };
    static_assert(ARRAYSIZE(kFields) == static_cast<size_t>(NumberKind::kCount),
                  "every kind needs a row");
    const size_t at = static_cast<size_t>(kind);
    return kFields[at < ARRAYSIZE(kFields) ? at : 0];
}

void ClipWindow::BeginNumberEntry(NumberKind kind) noexcept {
    if (document_ == nullptr || hwnd_ == nullptr) {
        return;
    }
    EndNumberEntry(true);

    numberKind_ = kind;
    numberTarget_ = HoveredTextTarget();
    numberId_ = 0;

    // Pointing at a piece of text changes that piece. With nothing pointed at,
    // it is what the next piece will be given.
    float value = 0.0f;
    if (numberTarget_ != static_cast<size_t>(-1)) {
        const ccl::doc::TextAnnotation& text =
            document_->Annotations()[numberTarget_].text;
        numberId_ = document_->Annotations()[numberTarget_].id;
        switch (kind) {
            case NumberKind::FontSize: value = text.fontSize; break;
            case NumberKind::OutlineWidth: value = text.outlineWidth; break;
            case NumberKind::ShadowLength: value = text.shadowLength; break;
            case NumberKind::ShadowOpacity:
                value = text.shadowColor.a * 100.0f;
                break;
            default: break;
        }
    } else {
        switch (kind) {
            case NumberKind::FontSize: value = CurrentTextSize(); break;
            case NumberKind::OutlineWidth:
                value = tool_.textOutlineWidth;
                break;
            case NumberKind::ShadowLength:
                value = tool_.textShadowLength;
                break;
            case NumberKind::ShadowOpacity:
                value = tool_.textShadowColor.a * 100.0f;
                break;
            default: break;
        }
    }
    numberBefore_ = value;

    // Where the menu item was: the pointer is still on it, since clicking it is
    // what got here.
    POINT where{};
    ::GetCursorPos(&where);
    ::ScreenToClient(hwnd_, &where);

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const int width = ccl::dpi::Scale(64, dpi);
    const int height = ccl::dpi::Scale(22, dpi);
    const int x = std::clamp(static_cast<int>(where.x), 0,
                             (std::max)(0, static_cast<int>(client.right) - width));
    const int y = std::clamp(static_cast<int>(where.y), 0,
                             (std::max)(0, static_cast<int>(client.bottom) - height));

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    numberBox_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL, x, y,
        width, height, hwnd_, nullptr, instance, nullptr);
    if (numberBox_ == nullptr) {
        return;
    }
    ::SendMessageW(numberBox_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)),
                   TRUE);
    ::SetWindowSubclass(numberBox_, NumberProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

    wchar_t text[16];
    ::swprintf_s(text, L"%d", static_cast<int>(value + 0.5f));
    ::SetWindowTextW(numberBox_, text);
    // Everything selected, so typing replaces rather than appends -- the value
    // is being chosen, not edited a digit at a time.
    ::SendMessageW(numberBox_, EM_SETSEL, 0, -1);
    ::SetFocus(numberBox_);
}

void ClipWindow::ApplyNumber(float value) noexcept {
    if (document_ == nullptr) {
        return;
    }

    if (numberTarget_ == static_cast<size_t>(-1)) {
        // Nothing pointed at, so this is what the next piece of text will be
        // given: the session's own values, none of which reach the settings
        // file.
        switch (numberKind_) {
            case NumberKind::FontSize: tool_.textFontSize = value; break;
            case NumberKind::OutlineWidth:
                tool_.textOutlineWidth = value;
                break;
            case NumberKind::ShadowLength:
                tool_.textShadowLength = value;
                break;
            case NumberKind::ShadowOpacity:
                tool_.textShadowColor.a = value / 100.0f;
                break;
            default: break;
        }
        // The caption carries the size the next piece of text will be given,
        // and said the old one until something else happened to redraw it.
        // Nothing on the picture changes here, so there is nothing to draw.
        UpdateTitle();
        return;
    }
    if (numberTarget_ >= document_->Annotations().size()) {
        return;
    }

    ccl::doc::TextAnnotation& text =
        document_->MutableAnnotations()[numberTarget_].text;
    switch (numberKind_) {
        case NumberKind::FontSize:
            // Ranges carrying a size of their own move in proportion, the way
            // the bracket keys move them, so a word made bigger stays bigger.
            if (text.fontSize > 0.0f && value != text.fontSize) {
                const float ratio = value / text.fontSize;
                for (ccl::doc::TextRun& run : text.runs) {
                    if (run.fontSize > 0.0f) {
                        run.fontSize *= ratio;
                    }
                }
            }
            text.fontSize = value;
            break;
        case NumberKind::OutlineWidth: text.outlineWidth = value; break;
        case NumberKind::ShadowLength: text.shadowLength = value; break;
        case NumberKind::ShadowOpacity:
            text.shadowColor.a = value / 100.0f;
            break;
        default: break;
    }

    // A different size means different glyphs, so what was traced for this
    // piece is no longer what is being asked for. Everything else leaves the
    // glyphs alone and changes only the pixels drawn from them.
    if (FieldFor(numberKind_).reshapes) {
        renderer_.InvalidateText(numberId_);
    } else {
        renderer_.InvalidateTextPixels(numberId_);
    }
    Draw();
}

void ClipWindow::UpdateNumberEntry() noexcept {
    if (numberBox_ == nullptr) {
        return;
    }
    wchar_t text[16]{};
    ::GetWindowTextW(numberBox_, text, ARRAYSIZE(text));
    wchar_t* end = nullptr;
    const long typed = ::wcstol(text, &end, 10);
    if (end == text) {
        return;
    }
    const NumberField& field = FieldFor(numberKind_);
    ApplyNumber(
        std::clamp(static_cast<float>(typed), field.low, field.high));
}

void ClipWindow::EndNumberEntry(bool keep) noexcept {
    if (numberBox_ == nullptr) {
        return;
    }

    const HWND box = numberBox_;
    // Cleared first: taking the box down moves the focus, which comes back
    // through here.
    numberBox_ = nullptr;
    ::RemoveWindowSubclass(box, NumberProc, 1);
    ::DestroyWindow(box);

    if (!keep) {
        ApplyNumber(numberBefore_);
        ::SetFocus(hwnd_);
        return;
    }

    // Put back, record, then apply again. A step recorded while the new value
    // was already showing would return to a size the text never really had.
    if (numberTarget_ != static_cast<size_t>(-1) && document_ != nullptr &&
        numberTarget_ < document_->Annotations().size()) {
        const ccl::doc::TextAnnotation& text =
            document_->Annotations()[numberTarget_].text;
        float chosen = numberBefore_;
        switch (numberKind_) {
            case NumberKind::FontSize: chosen = text.fontSize; break;
            case NumberKind::OutlineWidth: chosen = text.outlineWidth; break;
            case NumberKind::ShadowLength: chosen = text.shadowLength; break;
            case NumberKind::ShadowOpacity:
                chosen = text.shadowColor.a * 100.0f;
                break;
            default: break;
        }
        if (chosen != numberBefore_) {
            ApplyNumber(numberBefore_);
            history_.Record(document_->Annotations(), ToolForHistory());
            ApplyNumber(chosen);
        }
    }
    ::SetFocus(hwnd_);
}

std::vector<ccl::doc::TextRun> ClipWindow::ReadRuns(int length) noexcept {
    std::vector<ccl::doc::TextRun> runs;
    if (editor_ == nullptr || length <= 0) {
        return runs;
    }

    // Preferred, because it does not disturb the selection. Measured at 0.12ms
    // for 500 characters against 311ms for the walk below, which the control
    // answered by repainting itself once per character.
    if (ReadRunsByTom(length, runs)) {
        return runs;
    }
    return ReadRunsBySelection(length);
}

bool ClipWindow::ReadRunsByTom(int length,
                               std::vector<ccl::doc::TextRun>& runs) noexcept {
    if (editorDoc_ == nullptr || length <= 0) {
        return false;
    }

    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const float zoom = std::max(0.01f, view_.Zoom());

    std::vector<ccl::doc::TextRun> found;
    ccl::doc::TextRun current{};
    bool open = false;

    // Read one character at a time and join neighbours that match. Asking a
    // range to widen itself to the run it sits in looked cheaper, but it
    // returned overlapping ranges near the end of the text once there were
    // line breaks, and this is fast enough as it is.
    for (int i = 0; i < length; ++i) {
        ITextRange* range = nullptr;
        if (FAILED(editorDoc_->Range(i, i + 1, &range)) || range == nullptr) {
            return false;
        }

        ITextFont* font = nullptr;
        if (FAILED(range->GetFont(&font)) || font == nullptr) {
            range->Release();
            return false;
        }

        float points = 0.0f;
        long colour = 0;
        long bold = 0;
        long italic = 0;
        long underline = 0;
        long strikethrough = 0;
        font->GetSize(&points);
        font->GetForeColor(&colour);
        font->GetBold(&bold);
        font->GetItalic(&italic);
        font->GetUnderline(&underline);
        font->GetStrikeThrough(&strikethrough);

        BSTR face = nullptr;
        std::wstring family;
        if (SUCCEEDED(font->GetName(&face)) && face != nullptr) {
            family = face;
            ::SysFreeString(face);
        }
        font->Release();
        range->Release();

        ccl::doc::TextRun style{};
        // Text with no colour of its own reports "automatic" rather than a
        // colour, and a range covering more than one style reports "undefined".
        // Either would come out as a nonsense colour if stored as it stands, so
        // both fall back to what asking through the selection would have given.
        style.color =
            FromColorRef((colour == tomAutoColor || colour == tomUndefined)
                             ? RGB(0, 0, 0)
                             : static_cast<COLORREF>(colour));
        style.fontFamily = family;
        // Sizes arrive in points; the rest of the code works in the pixels the
        // text will be drawn at, so go through twips as the selection path does.
        style.fontSize =
            TwipsToPixels(static_cast<LONG>(std::lround(points * 20.0f)), dpi) /
            zoom;
        // Underline answers with the kind of line rather than a yes or no, so
        // only its presence is taken.
        style.bold = bold != 0;
        style.italic = italic != 0;
        style.underline = underline != 0;
        style.strikethrough = strikethrough != 0;

        if (open && current.SameStyle(style)) {
            ++current.length;
            continue;
        }
        if (open) {
            found.push_back(current);
        }
        current = style;
        current.start = static_cast<unsigned int>(i);
        current.length = 1;
        open = true;
    }
    if (open) {
        found.push_back(current);
    }

    runs = std::move(found);
    return true;
}

std::vector<ccl::doc::TextRun> ClipWindow::ReadRunsBySelection(
    int length) noexcept {
    std::vector<ccl::doc::TextRun> runs;
    if (editor_ == nullptr || length <= 0) {
        return runs;
    }

    CHARRANGE saved{};
    ::SendMessageW(editor_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));

    // With nothing selected the control holds a format waiting for the next
    // character typed, and moving the selection about throws it away. Since
    // this walk happens every time the box is refitted -- which is after every
    // keystroke -- a size chosen before typing would never survive to be used.
    // Read here, put back at the end. The mask comes back naming only the
    // attributes the selection agrees on, so putting it back changes nothing.
    CHARFORMAT2W pending{};
    pending.cbSize = sizeof(pending);
    pending.dwMask = CFM_COLOR | CFM_SIZE | CFM_FACE | CFM_BOLD | CFM_ITALIC |
                     CFM_UNDERLINE | CFM_STRIKEOUT;
    ::SendMessageW(editor_, EM_GETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&pending));

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
    ::SendMessageW(editor_, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&pending));
    return runs;
}

void ClipWindow::ApplyParagraphFormat(
    const ccl::doc::TextAnnotation& shown) noexcept {
    if (editor_ == nullptr || editorDoc_ == nullptr) {
        return;
    }

    // Each line gets the height the drawing is going to give it. Pinning the
    // whole box to one spacing -- the largest size anywhere in it -- was the
    // earlier attempt: it kept typed and drawn text on the same lines, but a
    // box with one large word in it spaced every other line out to match, and
    // the height was still worked out from the natural spacing, so the last
    // lines fell outside the box and could not be seen.
    std::vector<float> heights;
    if (!renderer_.MeasureLines(shown, heights) || heights.empty()) {
        return;
    }

    // Nothing to do while the lines still want what they were last given. This
    // runs after every keystroke, and setting it costs about the same whether
    // there are five lines or a hundred.
    if (heights == pinnedLineHeights_) {
        return;
    }
    pinnedLineHeights_ = heights;

    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const float zoom = view_.Zoom();
    const int length = ::GetWindowTextLengthW(editor_);

    // Walked through the Text Object Model, which leaves the selection where it
    // is. Selecting each paragraph to format it would move the caret about on
    // every keystroke, and the control repaints itself when it does.
    long position = 0;
    size_t line = 0;
    int guard = 0;
    while (position < length && line < heights.size() && guard++ < 4096) {
        ITextRange* range = nullptr;
        if (FAILED(editorDoc_->Range(position, position, &range)) ||
            range == nullptr) {
            break;
        }

        long delta = 0;
        range->Expand(tomParagraph, &delta);
        long from = position;
        long to = position;
        range->GetStart(&from);
        range->GetEnd(&to);
        if (to <= position) {
            range->Release();
            break;
        }

        ITextPara* para = nullptr;
        if (SUCCEEDED(range->GetPara(&para)) && para != nullptr) {
            // Rich edit indents paragraphs and spaces them apart of its own
            // accord; both have to go, or typed text sits somewhere other than
            // where it will be drawn.
            para->SetSpaceBefore(0.0f);
            para->SetSpaceAfter(0.0f);
            para->SetIndents(0.0f, 0.0f, 0.0f);
            // Spacing is asked for in points, while the height came back in the
            // pixels the picture will draw at.
            const float points =
                heights[line] * zoom * 72.0f / static_cast<float>(dpi);
            para->SetLineSpacing(tomLineSpaceExactly, points);
            para->Release();
        }
        range->Release();

        ++line;
        position = to;
    }
}

ccl::doc::TextAnnotation ClipWindow::EditorSnapshot() noexcept {
    ccl::doc::TextAnnotation snapshot;
    if (editor_ == nullptr || settings_ == nullptr) {
        return snapshot;
    }

    const int length = ::GetWindowTextLengthW(editor_);
    if (length > 0) {
        std::wstring raw(static_cast<size_t>(length) + 1, L'\0');
        ::GetWindowTextW(editor_, raw.data(), length + 1);
        raw.resize(static_cast<size_t>(length));
        snapshot.text = ToStoredLineEndings(raw);
        // Read in the control's coordinates, then moved onto the stored ones,
        // so that the ranges line up with the text they are describing.
        snapshot.runs = MoveRuns(ReadRuns(length), StoredOffsets(raw));
    }

    snapshot.fontSize =
        editingExisting_ ? editingOriginal_.fontSize : CurrentTextSize();
    snapshot.fontFamily = CurrentTextFont();
    snapshot.color = tool_.Color();
    snapshot.bold = tool_.textBold;
    snapshot.italic = tool_.textItalic;
    snapshot.underline = tool_.textUnderline;
    snapshot.strikethrough = tool_.textStrikethrough;
    return snapshot;
}

void ClipWindow::ResizeEditor() noexcept {
    if (editor_ == nullptr || settings_ == nullptr) {
        return;
    }

    // Measured the way the picture will draw it, with the same code, rather
    // than by asking the control. Asking was measured and found to answer
    // wrongly in both directions: the height it wants never changes, however
    // large the letters are, because the line pitch is pinned; and the width it
    // wants is its own width plus a constant, so every keystroke made the box a
    // little wider than the last.
    const ccl::doc::TextAnnotation shown = EditorSnapshot();

    ApplyParagraphFormat(shown);

    // The largest size anywhere in the box. The lines are spaced individually
    // now, but this still decides how much room to leave around the text.
    float tallest = shown.fontSize;
    for (const ccl::doc::TextRun& run : shown.runs) {
        tallest = std::max(tallest, run.fontSize);
    }

    // An empty box still needs room for one line and the caret.
    ccl::doc::TextAnnotation probe = shown;
    if (probe.text.empty()) {
        probe.text = L"W";
        probe.runs.clear();
        probe.fontSize = tallest;
    }

    D2D1_RECT_F bounds{};
    if (!renderer_.MeasureText(probe, bounds)) {
        return;
    }

    const float zoom = view_.Zoom();
    const int margin =
        std::max(4, static_cast<int>(std::lround(tallest * zoom * 0.35f)));
    const int width = std::max(
        40, static_cast<int>(std::lround((bounds.right - bounds.left) * zoom)) +
                margin);
    const int height = std::max(
        12, static_cast<int>(std::lround((bounds.bottom - bounds.top) * zoom)) +
                margin);

    RECT current{};
    ::GetWindowRect(editor_, &current);
    const int currentWidth = current.right - current.left;
    const int currentHeight = current.bottom - current.top;
    if (width == currentWidth && height == currentHeight) {
        return;
    }

    ::SetWindowPos(editor_, nullptr, 0, 0, width, height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Only a shrink exposes image that the parent has to repaint; growing is
    // covered by the control itself.
    if (width < currentWidth || height < currentHeight) {
        Draw();
    }
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
    // Released before the window it belongs to goes away.
    if (editorDoc_ != nullptr) {
        editorDoc_->Release();
        editorDoc_ = nullptr;
    }
    if (editor_ != nullptr) {
        ::DestroyWindow(editor_);
        editor_ = nullptr;
    }
    // The next box starts unpinned, whatever this one settled on.
    pinnedLineHeights_.clear();
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
        // refer to the control's own character indices -- and then moved onto
        // the stored ones, since a line break stops taking two characters.
        runs = MoveRuns(ReadRuns(static_cast<int>(::wcslen(raw.c_str()))),
                        StoredOffsets(raw));
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
        history_.Record(document_->Annotations(), ToolForHistory());
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
        annotation.text.fontSize = CurrentTextSize();
        annotation.text.fontFamily = CurrentTextFont();
    }

    annotation.text.color = tool_.Color();
    annotation.text.bold = tool_.textBold;
    annotation.text.italic = tool_.textItalic;
    annotation.text.underline = tool_.textUnderline;
    annotation.text.strikethrough = tool_.textStrikethrough;
    // Left as the piece already had them when it is being reopened. These two
    // are switched from outside the box, on the text being pointed at, so the
    // box has no say in them: taking them from the current value here would
    // undo, on commit, whatever had been set from outside.
    if (!wasEditing) {
        annotation.text.shadow = tool_.textShadow;
        annotation.text.outline = tool_.textOutline;
        annotation.text.outlineWidth = tool_.textOutlineWidth;
        annotation.text.shadowLength = tool_.textShadowLength;
        annotation.text.shadowDirection = tool_.textShadowDirection;
        annotation.text.shadowColor = tool_.textShadowColor;
        annotation.text.outlineColor = tool_.textOutlineColor;
    }
    annotation.text.runs = std::move(runs);

    document_->MutableAnnotations().push_back(std::move(annotation));
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

const ccl::doc::Stroke* ClipWindow::RecentStroke() const noexcept {
    if (drawing_) {
        return &activeStroke_;
    }
    if (document_ == nullptr ||
        recentStrokeIndex_ >= document_->Annotations().size()) {
        return nullptr;
    }
    const ccl::doc::Annotation& annotation =
        document_->Annotations()[recentStrokeIndex_];
    return annotation.kind == ccl::doc::AnnotationKind::Stroke
               ? &annotation.stroke
               : nullptr;
}

// The same, for the callers that go on to change what they are given. Kept
// apart from the reading one so that merely asking whether there is a head to
// turn does not count as having changed the picture.
ccl::doc::Stroke* ClipWindow::MutableRecentStroke() noexcept {
    if (drawing_) {
        return &activeStroke_;
    }
    if (document_ == nullptr ||
        recentStrokeIndex_ >= document_->Annotations().size()) {
        return nullptr;
    }
    ccl::doc::Annotation& annotation =
        document_->MutableAnnotations()[recentStrokeIndex_];
    return annotation.kind == ccl::doc::AnnotationKind::Stroke
               ? &annotation.stroke
               : nullptr;
}

void ClipWindow::ForgetRecentStroke() noexcept {
    recentStrokeIndex_ = static_cast<size_t>(-1);
}

bool ClipWindow::HasAdjustableArrow() const noexcept {
    const ccl::doc::Stroke* stroke = RecentStroke();
    return stroke != nullptr && !stroke->arrows.empty();
}

void ClipWindow::InsertArrowhead() noexcept {
    ccl::doc::Stroke* stroke = MutableRecentStroke();
    // Nothing to point along: the first point of a line has nothing behind it.
    if (stroke == nullptr || stroke->points.size() < 2) {
        return;
    }

    const auto at = static_cast<unsigned int>(stroke->points.size() - 1);

    // Pressing again without having moved would stack a second head on the
    // first, which only makes the edges harsher.
    if (!stroke->arrows.empty() && stroke->arrows.back().at == at) {
        return;
    }

    // A line already finished is being edited, so it is a step of its own.
    // One still being drawn is not: it goes into the history whole when the
    // button comes up.
    if (!drawing_ && document_ != nullptr) {
        history_.Record(document_->Annotations(), ToolForHistory());
        // Recorded before the change, so the pointer has to be taken again.
        stroke = MutableRecentStroke();
        if (stroke == nullptr) {
            return;
        }
    }

    ccl::doc::StrokeArrow arrow;
    arrow.at = at;
    stroke->arrows.push_back(arrow);

    // The size keys were pointed at an effect; they are not now.
    adjustingEffectIndex_ = static_cast<size_t>(-1);
    UpdateTitle();
    Draw();
}

void ClipWindow::TurnArrowhead(int steps) noexcept {
    ccl::doc::Stroke* stroke = MutableRecentStroke();
    if (stroke == nullptr || stroke->arrows.empty() || settings_ == nullptr) {
        return;
    }

    // Not a step of its own, as adjusting an effect just placed is not: undo
    // takes back the head, or the line, rather than the nudges to it.
    stroke->arrows.back().turn +=
        static_cast<float>(steps) * settings_->arrowTurnDegrees;
    UpdateTitle();
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
        history_.Record(document_->Annotations(), ToolForHistory());

        ccl::doc::Annotation annotation;
        annotation.id = ccl::doc::NextAnnotationId();
        annotation.kind = ccl::doc::AnnotationKind::Stroke;
        annotation.stroke = std::move(activeStroke_);
        document_->MutableAnnotations().push_back(std::move(annotation));

        // Kept in reach: a line is often finished before it is clear that it
        // wanted an arrow on the end, and having to draw it again for that
        // would be a poor answer.
        recentStrokeIndex_ = document_->Annotations().size() - 1;
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
        const ccl::doc::Annotation& annotation = annotations[i];
        if (annotation.kind == ccl::doc::AnnotationKind::Stroke &&
            StrokeHit(annotation.stroke, point, radius)) {
            victims.push_back(i);
            continue;
        }
        if (annotation.kind != ccl::doc::AnnotationKind::Area) {
            continue;
        }
        const ccl::doc::AreaAnnotation& area = annotation.area;
        if (area.width > 0.0f) {
            // A line round an area is rubbed out by touching the line, as any
            // other line is -- reaching for the middle of a large ring and
            // having it disappear would be the surprise.
            if (ccl::doc::SelectionNearEdge(area.shape, point.x, point.y,
                                            radius + area.width * 0.5f)) {
                victims.push_back(i);
            }
            continue;
        }
        // Paint goes as a whole rather than being worn away. Taken at the
        // centre of the eraser, not across its width: brushing the edge of a
        // large area of colour and having all of it vanish would be a shock.
        if (ccl::doc::SelectionContains(area.shape, point.x, point.y)) {
            victims.push_back(i);
        }
    }
    if (victims.empty()) {
        return;
    }

    // One undo entry per erase drag, not per stroke removed.
    if (!erasedAny_) {
        history_.Record(annotations, ToolForHistory());
        erasedAny_ = true;
    }

    // Taken only now that there is something to take out. Asked for on every
    // move of the eraser, this would count as a change whether or not anything
    // was rubbed out.
    auto& mutableAnnotations = document_->MutableAnnotations();
    for (size_t i = victims.size(); i > 0; --i) {
        mutableAnnotations.erase(mutableAnnotations.begin() +
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
    // Pressing a button is doing something else, whatever it turns out to be.
    // Only moving the mouse leaves the line just drawn still in reach.
    ForgetRecentStroke();

    // Alt reaches for the eyedropper without leaving the current tool, matching
    // the shortcut image editors use. It is the same eyedropper the key opens
    // -- magnifier, the whole screen to sample from, all of it -- and letting
    // go of the button hands the tool back.
    //
    // The press has already happened by the time the hook goes up, so the hook
    // only ever sees the movement and the release. That is enough: the release
    // is what settles the colour.
    //
    // Not while selecting an area: there Alt means "take this piece away
    // again", which is the one place a colour cannot be used anyway -- nothing
    // there draws with it.
    if (IsKeyDown(VK_MENU) && tool_.tool != ccl::tool::Tool::Eyedropper &&
        !IsSelectionTool(tool_.tool) && !IsObjectTool(tool_.tool)) {
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
        case ccl::tool::Tool::Lasso: {
            // Placing a new selection ends adjustment of the previous effect.
            adjustingEffectIndex_ = static_cast<size_t>(-1);

            // Kept before anything is thrown away, so that the drag can be
            // stepped back over whichever kind it turns out to be.
            selectionBeforeDrag_ = selection_;

            // Shift adds to what is there, Alt takes away, neither starts
            // again. Ctrl is left out of it: it can be assigned to scrolling
            // or to moving the window, and taking it here would quietly
            // override that for this tool alone.
            ccl::doc::SelectionOp op = ccl::doc::SelectionOp::Replace;
            if (IsKeyDown(VK_SHIFT)) {
                op = ccl::doc::SelectionOp::Add;
            } else if (IsKeyDown(VK_MENU)) {
                op = ccl::doc::SelectionOp::Subtract;
            } else {
                // Starting again: the settled pieces go, rather than being
                // left underneath where they would pile up unseen.
                selection_.clear();
            }

            const D2D1_POINT_2F start = ToImage(client);
            pending_ = ccl::doc::SelectionShape{};
            pending_.op = op;
            if (tool_.tool == ccl::tool::Tool::Lasso) {
                pending_.lasso = true;
                // Back to the fine spacing for each new lasso: the last one
                // having been long says nothing about this one.
                lassoSpacing_ = kLassoSpacing;
                pending_.points.push_back({start.x, start.y});
            } else {
                pending_.left = start.x;
                pending_.top = start.y;
                pending_.right = start.x;
                pending_.bottom = start.y;
            }
            selecting_ = true;
            RefreshSelection();

            ::SetCapture(hwnd_);
            Draw();
            return;
        }

        case ccl::tool::Tool::ObjectSelect:
        case ccl::tool::Tool::ObjectLasso: {
            // The same band the area tools drag out, and the same modifiers.
            // What differs is what happens when it is let go of: here it picks
            // out the pieces it reached rather than settling as an area.
            ccl::doc::SelectionOp op = ccl::doc::SelectionOp::Replace;
            if (IsKeyDown(VK_SHIFT)) {
                op = ccl::doc::SelectionOp::Add;
            } else if (IsKeyDown(VK_MENU)) {
                op = ccl::doc::SelectionOp::Subtract;
            }

            const D2D1_POINT_2F start = ToImage(client);

            // Everything a press here would reach, from the top down. Pressing
            // the same place again works its way down this, so a piece
            // underneath can be got at without moving the one on top out of
            // the way first.
            std::vector<size_t> stacked;
            ObjectsAt(start, stacked);

            const auto holds = [](const std::vector<unsigned int>& list,
                                  unsigned int id) {
                return std::find(list.begin(), list.end(), id) != list.end();
            };

            // Same place, close enough in time to read as one gesture rather
            // than two separate presses, and asking for the same thing: then
            // this press carries the walk on. Anything else starts a new one.
            // The interval is the one set for double clicks, since that is the
            // same judgement being made.
            const ULONGLONG when = ::GetTickCount64();
            const bool samePlace =
                std::abs(client.x - cycleAt_.x) <= kClickThreshold &&
                std::abs(client.y - cycleAt_.y) <= kClickThreshold;
            const bool soon = when - cycleWhen_ <= ::GetDoubleClickTime();

            if (samePlace && soon && op == cycleOp_ && !cycleTargets_.empty()) {
                ++cycleDepth_;
            } else {
                cycleDepth_ = 0;
                cycleOp_ = op;
                cycleBase_ = pickedIds_;
                cycleTargets_.clear();
                if (document_ != nullptr) {
                    for (size_t index : stacked) {
                        const unsigned int id =
                            document_->Annotations()[index].id;
                        // Shift walks the pieces it could add, Alt those it
                        // could take out. Neither touches what the other one
                        // is for, so what was picked before the walk began
                        // stays exactly as it was.
                        const bool candidate =
                            op == ccl::doc::SelectionOp::Replace ||
                            (op == ccl::doc::SelectionOp::Add
                                 ? !holds(cycleBase_, id)
                                 : holds(cycleBase_, id));
                        if (candidate) {
                            cycleTargets_.push_back(id);
                        }
                    }
                }
            }
            cycleAt_ = client;
            cycleWhen_ = when;

            const unsigned int target =
                cycleTargets_.empty()
                    ? 0u
                    : cycleTargets_[cycleDepth_ % cycleTargets_.size()];

            // Pressing on something takes hold of it. Having to pick a piece
            // out first and then press it again to move it would be two
            // actions for what reads as one -- and pressing on a piece can
            // mean nothing else here.
            //
            // Only without a modifier: Shift and Alt are how the picking is
            // adjusted, and a band drawn from on top of a piece is a perfectly
            // ordinary way to reach for its neighbours.
            if (op == ccl::doc::SelectionOp::Replace && target != 0) {
                // Not picked yet: pressing it picks it, and nothing else,
                // which is what pressing a thing means everywhere else in the
                // program. Having worked down to it settles on it either way
                // -- otherwise reaching a piece already in the set would
                // change nothing and the walk would have nothing to show for
                // itself.
                if (cycleDepth_ != 0 || !holds(pickedIds_, target)) {
                    const std::vector<unsigned int> before = pickedIds_;
                    pickedIds_.assign(1, target);
                    RecordPickedChange(before);
                    UpdateTitle();
                }
                BeginPickedDrag(client);
                Draw();
                return;
            }

            // With a modifier the press may still turn out to be a click on
            // one piece rather than the start of a band. Which it is cannot be
            // told yet, so the piece it would act on is remembered and the band
            // is begun as usual; the release decides between them.
            bandClickId_ = 0;
            bandClickStart_ = client;
            if (op != ccl::doc::SelectionOp::Replace) {
                bandClickId_ = target;
            }

            pending_ = ccl::doc::SelectionShape{};
            pending_.op = op;
            if (tool_.tool == ccl::tool::Tool::ObjectLasso) {
                pending_.lasso = true;
                lassoSpacing_ = kLassoSpacing;
                pending_.points.push_back({start.x, start.y});
            } else {
                pending_.left = start.x;
                pending_.top = start.y;
                pending_.right = start.x;
                pending_.bottom = start.y;
            }
            selecting_ = true;
            RefreshSelection();

            ::SetCapture(hwnd_);
            Draw();
            return;
        }

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
        // Once the pointer has actually travelled, the press was the start of
        // a band after all, and the piece it landed on stops being the answer.
        if (bandClickId_ != 0 &&
            (std::abs(client.x - bandClickStart_.x) > kClickThreshold ||
             std::abs(client.y - bandClickStart_.y) > kClickThreshold)) {
            bandClickId_ = 0;
        }

        if (pending_.lasso) {
            ExtendLasso(client);
        } else {
            const D2D1_POINT_2F at = ToImage(client);
            pending_.right = at.x;
            pending_.bottom = at.y;
        }
        RefreshSelection();
        Draw();
        return;
    }

    if (movingPicked_) {
        ContinuePickedDrag(client);
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
            history_.Record(document_->Annotations(), ToolForHistory());
        }

        if (document_ != nullptr &&
            movingTextIndex_ < document_->Annotations().size()) {
            const float zoom = view_.Zoom();
            auto& text =
                document_->MutableAnnotations()[movingTextIndex_].text;
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
    if (tool_.tool == ccl::tool::Tool::Text && editor_ == nullptr &&
        !decorOpen_ && !fontPickerOpen_) {
        const size_t hovered = FindTextAt(ToImage(client));
        if (hovered != hoveredTextIndex_) {
            hoveredTextIndex_ = hovered;
            // Pointing at something else ends the run of size steps, so the
            // next one starts a fresh undo step.
            resizingTextId_ = 0;
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
    if (movingPicked_) {
        EndPickedDrag();
        return;
    }

    if (selecting_ && IsObjectTool(tool_.tool)) {
        // Settled before the capture is let go of, as the area tools do: the
        // release sends WM_CAPTURECHANGED straight back here, and that path
        // drops a drag still marked as running.
        const ccl::doc::SelectionOp op = pending_.op;
        // Taken before either path touches it: what is picked is a step of its
        // own, so stepping back once returns the previous set rather than
        // reaching past it to the last thing drawn.
        //
        // For a walk this is the set as the last step of it left things, not
        // as the walk began. Each press of a walk is its own step: reaching
        // one piece too far should cost one press back, which is the whole
        // point of walking down in the first place.
        const std::vector<unsigned int> beforePick = pickedIds_;
        if (bandClickId_ != 0) {
            // Pressed on a piece with a modifier and let go without moving:
            // that one piece is what was meant, not the band of no size the
            // drag would otherwise settle.
            //
            // Back to how the walk found things before acting. On its first
            // step that changes nothing; on later ones it is what takes back
            // the step before, so one piece is added or taken out at a time
            // rather than the stack being gathered up press by press.
            pickedIds_ = cycleBase_;
            PickOne(bandClickId_, op);
        } else {
            ApplyObjectBand(op);
            // A band settles somewhere of its own, so there is no walk left to
            // carry on from.
            cycleWhen_ = 0;
            cycleTargets_.clear();
        }
        RecordPickedChange(beforePick);
        bandClickId_ = 0;
        selecting_ = false;
        pending_ = ccl::doc::SelectionShape{};
        ::ReleaseCapture();
        RefreshSelection();
        UpdateTitle();
        UpdateCursor();
        Draw();
        return;
    }

    if (selecting_) {
        // Settled before the capture is let go of, not after: releasing it
        // sends WM_CAPTURECHANGED straight back here, and that path drops a
        // drag still in progress. Reaching it with the drag still marked as
        // running threw the area away the instant it was finished.
        selection_ = CurrentShapes();
        selecting_ = false;
        pending_ = ccl::doc::SelectionShape{};
        ::ReleaseCapture();
        RefreshSelection();

        // A click without a drag settles on nothing, rather than leaving an
        // area with no size behind.
        if (!HasSelection()) {
            ClearSelection();
        }

        // A step of its own, every time: whichever kind of drag it was, whether
        // or not anything was selected before it, and whether or not the result
        // looks any different. The press was made, so it belongs in the record
        // of what was done -- which of them are worth stepping back over is not
        // for this to decide.
        if (document_ != nullptr) {
            history_.RecordSelection(document_->Annotations(),
                                     selectionBeforeDrag_, ToolForHistory());
        }
        selectionBeforeDrag_.clear();

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

    const ccl::app::Command command = settings_->shortcuts.Lookup(pressed);

    // Every command except putting a head on it counts as having done
    // something else, which takes R and the arrow keys off the line just drawn.
    if (command != ccl::app::Command::InsertArrowhead) {
        ForgetRecentStroke();
    }

    switch (command) {
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
        case ccl::app::Command::ToolLasso:
            SelectTool(ccl::tool::Tool::Lasso);
            return true;
        // These arrive at the tool and stay there, like every other tool key.
        // They used to hand the tool back when pressed a second time, so that
        // reaching for a piece in the middle of drawing had a way home; but
        // the two ways of selecting look alike enough from the keyboard that
        // the second press was as often meant as "get me in there" -- and it
        // took the user out instead. Leaving is what the other tool keys are
        // for, and they were never ambiguous.
        case ccl::app::Command::ToolObjectSelect:
            SelectTool(ccl::tool::Tool::ObjectSelect);
            return true;
        case ccl::app::Command::ToolObjectLasso:
            SelectTool(ccl::tool::Tool::ObjectLasso);
            return true;
        // Quiet unless something is picked, which is what ReorderPicked checks.
        // Taken here rather than only in the picking tools: what is picked is
        // let go of on the way out of them, so there is nothing to act on
        // anywhere else anyway.
        case ccl::app::Command::ObjectRaise:
            ReorderPicked(1, false);
            return true;
        case ccl::app::Command::ObjectLower:
            ReorderPicked(-1, false);
            return true;
        case ccl::app::Command::ObjectToFront:
            ReorderPicked(1, true);
            return true;
        case ccl::app::Command::ObjectToBack:
            ReorderPicked(-1, true);
            return true;
        case ccl::app::Command::ObjectRotate:
            RotatePicked();
            return true;
        // Routed through the menu commands so there is one path to each of
        // these, whether it was reached by key or by menu. Each does nothing
        // without an area selected, which is the whole of the condition: an
        // area only exists while a tool that selects is in use.
        case ccl::app::Command::FillSelection:
            OnCommand(kMenuFill);
            return true;
        case ccl::app::Command::FillSelectionMarker:
            OnCommand(kMenuFillMarker);
            return true;
        case ccl::app::Command::OutlineSelection:
            OnCommand(kMenuOutline);
            return true;
        case ccl::app::Command::OutlineSelectionMarker:
            OnCommand(kMenuOutlineMarker);
            return true;
        case ccl::app::Command::Mosaic:
            OnCommand(kMenuMosaic);
            return true;
        case ccl::app::Command::Blur:
            OnCommand(kMenuBlur);
            return true;
        case ccl::app::Command::InsertArrowhead:
            InsertArrowhead();
            return true;
        case ccl::app::Command::TextDecor:
            OpenDecorPanel();
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
    // A modifier on its own is not doing something else -- it is the first
    // half of doing something. Left to fall through, reaching for Ctrl would
    // take the arrow keys off the line just drawn before the arrow arrived.
    if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU) {
        return;
    }

    // Turning the head just placed. Taken before anything else so that Ctrl
    // with an up or down arrow means this while there is a head to turn, and
    // goes back to scrolling the rest of the time.
    if (IsKeyDown(VK_CONTROL) && (key == VK_UP || key == VK_DOWN) &&
        HasAdjustableArrow()) {
        // Up turns it the way up looks: anticlockwise, since the picture's
        // vertical runs downwards.
        TurnArrowhead(key == VK_DOWN ? 1 : -1);
        return;
    }

    if (RunShortcut(key)) {
        return;
    }

    // Anything else that is a key is something else being done, which is what
    // takes R and the arrow keys off the line just drawn.
    ForgetRecentStroke();

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

            // Text under the pointer takes them too, for the same reason: what
            // is being pointed at, and outlined to say so, is what they act on.
            if (tool_.tool == ccl::tool::Tool::Text && editor_ == nullptr &&
                hoveredTextIndex_ != static_cast<size_t>(-1)) {
                ResizeHoveredText(steps);
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
            // While a head can still be turned, that is what the arrow keys
            // are for, so it is what gets reported.
            if (HasAdjustableArrow()) {
                ::swprintf_s(title,
                             L"%s  %d%%  %s %.0fpx  (Ctrl+↑ ↓ で矢印の向き)",
                             name.c_str(), zoom,
                             tool_.highlighter ? L"Marker" : L"Pen",
                             tool_.Width());
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
                         CurrentTextSize());
            break;
        case ccl::tool::Tool::ObjectSelect:
        case ccl::tool::Tool::ObjectLasso: {
            const wchar_t* pickName =
                tool_.tool == ccl::tool::Tool::ObjectLasso ? L"ObjectLasso"
                                                           : L"Objects";
            if (pickedIds_.empty()) {
                ::swprintf_s(title, L"%s  %d%%  %s", name.c_str(), zoom,
                             pickName);
            } else {
                ::swprintf_s(title, L"%s  %d%%  %s %d", name.c_str(), zoom,
                             pickName, static_cast<int>(pickedIds_.size()));
            }
            break;
        }

        case ccl::tool::Tool::Select:
        case ccl::tool::Tool::Lasso: {
            // Which of the two is in use, since neither has a cursor of its
            // own to say so.
            const wchar_t* selectName =
                tool_.tool == ccl::tool::Tool::Lasso ? L"Lasso" : L"Select";

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
            } else if (SelectionIsSingleRect()) {
                const D2D1_RECT_F area = SelectionBounds();
                ::swprintf_s(title, L"%s  %d%%  %s %.0f x %.0f", name.c_str(),
                             zoom, selectName, area.right - area.left,
                             area.bottom - area.top);
            } else if (HasSelection()) {
                // Several pieces have no one width and height to report, so
                // how much is covered is what gets said instead.
                ::swprintf_s(title, L"%s  %d%%  %s %.0fpx", name.c_str(), zoom,
                             selectName, selectionGeometry_.Area());
            } else {
                ::swprintf_s(title, L"%s  %d%%  %s", name.c_str(), zoom,
                             selectName);
            }
            break;
        }
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

    // Pointing at a piece of text makes it the target: the colour lands on it
    // rather than on whatever is typed next. What it looked like beforehand is
    // kept so that dragging across the gradient can be undone by walking away.
    const size_t target = HoveredTextTarget();
    ccl::doc::TextAnnotation before;
    if (target != static_cast<size_t>(-1)) {
        before = document_->Annotations()[target].text;
    }

    // Same reason as the styling menu: the palette takes focus, and that must
    // not end the edit in progress.
    ++suppressCommitDepth_;

    ColorPopup popup;
    const auto chosen = popup.Show(
        hwnd_, screen, original, tool_.quickColors, tool_.RecentColors(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100,
        [this, target](const ccl::doc::Color& colour) {
            // Applied without recording it: dragging across a gradient would
            // otherwise fill the recent list with every shade passed over.
            tool_.SetColor(colour);

            // While text is being edited the palette's own swatch is the
            // preview. Pushing every intermediate colour into the control and
            // repainting behind it made dragging crawl and the palette flicker;
            // the colour is applied once, on commit.
            if (editor_ == nullptr) {
                // A piece of text being pointed at is repainted as the colour
                // moves, which costs nothing extra: this path already redraws
                // the window for every shade passed over.
                if (target != static_cast<size_t>(-1)) {
                    PaintText(target, colour);
                }
                Draw();
            }
        });

    if (target != static_cast<size_t>(-1) &&
        target < document_->Annotations().size()) {
        // Put back what the preview painted over. Accepting then repaints it
        // for good, with the state before the drag recorded first, so the whole
        // business is one step to undo and a walk away leaves no step at all.
        document_->MutableAnnotations()[target].text = before;
        if (chosen.has_value()) {
            history_.Record(document_->Annotations(), ToolForHistory());
            PaintText(target, *chosen);
        }
    }

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
    //
    // Losing it is a step, though. Without one, coming back to the tool leaves
    // nothing to act on and no way to get it back except by drawing it again --
    // and the marks made with it cannot be redone over the same area at all.
    // The switch itself is still not a step: only what it does to the area is.
    if (!IsSelectionTool(tool) && !eyedropperAside) {
        if (!selection_.empty() && document_ != nullptr) {
            history_.RecordSelection(document_->Annotations(), selection_,
                                     ToolForHistory());
        }
        ClearSelection();
    }

    // What is picked out follows the same rule as the selected area: it belongs
    // to the tools that pick, and is let go of on the way out. Not recorded as
    // a step, unlike the area -- an area cannot be drawn again over the same
    // place, but the same pieces can be picked again by pressing on them.
    if (!IsObjectTool(tool) && !eyedropperAside) {
        ClearPicked();
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

std::wstring ClipWindow::FontInForce() const noexcept {
    // The piece being pointed at, or the selection in the box, or what the next
    // piece will be given -- the same order the change itself follows, so what
    // is shown as current is what a change would replace.
    const size_t pointed = HoveredTextTarget();
    if (pointed != static_cast<size_t>(-1)) {
        return document_->Annotations()[pointed].text.fontFamily;
    }
    if (editor_ != nullptr) {
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_FACE;
        ::SendMessageW(editor_, EM_GETCHARFORMAT, SCF_SELECTION,
                       reinterpret_cast<LPARAM>(&format));
        // What comes back in dwMask is what the selection agrees on. Across two
        // faces the flag is cleared while szFaceName still holds one of them,
        // so reading the name without testing the flag reports a face that only
        // part of the selection is in. Nothing is in force there.
        if ((format.dwMask & CFM_FACE) == 0) {
            return {};
        }
        return format.szFaceName;
    }
    return CurrentTextFont();
}

void ClipWindow::ApplyFontChoice(const std::wstring& family) noexcept {
    // The piece being pointed at takes it; failing that, the box being typed
    // into; failing that, whatever is typed next.
    if (!RefontHoveredText(family)) {
        SetTextFont(family);
    }
}

void ClipWindow::OpenFontPicker() noexcept {
    if (context_ == nullptr || hwnd_ == nullptr) {
        return;
    }
    EndNumberEntry(true);

    POINT screen = lastCursor_;
    ::ClientToScreen(hwnd_, &screen);

    // Held still while the window is up, for the same reason the decoration
    // panel holds it: walking the pointer past the window would otherwise move
    // what the choice lands on.
    fontPickerOpen_ = true;
    // The window takes focus off the editor, which must not be mistaken for
    // clicking away and end the edit.
    ++suppressCommitDepth_;

    ccl::ui::FontPicker picker;
    const std::optional<std::wstring> chosen = picker.Show(
        hwnd_, screen, InstalledFonts(context_->Text()), FontInForce(),
        settings_ != nullptr ? settings_->paletteScalePercent : 100);

    --suppressCommitDepth_;
    fontPickerOpen_ = false;

    if (chosen.has_value()) {
        ApplyFontChoice(*chosen);
    }
    if (editor_ != nullptr) {
        ::SetFocus(editor_);
    }
    UpdateTitle();
    Draw();
}

HMENU ClipWindow::BuildFontMenu() noexcept {
    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr || context_ == nullptr) {
        return menu;
    }

    const std::wstring current = FontInForce();
    const auto& fonts = InstalledFonts(context_->Text());
    for (size_t i = 0; i < fonts.size(); ++i) {
        UINT flags = MF_STRING;
        if (fonts[i].shown == current) {
            flags |= MF_CHECKED;
        }
        // Wrapped into columns; the list is long enough to run off screen.
        if (i > 0 && i % kMenuColumnLength == 0) {
            flags |= MF_MENUBARBREAK;
        }
        ::AppendMenuW(menu, flags, kMenuFontBase + static_cast<UINT>(i),
                      fonts[i].shown.c_str());
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
    // The same window the other menu opens, so that which font can be reached
    // does not depend on where the menu was opened from.
    ::AppendMenuW(menu, plain, kMenuFontPick, L"フォント...");
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
    // Offered but not usable from in here. The edge and the shadow are drawn
    // when the text is, not while it is being typed, so switching one now would
    // change nothing on screen; they are changed from outside instead, on the
    // text being pointed at. Left visible rather than removed, so that looking
    // for them finds them, greyed, where they have always been.
    const UINT locked = MF_STRING | MF_GRAYED;
    // Shown with its key, from the bindings rather than written in, so the
    // menu still tells the truth after the key has been reassigned.
    const std::wstring decor =
        L"飾り..." + (settings_ != nullptr
                          ? settings_->shortcuts.MenuSuffix(
                                ccl::app::Command::TextDecor)
                          : std::wstring());
    ::AppendMenuW(menu, locked, kMenuTextDecor, decor.c_str());
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
    // Settled before the menu is built, because some of what goes in it depends
    // on what is being pointed at.
    RefreshHoveredText();

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
    toolEntry(ccl::tool::Tool::Lasso, L"投げ縄", ccl::app::Command::ToolLasso);
    toolEntry(ccl::tool::Tool::ObjectSelect, L"オブジェクト選択",
              ccl::app::Command::ToolObjectSelect);
    toolEntry(ccl::tool::Tool::ObjectLasso, L"オブジェクト投げ縄",
              ccl::app::Command::ToolObjectLasso);
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
                              ccl::app::Command::ToolLasso,
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
    // The same things in the same order as the menu the editing box offers, so
    // that what can be changed about text does not depend on where the menu was
    // opened from. The colour sits at the top level of this menu already, with
    // a key of its own, and is not repeated here.
    // A window rather than a submenu: several hundred families laid out as menu
    // columns fill the screen, and a menu item cannot hold a box to type into.
    ::AppendMenuW(textStyle, MF_STRING, kMenuFontPick, L"フォント...");
    // Outright, rather than by stepping with the bracket keys. Greyed while the
    // box is open: what is being typed is sized by the box, and the piece this
    // would act on is the one being pointed at.
    ::AppendMenuW(textStyle,
                  MF_STRING | (editor_ != nullptr ? MF_GRAYED : 0),
                  kMenuTextSize, L"大きさ...");
    ::AppendMenuW(textStyle, MF_SEPARATOR, 0, nullptr);

    // Pointing at a piece of text makes every one of these read that piece.
    // With nothing pointed at they read what the next piece will be given.
    const size_t styleTarget = HoveredTextTarget();
    const ccl::doc::TextAnnotation* pointed =
        styleTarget != static_cast<size_t>(-1)
            ? &document_->Annotations()[styleTarget].text
            : nullptr;
    const bool isBold = pointed != nullptr ? pointed->bold : tool_.textBold;
    const bool isItalic =
        pointed != nullptr ? pointed->italic : tool_.textItalic;
    const bool isUnderline =
        pointed != nullptr ? pointed->underline : tool_.textUnderline;
    const bool isStruck = pointed != nullptr ? pointed->strikethrough
                                             : tool_.textStrikethrough;

    ::AppendMenuW(textStyle, isBold ? checked : plain, kMenuBold,
                  L"太字\tCtrl+B");
    ::AppendMenuW(textStyle, isItalic ? checked : plain, kMenuItalic,
                  L"斜体\tCtrl+I");
    ::AppendMenuW(textStyle, isUnderline ? checked : plain, kMenuUnderline,
                  L"下線\tCtrl+U");
    ::AppendMenuW(textStyle, isStruck ? checked : plain, kMenuStrikethrough,
                  L"打ち消し線");
    ::AppendMenuW(textStyle, MF_SEPARATOR, 0, nullptr);
    // The edge and the shadow are on the panel, switches and all. They had ten
    // rows between them here once, which made this a list to be read rather
    // than a thing to point at -- and changing two of them meant opening the
    // menu twice.
    //
    // Greyed while the box is open, as those rows were: what is being typed is
    // drawn by the box, which has neither an edge nor a shadow to switch.
    ::AppendMenuW(
        textStyle, MF_STRING | (editor_ != nullptr ? MF_GRAYED : 0),
        kMenuTextDecor,
        withKey(L"飾り...", ccl::app::Command::TextDecor).c_str());
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(textStyle),
                  L"文字\tCtrl+B I U");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Only offered when there is something selected to obscure.
    const HMENU selection = ::CreatePopupMenu();
    const UINT selectionState = HasSelection() ? plain : (plain | MF_GRAYED);
    // Cropping can only produce a rectangle, so it is not offered for an area
    // made of several pieces. Greyed out rather than hidden: the entry moving
    // about would be worse than seeing why it cannot be used.
    const UINT cropState =
        SelectionIsSingleRect() ? plain : (plain | MF_GRAYED);
    // Putting colour down and hiding what is there are different intentions,
    // so they are kept in separate blocks. The order runs from adding, through
    // hiding, to reshaping, to having done with it.
    ::AppendMenuW(
        selection, selectionState, kMenuFill,
        withKey(L"塗りつぶし", ccl::app::Command::FillSelection).c_str());
    ::AppendMenuW(selection, selectionState, kMenuFillMarker,
                  withKey(L"マーカー塗りつぶし",
                          ccl::app::Command::FillSelectionMarker)
                      .c_str());
    ::AppendMenuW(
        selection, selectionState, kMenuOutline,
        withKey(L"境界線を描く", ccl::app::Command::OutlineSelection).c_str());
    ::AppendMenuW(selection, selectionState, kMenuOutlineMarker,
                  withKey(L"マーカーで境界線を描く",
                          ccl::app::Command::OutlineSelectionMarker)
                      .c_str());
    ::AppendMenuW(selection, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(selection, selectionState, kMenuMosaic,
                  withKey(L"モザイク", ccl::app::Command::Mosaic).c_str());
    ::AppendMenuW(selection, selectionState, kMenuBlur,
                  withKey(L"ぼかし", ccl::app::Command::Blur).c_str());
    ::AppendMenuW(selection, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(selection, cropState, kMenuCrop, L"この範囲で切り抜く");
    ::AppendMenuW(selection, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(selection, selectionState, kMenuClearSelection,
                  L"選択を解除");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(selection),
                  L"選択範囲");

    // What was drawn on the picture, rather than an area of it. Kept as a block
    // of its own next to the area one: the two read alike and act on different
    // things, so putting them together is what makes the difference visible.
    const UINT objectState =
        pickedIds_.empty() ? (plain | MF_GRAYED) : plain;
    const HMENU objects = ::CreatePopupMenu();
    ::AppendMenuW(objects, objectState, kMenuObjectRotate,
                  withKey(L"回転...", ccl::app::Command::ObjectRotate).c_str());
    ::AppendMenuW(objects, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(objects, objectState, kMenuObjectRaise,
                  withKey(L"前へ出す", ccl::app::Command::ObjectRaise).c_str());
    ::AppendMenuW(
        objects, objectState, kMenuObjectLower,
        withKey(L"後ろへ送る", ccl::app::Command::ObjectLower).c_str());
    ::AppendMenuW(
        objects, objectState, kMenuObjectToFront,
        withKey(L"最前面へ", ccl::app::Command::ObjectToFront).c_str());
    ::AppendMenuW(
        objects, objectState, kMenuObjectToBack,
        withKey(L"最背面へ", ccl::app::Command::ObjectToBack).c_str());
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(objects),
                  L"オブジェクト");

    // Reshaping the picture. Every one of these burns the annotations in, so
    // they are kept together and away from the tools.
    const HMENU image = ::CreatePopupMenu();
    ::AppendMenuW(image, plain, kMenuRotateLeft, L"左に 90 度回転");
    ::AppendMenuW(image, plain, kMenuRotateRight, L"右に 90 度回転");
    ::AppendMenuW(image, plain, kMenuRotate180, L"180 度回転");
    ::AppendMenuW(image, plain, kMenuRotateFree, L"自由に回転...");
    ::AppendMenuW(image, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(image, plain, kMenuFlipHorizontal, L"左右反転");
    ::AppendMenuW(image, plain, kMenuFlipVertical, L"上下反転");
    ::AppendMenuW(image, MF_SEPARATOR, 0, nullptr);
    const UINT pasteState =
        ccl::io::ClipboardHasImage() ? plain : (plain | MF_GRAYED);
    ::AppendMenuW(image, pasteState, kMenuConcat,
                  L"クリップボードの画像を連結する...");
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
            ApplyFontChoice(fonts[index].shown);
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
        case kMenuRotateFree:
            RotateFreely();
            return;
        case kMenuFlipHorizontal:
            ApplyTransform(ccl::io::FlipHorizontal(FlattenForTransform()));
            return;
        case kMenuFlipVertical:
            ApplyTransform(ccl::io::FlipVertical(FlattenForTransform()));
            return;
        case kMenuConcat:
            ConcatenateClipboard();
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

        // Pointing at a piece of text makes it the target, the way the colour
        // and the size already do. With nothing pointed at, these say what the
        // next piece of text will be given.
        case kMenuBold:
            if (StyleHoveredText(&ccl::doc::TextAnnotation::bold,
                                 &ccl::doc::TextRun::bold)) {
                return;
            }
            tool_.textBold = !tool_.textBold;
            ApplyTextEffect(CFM_BOLD, CFE_BOLD, tool_.textBold);
            return;
        case kMenuItalic:
            if (StyleHoveredText(&ccl::doc::TextAnnotation::italic,
                                 &ccl::doc::TextRun::italic)) {
                return;
            }
            tool_.textItalic = !tool_.textItalic;
            ApplyTextEffect(CFM_ITALIC, CFE_ITALIC, tool_.textItalic);
            return;
        case kMenuUnderline:
            if (StyleHoveredText(&ccl::doc::TextAnnotation::underline,
                                 &ccl::doc::TextRun::underline)) {
                return;
            }
            tool_.textUnderline = !tool_.textUnderline;
            ApplyTextEffect(CFM_UNDERLINE, CFE_UNDERLINE, tool_.textUnderline);
            return;
        case kMenuStrikethrough:
            if (StyleHoveredText(&ccl::doc::TextAnnotation::strikethrough,
                                 &ccl::doc::TextRun::strikethrough)) {
                return;
            }
            tool_.textStrikethrough = !tool_.textStrikethrough;
            ApplyTextEffect(CFM_STRIKEOUT, CFE_STRIKEOUT,
                            tool_.textStrikethrough);
            return;
        case kMenuCommitText:
            CommitText();
            return;
        case kMenuFill:
            PaintSelection(1.0f, 0.0f);
            return;
        case kMenuFillMarker:
            PaintSelection(ccl::doc::kHighlighterOpacity, 0.0f);
            return;
        case kMenuOutline:
            PaintSelection(1.0f, tool_.Width());
            return;
        case kMenuOutlineMarker:
            PaintSelection(ccl::doc::kHighlighterOpacity, tool_.Width());
            return;
        case kMenuObjectRotate:
            RotatePicked();
            return;
        case kMenuObjectRaise:
            ReorderPicked(1, false);
            return;
        case kMenuObjectLower:
            ReorderPicked(-1, false);
            return;
        case kMenuObjectToFront:
            ReorderPicked(1, true);
            return;
        case kMenuObjectToBack:
            ReorderPicked(-1, true);
            return;
        case kMenuMosaic:
            ApplyEffectToSelection(ccl::doc::EffectKind::Mosaic);
            return;
        case kMenuBlur:
            ApplyEffectToSelection(ccl::doc::EffectKind::Blur);
            return;
        case kMenuClearSelection:
            if (document_ != nullptr) {
                history_.RecordSelection(document_->Annotations(), selection_,
                                         ToolForHistory());
            }
            ClearSelection();
            UpdateTitle();
            Draw();
            return;
        case kMenuEyedropper:
            SelectTool(ccl::tool::Tool::Eyedropper);
            return;
        case kMenuColorPicker:
            ChooseColorFromPicker();
            return;
        case kMenuTextSize:
            BeginNumberEntry(NumberKind::FontSize);
            return;
        case kMenuTextDecor:
            OpenDecorPanel();
            return;
        case kMenuFontPick:
            OpenFontPicker();
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

    // Text still being typed belongs to the picture on its way out: committing
    // it here puts it into whatever is written out and takes the editor off a
    // picture it was never about. Not left to the automatic save, which has
    // several reasons to do nothing at all.
    if (EditingText()) {
        CommitText();
    }

    // The capture on screen is written out first if auto-saving is on, since
    // replacing it discards it. If that write fails the replacement is called
    // off: the picture on screen is still the only copy there is.
    if (!AutoSaveBeforeClosing(Departure::Window)) {
        return;
    }

    *document_ = ccl::doc::Document(std::move(image));

    // Everything tied to the old picture goes with it: its annotations are
    // gone, so its history would restore edits onto a different image.
    history_ = ccl::doc::History{};
    adjustingEffectIndex_ = static_cast<size_t>(-1);
    ClearSelection();
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
    ccl::tool::Tool tool = ToolForHistory();
    if (!history_.Undo(document_->MutableAnnotations(),
                       document_->MutableImage(), selection_, pickedIds_,
                       tool)) {
        return;
    }
    RestoreTool(tool);

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    resizingTextId_ = 0;
    // A step that changed a piece of text or the strength of an effect has just
    // put the earlier value back under the same id, and what was worked out
    // from the later one is still being kept against it. Without this, undoing
    // a size change left the text drawn at the size it had been given.
    renderer_.InvalidateResults();
    // A step may have taken away pieces that were picked out, or put back a
    // list they were never in. Ids are never reused, so anything no longer
    // there is gone for good and is dropped.
    PrunePicked();
    // The step may have put a different area back, and a step that carried
    // none leaves the one in hand alone. Either way the folded shape has to
    // be built again from what is there now.
    RefreshSelection();
    if (reshaped) {
        // The area is not thrown away here: the step carried the one that
        // belonged to the picture it just put back, so it measures against it
        // again.
        renderer_.SetDocument(document_);
        ResizeToImage();
        ClampScroll();
    }
    // Always, not only when the picture changed: what the title reports may
    // have been stepped over too -- the area selected, or the effect the size
    // keys were pointed at.
    UpdateTitle();
    Draw();
}

void ClipWindow::Redo() noexcept {
    if (document_ == nullptr) {
        return;
    }
    const bool reshaped = history_.NextRedoChangesImage();
    ccl::tool::Tool tool = ToolForHistory();
    if (!history_.Redo(document_->MutableAnnotations(),
                       document_->MutableImage(), selection_, pickedIds_,
                       tool)) {
        return;
    }
    RestoreTool(tool);

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    resizingTextId_ = 0;
    // Same as undo: the values under these ids have just been swapped.
    renderer_.InvalidateResults();
    // A step may have taken away pieces that were picked out, or put back a
    // list they were never in. Ids are never reused, so anything no longer
    // there is gone for good and is dropped.
    PrunePicked();
    RefreshSelection();
    if (reshaped) {
        // The area is not thrown away here: the step carried the one that
        // belonged to the picture it just put back, so it measures against it
        // again.
        renderer_.SetDocument(document_);
        ResizeToImage();
        ClampScroll();
    }
    // Always, not only when the picture changed: what the title reports may
    // have been stepped over too -- the area selected, or the effect the size
    // keys were pointed at.
    UpdateTitle();
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

    history_.RecordWithImage(document_->Annotations(), document_->Image(),
                             selection_, ToolForHistory());

    document_->MutableImage() = std::move(transformed);
    document_->MutableAnnotations().clear();

    adjustingEffectIndex_ = static_cast<size_t>(-1);
    hoveredTextIndex_ = static_cast<size_t>(-1);
    resizingTextId_ = 0;
    ClearSelection();
    saved_ = false;

    renderer_.SetDocument(document_);
    ResizeToImage();
    ClampScroll();
    UpdateTitle();
    Draw();
}

void ClipWindow::RotateFreely() noexcept {
    if (document_ == nullptr || !document_->IsValid()) {
        return;
    }
    // Anything still being typed becomes part of the picture first, as it does
    // for every other reshaping.
    if (EditingText()) {
        CommitText();
    }

    // Black for the corners a turn leaves empty, matching what joining two
    // pictures fills with.
    constexpr ccl::doc::Color fill{0.0f, 0.0f, 0.0f, 1.0f};

    // The angle is shown on the picture itself while it is being chosen, at its
    // own size. What falls outside the window is only out of sight -- settling
    // grows the picture to fit its corners.
    const auto degrees =
        ccl::ui::ShowRotateDialog(hwnd_, [this, fill](float angle) {
            renderer_.SetPreviewRotation(angle, fill);
            Draw();
        });

    // The dialog puts the preview back on its way out. Cleared again here
    // because a turn left on screen would outlive the dialog that caused it,
    // and that is not a fault worth risking to save a line.
    renderer_.SetPreviewRotation(0.0f, fill);

    if (!degrees.has_value()) {
        Draw();
        return;
    }

    // Where the middle of the picture sits on the desktop.
    //
    // The middle of the picture, not the middle of the window: the two are
    // different points as soon as the picture is scrolled or larger than what
    // shows. The turn works about the middle of the picture, so that is the
    // point worth keeping still -- it is the one that was just watched staying
    // put while the angle was chosen.
    const auto middleOnScreen = [this]() noexcept {
        POINT point{};
        if (document_ != nullptr) {
            const float zoom = view_.Zoom();
            const POINT scroll = view_.Scroll();
            const auto border = static_cast<float>(BorderWidth());
            point.x = std::lround(document_->Width() * 0.5f * zoom + border) -
                      scroll.x;
            point.y = std::lround(document_->Height() * 0.5f * zoom + border) -
                      scroll.y;
        }
        ::ClientToScreen(hwnd_, &point);
        return point;
    };

    const POINT middleBefore = middleOnScreen();

    ApplyTransform(renderer_.RenderRotated(*degrees, fill, 1.0f));

    if (settings_ == nullptr ||
        settings_->rotateAnchor != ccl::app::RotateAnchor::Center) {
        return;
    }

    // Measured on both sides rather than worked out from how much the picture
    // grew. Growing is capped by the work area, and the scroll offset is
    // re-clamped against the larger picture; either can shift the middle by an
    // amount the size difference on its own does not account for.
    const POINT middleAfter = middleOnScreen();
    const LONG dx = middleAfter.x - middleBefore.x;
    const LONG dy = middleAfter.y - middleBefore.y;
    if (dx == 0 && dy == 0) {
        return;
    }

    RECT frame{};
    ::GetWindowRect(hwnd_, &frame);
    ::SetWindowPos(hwnd_, nullptr, frame.left - dx, frame.top - dy, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ClipWindow::CropToSelection() noexcept {
    // Checked here as well as on the menu entry: a greyed entry can still be
    // reached, and an area of several pieces has no one rectangle to cut to.
    if (!SelectionIsSingleRect()) {
        return;
    }

    const D2D1_RECT_F area = SelectionBounds();
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

    // Held so that what the dialog actually changed can be told from what it
    // merely showed. Anything it left alone keeps the value this session is
    // working with, which is not always what the settings file says.
    const ccl::app::Settings before = *settings_;

    // The window takes focus while it is up, which the editor would otherwise
    // read as clicking away.
    ++suppressCommitDepth_;
    const bool changed = ccl::ui::ShowSettingsDialog(hwnd_, *settings_);
    --suppressCommitDepth_;

    if (!changed) {
        return;
    }

    // Applied where it costs nothing to do so. What is left -- the capture
    // settings -- takes effect the next time it is used.
    renderer_.SetSmoothScaling(settings_->smoothScaling);
    renderer_.SetArrowShape(settings_->arrowScale, settings_->arrowAspect,
                            settings_->arrowRounding);
    view_.SetZoomStepPercent(settings_->zoomStepPercent);

    // The text defaults too. They were seeded when the window opened and left
    // alone after, so changing them here did nothing until the next run -- the
    // settings appeared to be ignored.
    //
    // Only what was actually altered in the dialog is taken. Taking all of it
    // meant that opening the settings and pressing OK put back every value the
    // menu had changed during the session, whether or not the dialog had
    // anything to say about it.
    const auto take = [](auto& session, const auto& now, const auto& was) {
        if (now != was) {
            session = now;
        }
    };
    take(tool_.usePressure, settings_->usePenPressure, before.usePenPressure);
    take(tool_.quickColors, settings_->quickColors, before.quickColors);
    take(tool_.textFontSize, settings_->textFontSize, before.textFontSize);
    take(tool_.textFontFamily, settings_->textFontFamily,
         before.textFontFamily);
    take(tool_.textShadow, settings_->textShadow, before.textShadow);
    take(tool_.textOutline, settings_->textOutline, before.textOutline);
    take(tool_.textOutlineWidth, settings_->textOutlineWidth,
         before.textOutlineWidth);
    take(tool_.textShadowLength, settings_->textShadowLength,
         before.textShadowLength);
    take(tool_.textShadowDirection, settings_->textShadowDirection,
         before.textShadowDirection);
    take(tool_.textShadowColor, settings_->ShadowColor(), before.ShadowColor());
    take(tool_.textOutlineColor, settings_->textOutlineColor,
         before.textOutlineColor);

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

void ClipWindow::ConcatenateClipboard() noexcept {
    if (document_ == nullptr) {
        return;
    }
    ccl::capture::DibBuffer addition = ccl::io::PasteFromClipboard(hwnd_);
    if (!addition.IsValid()) {
        return;
    }

    // Flattened once, before anything is shown. The annotations do not change
    // while the placement is being chosen, so remaking this for every nudge
    // would be the same work over and over.
    const ccl::capture::DibBuffer flat = FlattenForTransform();
    if (!flat.IsValid()) {
        return;
    }

    // Black for the space neither picture reaches, matching what a free turn
    // fills its corners with.
    constexpr ccl::doc::Color fill{0.0f, 0.0f, 0.0f, 1.0f};

    // What the document holds now, kept aside so the preview can work on the
    // real thing. Showing the join on the picture itself is the only way to
    // judge it -- the two end up side by side, so anything small enough to sit
    // in the dialog would be too small to see.
    ccl::capture::DibBuffer originalImage = std::move(document_->MutableImage());
    ccl::doc::AnnotationList originalAnnotations = document_->Annotations();

    const auto restore = [&]() {
        document_->MutableImage() = originalImage.Clone();
        document_->MutableAnnotations() = originalAnnotations;
        renderer_.InvalidateResults();
        renderer_.SetDocument(document_);
        ResizeToImage();
        ClampScroll();
        Draw();
    };

    // Where the picture already open sits inside the joined one. Nudging the
    // added picture up or to the left grows the result that way, which moves
    // the other one along inside it -- so the window is moved back by as much,
    // and what was already there stays put on the desktop. Without this the
    // picture being judged against slides away with every press of a key.
    int heldX = 0;
    int heldY = 0;
    const auto keepOriginalStill = [&](const ccl::io::ConcatLayout& layout) {
        const int dx = layout.sourceX - heldX;
        const int dy = layout.sourceY - heldY;
        heldX = layout.sourceX;
        heldY = layout.sourceY;
        if (dx == 0 && dy == 0) {
            return;
        }
        RECT bounds{};
        if (!::GetWindowRect(hwnd_, &bounds)) {
            return;
        }
        const float zoom = view_.Zoom();
        ::SetWindowPos(hwnd_, nullptr,
                       bounds.left - std::lround(dx * zoom),
                       bounds.top - std::lround(dy * zoom), 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Every change rebuilds the joined picture and puts it in place. No step is
    // recorded for any of it: the walk through possible placements is not
    // twenty edits, it is one, and it is recorded when it settles.
    const auto preview = [&](const ccl::io::ConcatPlacement& placement) {
        ccl::capture::DibBuffer joined =
            ccl::io::Concatenate(flat, addition, placement, fill);
        if (!joined.IsValid()) {
            return;
        }
        document_->MutableImage() = std::move(joined);
        document_->MutableAnnotations().clear();
        renderer_.InvalidateResults();
        renderer_.SetDocument(document_);
        ResizeToImage();
        keepOriginalStill(ccl::io::PlanConcat(flat.Width(), flat.Height(),
                                              addition.Width(),
                                              addition.Height(), placement));
        ClampScroll();
        Draw();
    };

    ccl::io::ConcatPlacement start;
    start.margin = settings_ != nullptr
                       ? static_cast<int>(settings_->concatMargin)
                       : 0;

    const auto chosen = ccl::ui::ShowConcatDialog(hwnd_, start, preview);

    // Back to how things were either way, so that settling goes through the
    // ordinary path: it is what records the step, and it has to record the
    // state the picture was in before any of this.
    restore();
    heldX = 0;
    heldY = 0;
    if (!chosen.has_value()) {
        return;
    }
    ApplyTransform(ccl::io::Concatenate(flat, addition, *chosen, fill));
    // Settling moves the picture inside the result the same way a preview did,
    // so the window is brought back the same way. Otherwise pressing OK would
    // shift the whole thing at the last moment, after it had been held still
    // for the entire time the placement was being chosen.
    keepOriginalStill(ccl::io::PlanConcat(flat.Width(), flat.Height(),
                                          addition.Width(), addition.Height(),
                                          *chosen));
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

bool ClipWindow::AutoSaveBeforeClosing(Departure departure) noexcept {
    if (saved_ || discarding_ || context_ == nullptr || document_ == nullptr ||
        settings_ == nullptr) {
        return true;
    }
    // Nothing to fail at, and nothing to warn about either: an empty folder is
    // how auto-saving is switched off.
    if (settings_->autoSaveFolder.empty()) {
        return true;
    }
    // Holding Shift while closing skips the automatic save -- but only for the
    // window in front, the one whose closing the hand on the keyboard is about.
    // Closing a whole group from the taskbar sends the same message to every
    // window at once, and Shift is how that menu is reached, so honouring it
    // there would throw away every capture on the screen.
    if (IsKeyDown(VK_SHIFT) && ::GetForegroundWindow() == hwnd_) {
        return true;
    }

    if (EditingText()) {
        CommitText();
    }

    const ccl::capture::DibBuffer flat = renderer_.Flatten();
    const bool written =
        !ccl::io::AutoSaveImage(
             *context_, flat.IsValid() ? flat : document_->Image(), *settings_,
             sourceTitle_,
             departure == Departure::SessionEnd ? ccl::io::HistoryCleanup::Skip
                                                : ccl::io::HistoryCleanup::Prune)
             .empty();
    if (written) {
        // Counts as saved, so a close arriving after the session-end write does
        // not put a second copy of the same picture on disk.
        saved_ = true;
        return true;
    }

    if (departure == Departure::SessionEnd) {
        return true;
    }

    // Losing the capture without a word is the one outcome worth interrupting
    // for: the window is the only place the picture still exists.
    ::MessageBoxW(hwnd_,
                  L"自動保存に失敗しました。保存先を確認してください。\n"
                  L"画像を失わないよう、ウィンドウは閉じません。",
                  L"CapturaClipA2", MB_ICONWARNING | MB_OK);
    return false;
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

    ID2D1Geometry* selection = nullptr;
    ID2D1Geometry* removing = nullptr;
    if (IsSelectionTool(tool_.tool)) {
        if (HasSelection()) {
            selection = selectionGeometry_.Get();
        }
        removing = removingGeometry_.Get();
    } else if (IsObjectTool(tool_.tool) && selecting_) {
        // Only while the band is being dragged out. Nothing settles here, so
        // there is no shape to keep showing once the button comes up.
        selection = selectionGeometry_.Get();
        removing = removingGeometry_.Get();
    }

    // Only the ids go across. The renderer already keeps each piece's shape
    // against its id, and marks it along that shape rather than round its box.
    const bool marking = IsObjectTool(tool_.tool) && !pickedIds_.empty();

    if (tool_.tool == ccl::tool::Tool::Text &&
        editor_ == nullptr &&
        document_ != nullptr &&
        hoveredTextIndex_ < document_->Annotations().size()) {
        const auto& annotation = document_->Annotations()[hoveredTextIndex_];
        if (annotation.kind == ccl::doc::AnnotationKind::Text) {
            hasHighlight = renderer_.MeasureText(annotation.text, highlight);
        }
    }

    // Green while the object tools are in hand, so the band being dragged says
    // which of the two kinds of selecting it is before it settles into either.
    const D2D1_COLOR_F objectBand = D2D1::ColorF(0.30f, 0.85f, 0.40f, 0.9f);
    const bool objectBanding = IsObjectTool(tool_.tool);

    renderer_.Draw(view_, drawing_ ? &activeStroke_ : nullptr,
                   showCursor ? &cursor : nullptr,
                   hasHighlight ? &highlight : nullptr, selection, removing,
                   objectBanding ? &objectBand : nullptr,
                   marking ? pickedIds_.data() : nullptr,
                   marking ? pickedIds_.size() : 0, GrabSlack());

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
    renderer_.SetArrowShape(settings.arrowScale, settings.arrowAspect,
                            settings.arrowRounding);
    renderer_.SetBorderWidth(BorderWidth());
    tool_.usePressure = settings.usePenPressure;
    tool_.textFontSize = settings.textFontSize;
    tool_.textFontFamily = settings.textFontFamily;
    tool_.textShadow = settings.textShadow;
    tool_.textOutline = settings.textOutline;
    tool_.textOutlineWidth = settings.textOutlineWidth;
    tool_.textShadowLength = settings.textShadowLength;
    tool_.textShadowDirection = settings.textShadowDirection;
    tool_.textShadowColor = settings.ShadowColor();
    tool_.textOutlineColor = settings.textOutlineColor;
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

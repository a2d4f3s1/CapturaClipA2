#include "ui/RotateDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"CapturaClipA2.RotateDialog";

// Logical (96-DPI) layout.
constexpr int kMargin = 12;
constexpr int kRowHeight = 24;
constexpr int kRowGap = 8;
constexpr int kLabelWidth = 80;
constexpr int kNumberWidth = 64;
constexpr int kButtonWidth = 88;
constexpr int kDialogWidth = 360;

// A whole turn either way. Past that the picture only repeats.
constexpr int kMinDegrees = -360;
constexpr int kMaxDegrees = 360;

enum ControlId : UINT {
    kIdSlider = 100,
    kIdNumber,
};

struct State {
    const std::function<void(float)>* notify = nullptr;

    HFONT font = nullptr;
    UINT dpi = ccl::dpi::kDefaultDpi;

    HWND window = nullptr;
    HWND slider = nullptr;
    HWND number = nullptr;

    int degrees = 0;
    // Set while the slider and the number box are being brought into line with
    // each other. Without it, writing to one raises a change from the other and
    // the two chase each other round.
    bool syncing = false;

    bool accepted = false;
    bool finished = false;
};

HFONT CreateMessageFont(UINT dpi) noexcept {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                      &metrics, 0, dpi)) {
        return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    return ::CreateFontIndirectW(&metrics.lfMessageFont);
}

HWND Add(State& state, const wchar_t* className, const wchar_t* text,
         DWORD style, int x, int y, int width, int height, UINT id) noexcept {
    const HWND control = ::CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        state.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(
            ::GetWindowLongPtrW(state.window, GWLP_HINSTANCE)),
        nullptr);
    if (control != nullptr) {
        // Without this a control comes up in the old system font.
        ::SendMessageW(control, WM_SETFONT,
                       reinterpret_cast<WPARAM>(state.font), TRUE);
    }
    return control;
}

// How tall a stretch of text needs to be at this width. Going by line count
// gets it wrong as soon as a line wraps, and the overflow is simply cut off.
int MeasuredHeight(const State& state, const wchar_t* text, int width) noexcept {
    const int single = ccl::dpi::Scale(kRowHeight, state.dpi);
    const HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        return single;
    }
    const HGDIOBJ previous = ::SelectObject(screen, state.font);
    RECT bounds{0, 0, width, 0};
    ::DrawTextW(screen, text, -1, &bounds,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(screen, previous);
    ::ReleaseDC(nullptr, screen);
    // A couple of pixels over, so a descender on the last line is not shaved
    // off by the edge of the control.
    return (std::max)(single, static_cast<int>(bounds.bottom - bounds.top) + 2);
}

// `writeNumber` is false while the number box is what changed: writing the
// value back would move the caret out from under whoever is typing.
void SetDegrees(State& state, int degrees, bool writeNumber) noexcept {
    degrees = std::clamp(degrees, kMinDegrees, kMaxDegrees);
    if (degrees == state.degrees) {
        return;
    }
    state.degrees = degrees;

    state.syncing = true;
    ::SendMessageW(state.slider, TBM_SETPOS, TRUE, state.degrees);
    if (writeNumber) {
        wchar_t text[16];
        ::swprintf_s(text, L"%d", state.degrees);
        ::SetWindowTextW(state.number, text);
    }
    state.syncing = false;

    if (state.notify != nullptr && *state.notify) {
        (*state.notify)(static_cast<float>(state.degrees));
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state =
        reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_HSCROLL:
            if (state != nullptr && !state->syncing &&
                reinterpret_cast<HWND>(lParam) == state->slider) {
                SetDegrees(*state,
                           static_cast<int>(::SendMessageW(state->slider,
                                                           TBM_GETPOS, 0, 0)),
                           true);
            }
            return 0;

        case WM_COMMAND: {
            if (state == nullptr) {
                break;
            }
            const UINT id = LOWORD(wParam);
            if (id == kIdNumber && HIWORD(wParam) == EN_CHANGE &&
                !state->syncing) {
                wchar_t text[32]{};
                ::GetWindowTextW(state->number, text, ARRAYSIZE(text));
                wchar_t* end = nullptr;
                const long value = ::wcstol(text, &end, 10);
                if (end != text) {
                    SetDegrees(*state, static_cast<int>(value), false);
                }
                return 0;
            }
            if (id == IDOK) {
                // Enter arrives here as well as the button, so the angle is
                // checked here rather than only by disabling the button.
                if (state->degrees != 0) {
                    state->accepted = true;
                }
                state->finished = true;
                return 0;
            }
            if (id == IDCANCEL) {
                state->finished = true;
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (state != nullptr) {
                state->finished = true;
            }
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterDialogClass(HINSTANCE instance) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_3DFACE);
    wc.lpszClassName = kWindowClass;
    return ::RegisterClassExW(&wc) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

std::optional<float> ShowRotateDialog(
    HWND owner, const std::function<void(float)>& onAngleChanged) noexcept {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!RegisterDialogClass(instance)) {
        return std::nullopt;
    }

    // The slider comes from the common controls, and asking for one without
    // this leaves nothing for the window to create.
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    State state;
    state.notify = &onAngleChanged;
    state.dpi = ccl::dpi::ForWindow(owner);
    state.font = CreateMessageFont(state.dpi);

    const auto scaled = [&state](int logical) {
        return ccl::dpi::Scale(logical, state.dpi);
    };

    const int margin = scaled(kMargin);
    const int rowHeight = scaled(kRowHeight);
    const int gap = scaled(kRowGap);
    const int width = scaled(kDialogWidth);

    // WS_EX_CONTROLPARENT lets Tab move between the controls; WS_CLIPCHILDREN
    // keeps this window's own painting out of their area.
    state.window = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kWindowClass, L"自由に回転",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, width,
        scaled(200), owner, nullptr, instance, &state);
    if (state.window == nullptr) {
        ::DeleteObject(state.font);
        return std::nullopt;
    }

    int y = margin;

    Add(state, L"STATIC", L"角度 (度)", SS_LEFT, margin, y + scaled(4),
        scaled(kLabelWidth), rowHeight, 0);
    state.number =
        Add(state, L"EDIT", L"0", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
            width - margin - scaled(kNumberWidth), y, scaled(kNumberWidth),
            rowHeight, kIdNumber);
    y += rowHeight + gap;

    state.slider = Add(state, TRACKBAR_CLASSW, L"",
                       TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, margin, y,
                       width - 2 * margin, rowHeight, kIdSlider);
    ::SendMessageW(state.slider, TBM_SETRANGE, TRUE,
                   MAKELPARAM(kMinDegrees, kMaxDegrees));
    ::SendMessageW(state.slider, TBM_SETPOS, TRUE, 0);
    y += rowHeight + gap;

    const wchar_t* note =
        L"ウィンドウに結果が出ます。はみ出して見えなくなる所も、"
        L"決めたときには切れずに残ります。\n"
        L"任意の角度では画像を作り直すため少しぼやけます。"
        L"繰り返すほど失われるので、回すのは一度で決めてください。"
        L"90 度・180 度はメニューの項目を使えば劣化しません。";
    const int noteWidth = width - 2 * margin;
    const int noteHeight = MeasuredHeight(state, note, noteWidth);
    Add(state, L"STATIC", note, SS_LEFT, margin, y, noteWidth, noteHeight, 0);
    y += noteHeight + gap;

    const int buttonWidth = scaled(kButtonWidth);
    Add(state, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
        width - margin - 2 * buttonWidth - gap, y, buttonWidth, rowHeight,
        IDOK);
    Add(state, L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP,
        width - margin - buttonWidth, y, buttonWidth, rowHeight, IDCANCEL);
    y += rowHeight + margin;

    // Sized to what was laid out, then centred on the pointer. This is reached
    // from the menu, so the pointer is where the eye already is. Covering the
    // picture is fine -- the dialog can be dragged aside.
    RECT frame{0, 0, width, y};
    ::AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                         WS_EX_DLGMODALFRAME);
    const int outerWidth = frame.right - frame.left;
    const int outerHeight = frame.bottom - frame.top;

    POINT cursor{};
    if (!::GetCursorPos(&cursor)) {
        RECT ownerBounds{};
        ::GetWindowRect(owner, &ownerBounds);
        cursor = POINT{ownerBounds.left, ownerBounds.top};
    }

    int x = cursor.x - outerWidth / 2;
    int top = cursor.y - outerHeight / 2;
    if (const HMONITOR monitor =
            ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        monitor != nullptr) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(monitor, &info)) {
            // Kept on the monitor the pointer is on, so a dialog opened near an
            // edge slides back into view rather than hanging off it.
            x = std::clamp(x, static_cast<int>(info.rcWork.left),
                           static_cast<int>(info.rcWork.right) - outerWidth);
            top = std::clamp(top, static_cast<int>(info.rcWork.top),
                             static_cast<int>(info.rcWork.bottom) - outerHeight);
        }
    }
    ::SetWindowPos(state.window, HWND_TOP, x, top, outerWidth, outerHeight,
                   SWP_NOACTIVATE);

    // Modal by hand: the owner is disabled for the duration, and messages are
    // pumped here rather than by the caller's loop.
    ::EnableWindow(owner, FALSE);
    ::ShowWindow(state.window, SW_SHOW);
    ::SetForegroundWindow(state.window);

    MSG msg{};
    while (!state.finished && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(state.window, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    // The preview belongs to this dialog, so it is taken down here rather than
    // left for the caller to remember.
    if (onAngleChanged) {
        onAngleChanged(0.0f);
    }

    // Re-enabled before the window goes, so that focus lands back on the owner
    // rather than on some other application.
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
    ::DestroyWindow(state.window);
    ::DeleteObject(state.font);

    // A WM_QUIT taken out of the queue here would never reach the caller's
    // loop, so it is put back.
    if (msg.message == WM_QUIT) {
        ::PostQuitMessage(static_cast<int>(msg.wParam));
    }

    if (!state.accepted) {
        return std::nullopt;
    }
    return static_cast<float>(state.degrees);
}

}  // namespace ccl::ui

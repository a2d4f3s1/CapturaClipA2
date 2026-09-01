#include "ui/ConcatDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"CapturaClipA2.ConcatDialog";

// Logical (96-DPI) layout.
constexpr int kMargin = 12;
constexpr int kRowHeight = 24;
constexpr int kRowGap = 8;
constexpr int kLabelWidth = 96;
constexpr int kNumberWidth = 64;
constexpr int kButtonWidth = 88;
constexpr int kDialogWidth = 380;

// Enough to push the added picture right off the other one and then some.
// Without a bound a stray digit asks for a picture the size of a wall.
constexpr int kMaxMargin = 2000;
constexpr int kMaxOffset = 20000;

enum ControlId : UINT {
    kIdTop = 100,
    kIdBottom,
    kIdLeft,
    kIdRight,
    kIdMargin,
    kIdOffsetX,
    kIdOffsetY,
    kIdOnTop,
};

// Listed the way they are read out: up, down, left, right.
struct SideButton {
    ccl::io::ConcatSide side;
    UINT id;
    const wchar_t* label;
};

constexpr SideButton kSides[] = {
    {ccl::io::ConcatSide::Top, kIdTop, L"上"},
    {ccl::io::ConcatSide::Bottom, kIdBottom, L"下"},
    {ccl::io::ConcatSide::Left, kIdLeft, L"左"},
    {ccl::io::ConcatSide::Right, kIdRight, L"右"},
};

struct State {
    const std::function<void(const ccl::io::ConcatPlacement&)>* notify = nullptr;

    HFONT font = nullptr;
    UINT dpi = ccl::dpi::kDefaultDpi;
    HWND window = nullptr;
    HWND fields[3]{};  // margin, offsetX, offsetY

    ccl::io::ConcatPlacement placement;
    // The values the dialog opened with, put back on the way out so the caller
    // sees the preview cleared rather than left showing the last thing tried.
    ccl::io::ConcatPlacement initial;

    // Set while values are being written into the boxes, so that doing so does
    // not read as the user having typed something.
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
    return (std::max)(single, static_cast<int>(bounds.bottom - bounds.top) + 2);
}

void Announce(State& state) noexcept {
    if (state.notify != nullptr && *state.notify) {
        (*state.notify)(state.placement);
    }
}

int ReadNumber(HWND field, int fallback, int limit) noexcept {
    wchar_t text[32]{};
    ::GetWindowTextW(field, text, ARRAYSIZE(text));
    // A box holding nothing, or a lone minus sign part way through typing one,
    // is not a number yet. Treated as zero rather than refused, so the preview
    // keeps up instead of sticking at the last complete value.
    if (text[0] == L'\0' || (text[0] == L'-' && text[1] == L'\0')) {
        return 0;
    }
    wchar_t* end = nullptr;
    const long value = ::wcstol(text, &end, 10);
    if (end == text) {
        return fallback;
    }
    return std::clamp(static_cast<int>(value), -limit, limit);
}

void ReadFields(State& state) noexcept {
    // The gap only ever pushes them apart; pulling them together is what the
    // nudge is for, and letting both do it would make two ways to say one
    // thing.
    state.placement.margin =
        std::clamp(ReadNumber(state.fields[0], state.placement.margin, kMaxMargin),
                   0, kMaxMargin);
    state.placement.offsetX =
        ReadNumber(state.fields[1], state.placement.offsetX, kMaxOffset);
    state.placement.offsetY =
        ReadNumber(state.fields[2], state.placement.offsetY, kMaxOffset);
    Announce(state);
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

        case WM_COMMAND: {
            if (state == nullptr) {
                break;
            }
            const UINT id = LOWORD(wParam);
            const UINT code = HIWORD(wParam);

            for (const SideButton& side : kSides) {
                if (id == side.id && code == BN_CLICKED) {
                    state->placement.side = side.side;
                    Announce(*state);
                    return 0;
                }
            }
            if (id == kIdOnTop && code == BN_CLICKED) {
                state->placement.additionOnTop =
                    Button_GetCheck(::GetDlgItem(hwnd, kIdOnTop)) ==
                    BST_CHECKED;
                Announce(*state);
                return 0;
            }
            if ((id == kIdMargin || id == kIdOffsetX || id == kIdOffsetY) &&
                code == EN_CHANGE && !state->syncing) {
                ReadFields(*state);
                return 0;
            }
            if (id == IDOK) {
                state->accepted = true;
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

std::optional<ccl::io::ConcatPlacement> ShowConcatDialog(
    HWND owner, const ccl::io::ConcatPlacement& initial,
    const std::function<void(const ccl::io::ConcatPlacement&)>&
        onChanged) noexcept {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!RegisterDialogClass(instance)) {
        return std::nullopt;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    State state;
    state.notify = &onChanged;
    state.placement = initial;
    state.initial = initial;
    state.dpi = ccl::dpi::ForWindow(owner);
    state.font = CreateMessageFont(state.dpi);

    const auto scaled = [&state](int logical) {
        return ccl::dpi::Scale(logical, state.dpi);
    };

    const int margin = scaled(kMargin);
    const int rowHeight = scaled(kRowHeight);
    const int gap = scaled(kRowGap);
    const int width = scaled(kDialogWidth);

    state.window = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kWindowClass,
        L"クリップボードの画像を連結する",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, width,
        scaled(280), owner, nullptr, instance, &state);
    if (state.window == nullptr) {
        ::DeleteObject(state.font);
        return std::nullopt;
    }

    int y = margin;

    Add(state, L"STATIC", L"つなぐ向き", SS_LEFT, margin, y + scaled(4),
        scaled(kLabelWidth), rowHeight, 0);
    {
        // One group, so the four move together as a single choice. The first
        // gets WS_GROUP so the arrow keys walk them and Tab leaves the set.
        const int buttonWidth = (width - 2 * margin - scaled(kLabelWidth)) / 4;
        int x = margin + scaled(kLabelWidth);
        bool first = true;
        for (const SideButton& side : kSides) {
            DWORD style = BS_AUTORADIOBUTTON | WS_TABSTOP;
            if (first) {
                style |= WS_GROUP;
                first = false;
            }
            const HWND button = Add(state, L"BUTTON", side.label, style, x, y,
                                    buttonWidth, rowHeight, side.id);
            if (side.side == state.placement.side) {
                Button_SetCheck(button, BST_CHECKED);
            }
            x += buttonWidth;
        }
    }
    y += rowHeight + gap;

    struct NumberRow {
        const wchar_t* label;
        UINT id;
        int value;
    };
    const NumberRow rows[] = {
        {L"隙間 (px)", kIdMargin, state.placement.margin},
        {L"横のずれ (px)", kIdOffsetX, state.placement.offsetX},
        {L"縦のずれ (px)", kIdOffsetY, state.placement.offsetY},
    };
    state.syncing = true;
    for (int i = 0; i < 3; ++i) {
        Add(state, L"STATIC", rows[i].label, SS_LEFT, margin, y + scaled(4),
            scaled(kLabelWidth), rowHeight, 0);
        wchar_t text[16];
        ::swprintf_s(text, L"%d", rows[i].value);
        // No ES_NUMBER: the nudges take a minus sign, which it would refuse.
        state.fields[i] =
            Add(state, L"EDIT", text, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                margin + scaled(kLabelWidth), y, scaled(kNumberWidth),
                rowHeight, rows[i].id);
        y += rowHeight + gap;
    }
    state.syncing = false;

    const HWND onTop =
        Add(state, L"BUTTON", L"重なったとき、足す画像を上にする",
            BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, margin, y,
            width - 2 * margin, rowHeight, kIdOnTop);
    Button_SetCheck(onTop,
                    state.placement.additionOnTop ? BST_CHECKED : BST_UNCHECKED);
    y += rowHeight + gap;

    const wchar_t* note =
        L"ウィンドウに結果が出ます。ずれは向きに関係なく効くので、"
        L"重ねることもできます。空いた所は黒で埋まります。\n"
        L"決めると 1 枚の画像になり、それまでに描いた線や文字は"
        L"絵に焼き込まれます（取り消しで戻せます）。";
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
            x = std::clamp(x, static_cast<int>(info.rcWork.left),
                           static_cast<int>(info.rcWork.right) - outerWidth);
            top = std::clamp(top, static_cast<int>(info.rcWork.top),
                             static_cast<int>(info.rcWork.bottom) - outerHeight);
        }
    }
    ::SetWindowPos(state.window, HWND_TOP, x, top, outerWidth, outerHeight,
                   SWP_NOACTIVATE);

    // Shown with the starting placement already on the picture, so the window
    // and the dialog agree from the first moment rather than after the first
    // change.
    Announce(state);

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

    const ccl::io::ConcatPlacement chosen = state.placement;
    // The preview belongs to this dialog, so it is taken down here rather than
    // left for the caller to remember. Whether it was accepted or not: the
    // caller applies what is returned through its own path.
    state.placement = state.initial;
    Announce(state);

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
    return chosen;
}

}  // namespace ccl::ui

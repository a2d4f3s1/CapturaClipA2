#include "ui/DecorPanel.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kClass[] = L"CapturaClipA2DecorPanel";

constexpr int kIdOutline = 100;
constexpr int kIdOutlineWidth = 101;
constexpr int kIdOutlineColor = 102;
constexpr int kIdShadow = 103;
constexpr int kIdShadowLength = 104;
constexpr int kIdShadowWay = 105;
constexpr int kIdShadowColor = 106;
constexpr int kIdShadowOpacity = 107;

// The nine ways round, in the order they are numbered. The same names the menu
// uses -- two lists that drift apart would have the same number meaning two
// different directions.
constexpr const wchar_t* kWays[] = {L"上",   L"右上", L"右",
                                    L"右下", L"下",   L"左下",
                                    L"左",   L"左上", L"真下（にじみだけ）"};

COLORREF ToColorRef(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) {
        return static_cast<int>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

void SetNumber(HWND box, float value) noexcept {
    wchar_t text[16];
    ::swprintf_s(text, L"%d", static_cast<int>(value + 0.5f));
    ::SetWindowTextW(box, text);
}

float ReadNumber(HWND box, float fallback) noexcept {
    wchar_t text[16]{};
    ::GetWindowTextW(box, text, ARRAYSIZE(text));
    if (text[0] == L'\0') {
        return fallback;
    }
    return static_cast<float>(::_wtof(text));
}

}  // namespace

LRESULT CALLBACK DecorPanel::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    DecorPanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DecorPanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DecorPanel*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The number boxes answer the wheel, so a value can be nudged without typing.
LRESULT CALLBACK DecorPanel::NumberProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR id,
                                        DWORD_PTR data) {
    auto* self = reinterpret_cast<DecorPanel*>(data);
    if (msg == WM_MOUSEWHEEL && self != nullptr) {
        const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        self->Step(static_cast<int>(id), notches);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, &DecorPanel::NumberProc, id);
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool DecorPanel::Ours(HWND taking) const noexcept {
    if (taking == nullptr) {
        return false;
    }
    if (taking == hwnd_ || ::IsChild(hwnd_, taking)) {
        return true;
    }
    // A dropped list and the colour palette are windows of their own, owned by
    // the control that put them up rather than parented to it, so the owner
    // chain is what catches them.
    for (HWND owner = ::GetWindow(taking, GW_OWNER); owner != nullptr;
         owner = ::GetWindow(owner, GW_OWNER)) {
        if (owner == hwnd_ || ::IsChild(hwnd_, owner)) {
            return true;
        }
    }
    return false;
}

void DecorPanel::Close() noexcept {
    closing_ = true;
}

void DecorPanel::Step(int id, int by) noexcept {
    HWND box = ::GetDlgItem(hwnd_, id);
    if (box == nullptr || by == 0) {
        return;
    }
    const float now = ReadNumber(box, 0.0f);
    filling_ = true;
    SetNumber(box, now + static_cast<float>(by));
    filling_ = false;
    ReadNumbers();
    Notify(id == kIdOutlineWidth ? Field::OutlineWidth
                                 : (id == kIdShadowLength ? Field::ShadowLength
                                                          : Field::ShadowOpacity));
}

void DecorPanel::ReadNumbers() noexcept {
    values_.outlineWidth =
        ReadNumber(::GetDlgItem(hwnd_, kIdOutlineWidth), values_.outlineWidth);
    values_.shadowLength =
        ReadNumber(::GetDlgItem(hwnd_, kIdShadowLength), values_.shadowLength);
    values_.shadowOpacity = ReadNumber(::GetDlgItem(hwnd_, kIdShadowOpacity),
                                       values_.shadowOpacity);
}

void DecorPanel::Notify(Field field) noexcept {
    if (onChange_) {
        onChange_(field, values_);
    }
}

void DecorPanel::Refresh(const DecorValues& values) noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    values_ = values;
    filling_ = true;
    ::SendMessageW(::GetDlgItem(hwnd_, kIdOutline), BM_SETCHECK,
                   values_.outline ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(::GetDlgItem(hwnd_, kIdShadow), BM_SETCHECK,
                   values_.shadow ? BST_CHECKED : BST_UNCHECKED, 0);
    SetNumber(::GetDlgItem(hwnd_, kIdOutlineWidth), values_.outlineWidth);
    SetNumber(::GetDlgItem(hwnd_, kIdShadowLength), values_.shadowLength);
    SetNumber(::GetDlgItem(hwnd_, kIdShadowOpacity), values_.shadowOpacity);
    ComboBox_SetCurSel(::GetDlgItem(hwnd_, kIdShadowWay),
                       std::clamp(values_.shadowDirection, 0,
                                  static_cast<int>(ARRAYSIZE(kWays)) - 1));
    ::InvalidateRect(::GetDlgItem(hwnd_, kIdOutlineColor), nullptr, TRUE);
    ::InvalidateRect(::GetDlgItem(hwnd_, kIdShadowColor), nullptr, TRUE);
    filling_ = false;
}

void DecorPanel::Build(int scalePercent) noexcept {
    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const int scale = std::clamp(scalePercent, 50, 300);
    const auto units = [dpi, scale](int value) {
        return ::MulDiv(::MulDiv(value, static_cast<int>(dpi), 96), scale, 100);
    };

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                 &metrics, 0, dpi);
    font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);

    const int pad = units(10);
    const int row = units(26);
    const int checkLeft = units(8);
    const int labelLeft = units(24);
    const int labelWidth = units(80);
    const int fieldLeft = labelLeft + labelWidth + units(8);
    const int numberWidth = units(56);
    const int swatchWidth = units(34);
    const int height = units(20);
    const int lift = units(3);

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));

    const auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                         int x, int y, int width, int tall, int id) {
        HWND child = ::CreateWindowExW(
            0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, tall,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
            nullptr);
        ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return child;
    };

    // The list has to be wide enough for its longest entry, which is a good
    // deal longer than the rest: measured rather than guessed at, so that a
    // name changing does not quietly clip it.
    int comboWidth = units(120);
    if (const HDC dc = ::GetDC(hwnd_)) {
        const HGDIOBJ old = ::SelectObject(dc, font_);
        for (const wchar_t* way : kWays) {
            SIZE size{};
            ::GetTextExtentPoint32W(dc, way, static_cast<int>(::wcslen(way)),
                                    &size);
            comboWidth =
                (std::max)(comboWidth, static_cast<int>(size.cx) + units(34));
        }
        ::SelectObject(dc, old);
        ::ReleaseDC(hwnd_, dc);
    }

    int y = pad;
    add(L"BUTTON", L"縁取り", BS_AUTOCHECKBOX | WS_TABSTOP, checkLeft, y,
        labelWidth + units(24), height, kIdOutline);
    y += row;
    add(L"STATIC", L"縁の太さ", SS_RIGHT, labelLeft, y + lift, labelWidth,
        height, 0);
    add(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, fieldLeft,
        y, numberWidth, height, kIdOutlineWidth);
    y += row;
    add(L"STATIC", L"縁の色", SS_RIGHT, labelLeft, y + lift, labelWidth, height,
        0);
    add(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, fieldLeft, y, swatchWidth,
        height, kIdOutlineColor);
    y += row;
    add(L"BUTTON", L"影", BS_AUTOCHECKBOX | WS_TABSTOP, checkLeft, y,
        labelWidth + units(24), height, kIdShadow);
    y += row;
    add(L"STATIC", L"影の長さ", SS_RIGHT, labelLeft, y + lift, labelWidth,
        height, 0);
    add(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, fieldLeft,
        y, numberWidth, height, kIdShadowLength);
    y += row;
    add(L"STATIC", L"影の向き", SS_RIGHT, labelLeft, y + lift, labelWidth,
        height, 0);
    HWND combo = add(L"COMBOBOX", L"",
                     CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, fieldLeft, y,
                     comboWidth, units(220), kIdShadowWay);
    for (const wchar_t* way : kWays) {
        ComboBox_AddString(combo, way);
    }
    y += row;
    add(L"STATIC", L"影の色", SS_RIGHT, labelLeft, y + lift, labelWidth, height,
        0);
    add(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, fieldLeft, y, swatchWidth,
        height, kIdShadowColor);
    y += row;
    add(L"STATIC", L"影の濃さ", SS_RIGHT, labelLeft, y + lift, labelWidth,
        height, 0);
    add(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, fieldLeft,
        y, numberWidth, height, kIdShadowOpacity);
    y += row;

    for (const int id : {kIdOutlineWidth, kIdShadowLength, kIdShadowOpacity}) {
        ::SetWindowSubclass(::GetDlgItem(hwnd_, id), &DecorPanel::NumberProc,
                            static_cast<UINT_PTR>(id),
                            reinterpret_cast<DWORD_PTR>(this));
    }

    // Grown to fit what was laid out, then pulled back onto the screen it
    // landed on -- opened near the pointer, it can start off the edge.
    RECT wanted{0, 0, fieldLeft + comboWidth + pad, y + pad - (row - height)};
    ::AdjustWindowRectEx(&wanted, static_cast<DWORD>(::GetWindowLongPtrW(
                                      hwnd_, GWL_STYLE)),
                         FALSE, WS_EX_TOOLWINDOW);
    RECT at{};
    ::GetWindowRect(hwnd_, &at);
    const int width = wanted.right - wanted.left;
    const int tall = wanted.bottom - wanted.top;
    int left = at.left;
    int top = at.top;
    if (const HMONITOR screen =
            ::MonitorFromPoint(POINT{left, top}, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(screen, &info)) {
            left = std::clamp(left, static_cast<int>(info.rcWork.left),
                              static_cast<int>(info.rcWork.right) - width);
            top = std::clamp(top, static_cast<int>(info.rcWork.top),
                             static_cast<int>(info.rcWork.bottom) - tall);
        }
    }
    ::SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, tall, SWP_NOACTIVATE);
}

LRESULT DecorPanel::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE:
            // Anything that is not ours taking over means the pointer has gone
            // elsewhere, which is how this closes. A dropped list or the
            // palette is ours, and leaves it open.
            if (LOWORD(wParam) == WA_INACTIVE &&
                !Ours(reinterpret_cast<HWND>(lParam))) {
                Close();
            }
            return 0;

        case WM_CTLCOLORSTATIC:
            // Labels sit straight on the panel rather than in boxes of their
            // own.
            ::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));

        case WM_DRAWITEM: {
            // The colour rows: a button that is nothing but its colour, with a
            // thin edge so that a white one still reads as a button.
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            const ccl::doc::Color& colour = item->CtlID == kIdOutlineColor
                                                ? values_.outlineColor
                                                : values_.shadowColor;
            const HBRUSH fill = ::CreateSolidBrush(ToColorRef(colour));
            ::FillRect(item->hDC, &item->rcItem, fill);
            ::DeleteObject(fill);
            ::FrameRect(item->hDC, &item->rcItem,
                        ::GetSysColorBrush((item->itemState & ODS_FOCUS)
                                               ? COLOR_HIGHLIGHT
                                               : COLOR_WINDOWFRAME));
            return TRUE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (filling_) {
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case kIdOutline:
                        values_.outline =
                            ::SendMessageW(::GetDlgItem(hwnd_, kIdOutline),
                                           BM_GETCHECK, 0, 0) == BST_CHECKED;
                        Notify(Field::Outline);
                        return 0;
                    case kIdShadow:
                        values_.shadow =
                            ::SendMessageW(::GetDlgItem(hwnd_, kIdShadow),
                                           BM_GETCHECK, 0, 0) == BST_CHECKED;
                        Notify(Field::Shadow);
                        return 0;
                    case kIdOutlineColor:
                        if (onPick_) {
                            onPick_(Field::OutlineColor, hwnd_);
                        }
                        return 0;
                    case kIdShadowColor:
                        if (onPick_) {
                            onPick_(Field::ShadowColor, hwnd_);
                        }
                        return 0;
                    default:
                        break;
                }
            }
            if (code == CBN_SELCHANGE && id == kIdShadowWay) {
                const int way =
                    ComboBox_GetCurSel(::GetDlgItem(hwnd_, kIdShadowWay));
                if (way >= 0) {
                    values_.shadowDirection = way;
                    Notify(Field::ShadowDirection);
                }
                return 0;
            }
            if (code == EN_KILLFOCUS) {
                ReadNumbers();
                switch (id) {
                    case kIdOutlineWidth: Notify(Field::OutlineWidth); break;
                    case kIdShadowLength: Notify(Field::ShadowLength); break;
                    case kIdShadowOpacity: Notify(Field::ShadowOpacity); break;
                    default: break;
                }
                return 0;
            }
            return 0;
        }

        case WM_DESTROY:
            if (font_ != nullptr) {
                ::DeleteObject(font_);
                font_ = nullptr;
            }
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void DecorPanel::Show(HWND owner, POINT screen, const DecorValues& values,
                      int scalePercent, Changed onChange,
                      PickColour onPick) noexcept {
    values_ = values;
    onChange_ = std::move(onChange);
    onPick_ = std::move(onPick);
    closing_ = false;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DecorPanel::WndProcThunk;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClass;
        registered = ::RegisterClassExW(&wc) != 0 ||
                     ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered) {
            return;
        }
    }

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"",
                              WS_POPUP | WS_BORDER, screen.x, screen.y, 10, 10,
                              owner, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return;
    }

    Build(scalePercent);
    Refresh(values_);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(::GetDlgItem(hwnd_, kIdOutline));

    MSG msg;
    while (!closing_ && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            Close();
            break;
        }
        // Enter puts what has been typed in without waiting for the focus to
        // move on, and Tab walks the rows.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            const HWND focus = ::GetFocus();
            ReadNumbers();
            const int id = focus != nullptr ? ::GetDlgCtrlID(focus) : 0;
            if (id == kIdOutlineWidth) {
                Notify(Field::OutlineWidth);
            } else if (id == kIdShadowLength) {
                Notify(Field::ShadowLength);
            } else if (id == kIdShadowOpacity) {
                Notify(Field::ShadowOpacity);
            }
            continue;
        }
        if (::IsDialogMessageW(hwnd_, &msg)) {
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    onChange_ = nullptr;
    onPick_ = nullptr;
}

}  // namespace ccl::ui

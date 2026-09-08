#include "ui/FontPicker.h"

#include <commctrl.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>

#include "util/Dpi.h"

namespace ccl::ui {

// The en-us name is carried alongside so the picker can match on either: most
// of the families whose name is written in Japanese answer to an English one
// too, and matching both is what lets them be typed without the IME.
const std::vector<FontEntry>& InstalledFonts(IDWriteFactory* writer) {
    static std::vector<FontEntry> fonts = [writer] {
        std::vector<FontEntry> names;
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

            FontEntry entry;
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
                  [](const FontEntry& a, const FontEntry& b) {
                      return a.shown < b.shown;
                  });
        return names;
    }();
    return fonts;
}

namespace {

constexpr wchar_t kClass[] = L"CapturaClipA2FontPicker";

constexpr int kIdFilter = 100;
constexpr int kIdList = 101;

// Rows of the list. The upper bound is what fits comfortably; the lower one is
// what is still worth scrolling. Between them the monitor decides -- at 200%
// sixteen rows want more height than a short screen has, and clamping the
// window afterwards would cut the list off without saying so.
constexpr int kMaxRows = 16;
constexpr int kMinRows = 8;

// Case, width and kana are all ignored, so that "ms" reaches ＭＳ ゴシック and
// the hiragana an IME shows before conversion reaches the katakana in a name.
// Doing this by hand would mean carrying tables for both; the locale already
// has them.
constexpr DWORD kMatchFlags =
    NORM_IGNORECASE | NORM_IGNOREWIDTH | NORM_IGNOREKANATYPE;

bool Contains(const std::wstring& haystack, const std::wstring& needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (haystack.empty()) {
        return false;
    }
    return ::FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,
                             FIND_FROMSTART | kMatchFlags, haystack.c_str(),
                             static_cast<int>(haystack.size()), needle.c_str(),
                             static_cast<int>(needle.size()), nullptr, nullptr,
                             nullptr, 0) >= 0;
}

std::wstring TextOf(HWND control) noexcept {
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int written = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(written > 0 ? static_cast<size_t>(written) : 0);
    return text;
}

}  // namespace

LRESULT CALLBACK FontPicker::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    FontPicker* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<FontPicker*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<FontPicker*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Keys that belong to the list are handed to it while the box keeps the focus,
// so a name can be narrowed and then walked without reaching for the mouse.
LRESULT CALLBACK FontPicker::FilterProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR id,
                                        DWORD_PTR data) {
    auto* self = reinterpret_cast<FontPicker*>(data);
    if (self != nullptr && self->list_ != nullptr &&
        (msg == WM_KEYDOWN || msg == WM_MOUSEWHEEL)) {
        if (msg == WM_MOUSEWHEEL) {
            ::SendMessageW(self->list_, msg, wParam, lParam);
            return 0;
        }
        switch (wParam) {
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_HOME:
            case VK_END:
                // Home and End walk the list rather than the text: with the
                // list is where they are useful, and the box holds a word.
                ::SendMessageW(self->list_, msg, wParam, lParam);
                return 0;
            default:
                break;
        }
    }
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, &FontPicker::FilterProc, id);
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool FontPicker::Ours(HWND taking) const noexcept {
    if (taking == nullptr) {
        return false;
    }
    if (taking == hwnd_ || ::IsChild(hwnd_, taking)) {
        return true;
    }
    for (HWND owner = ::GetWindow(taking, GW_OWNER); owner != nullptr;
         owner = ::GetWindow(owner, GW_OWNER)) {
        if (owner == hwnd_ || ::IsChild(hwnd_, owner)) {
            return true;
        }
    }
    return false;
}

void FontPicker::Close(bool accepted) noexcept {
    accepted_ = accepted;
    finished_ = true;
}

std::optional<std::wstring> FontPicker::Selected() const noexcept {
    if (list_ == nullptr) {
        return std::nullopt;
    }
    const auto index = static_cast<int>(
        ::SendMessageW(list_, LB_GETCURSEL, 0, 0));
    if (index == LB_ERR) {
        return std::nullopt;
    }
    const auto length =
        static_cast<int>(::SendMessageW(list_, LB_GETTEXTLEN, index, 0));
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring name(static_cast<size_t>(length) + 1, L'\0');
    const auto written = static_cast<int>(::SendMessageW(
        list_, LB_GETTEXT, index, reinterpret_cast<LPARAM>(name.data())));
    if (written <= 0) {
        return std::nullopt;
    }
    name.resize(static_cast<size_t>(written));
    return name;
}

void FontPicker::Refilter() noexcept {
    if (list_ == nullptr || fonts_ == nullptr) {
        return;
    }
    const std::wstring needle = TextOf(filter_);
    // What is picked out survives the filter when it still matches, so that
    // narrowing and widening again does not lose the place. Before anything has
    // been picked, that is whatever font is already in force.
    const std::optional<std::wstring> picked = Selected();
    const std::wstring keep = picked.has_value() ? *picked : current_;

    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int shown = 0;
    int select = -1;
    for (const FontEntry& entry : *fonts_) {
        if (!Contains(entry.shown, needle) &&
            !Contains(entry.english, needle)) {
            continue;
        }
        ::SendMessageW(list_, LB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(entry.shown.c_str()));
        if (!keep.empty() && entry.shown == keep) {
            select = shown;
        }
        ++shown;
    }
    // Nothing matched leaves no selection, so that Enter has nothing to choose.
    // Something matched but the old pick did not survive: the first row, which
    // is what typing a name is aiming at.
    if (select < 0 && shown > 0 && !needle.empty()) {
        select = 0;
    }
    ::SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(select), 0);
    if (select >= 0) {
        // Put it a few rows down rather than hard against the bottom, which is
        // where LB_SETCURSEL alone leaves it: the names either side are part of
        // how a font is chosen.
        const int top = (std::max)(0, select - 3);
        ::SendMessageW(list_, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_, nullptr, TRUE);
}

void FontPicker::Build(POINT screen, int scalePercent) noexcept {
    const UINT dpi = ccl::dpi::ForWindow(hwnd_);
    const int scale = std::clamp(scalePercent, 50, 300);
    const auto units = [dpi, scale](int value) {
        return ::MulDiv(::MulDiv(value, static_cast<int>(dpi), 96), scale, 100);
    };

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                 &metrics, 0, dpi);
    // The scale goes into the type as well as the layout, unlike the decoration
    // panel: that one is rows of checkboxes and number fields, where the boxes
    // are what wants to grow, while this window is a list of names, where the
    // type is the content. Growing only the box leaves the same small names in
    // a larger frame, which is not what asking for a bigger palette meant.
    metrics.lfMessageFont.lfHeight =
        ::MulDiv(metrics.lfMessageFont.lfHeight, scale, 100);
    font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);

    // Width from the longest name actually installed, measured rather than
    // guessed at: a font added later must not be quietly clipped.
    int textWidth = units(140);
    int lineHeight = units(16);
    if (const HDC dc = ::GetDC(hwnd_)) {
        const HGDIOBJ old = ::SelectObject(dc, font_);
        TEXTMETRICW tm{};
        ::GetTextMetricsW(dc, &tm);
        lineHeight = tm.tmHeight + units(2);
        if (fonts_ != nullptr) {
            for (const FontEntry& entry : *fonts_) {
                SIZE size{};
                ::GetTextExtentPoint32W(dc, entry.shown.c_str(),
                                        static_cast<int>(entry.shown.size()),
                                        &size);
                textWidth = (std::max)(textWidth, static_cast<int>(size.cx));
            }
        }
        ::SelectObject(dc, old);
        ::ReleaseDC(hwnd_, dc);
    }

    const int pad = units(8);
    const int gap = units(6);
    const int boxHeight = units(22);
    const int width =
        textWidth + ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) + pad * 3;

    RECT work{0, 0, ::GetSystemMetrics(SM_CXSCREEN),
              ::GetSystemMetrics(SM_CYSCREEN)};
    const HMONITOR monitor = ::MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    }

    // How many rows the screen can take, before deciding where to put them.
    const int spare = (work.bottom - work.top) - (pad * 2 + boxHeight + gap);
    const int fits = lineHeight > 0 ? spare / lineHeight : kMaxRows;
    const int rows = std::clamp(fits, kMinRows, kMaxRows);
    const int listHeight = lineHeight * rows;

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));

    filter_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, pad,
        pad, width - pad * 2, boxHeight, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdFilter)), instance,
        nullptr);
    list_ = ::CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP |
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, pad + boxHeight + gap, width - pad * 2, listHeight, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), instance,
        nullptr);
    ::SendMessageW(filter_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ::SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ::SetWindowSubclass(filter_, &FontPicker::FilterProc,
                        static_cast<UINT_PTR>(kIdFilter),
                        reinterpret_cast<DWORD_PTR>(this));

    RECT wanted{0, 0, width, pad * 2 + boxHeight + gap + listHeight};
    ::AdjustWindowRectEx(
        &wanted,
        static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
        WS_EX_TOOLWINDOW);
    const int outWidth = wanted.right - wanted.left;
    const int outHeight = wanted.bottom - wanted.top;
    const int left = std::clamp(static_cast<int>(screen.x),
                                static_cast<int>(work.left),
                                (std::max)(static_cast<int>(work.left),
                                           static_cast<int>(work.right) - outWidth));
    const int top = std::clamp(static_cast<int>(screen.y),
                               static_cast<int>(work.top),
                               (std::max)(static_cast<int>(work.top),
                                          static_cast<int>(work.bottom) - outHeight));
    ::SetWindowPos(hwnd_, HWND_TOPMOST, left, top, outWidth, outHeight,
                   SWP_NOACTIVATE);
}

LRESULT FontPicker::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE:
            // Anything that is not ours taking over means the pointer has gone
            // elsewhere, which is how this is dismissed.
            if (ready_ && LOWORD(wParam) == WA_INACTIVE &&
                !Ours(reinterpret_cast<HWND>(lParam))) {
                Close(false);
            }
            return 0;

        case WM_CTLCOLORSTATIC:
            ::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == kIdFilter && code == EN_CHANGE) {
                Refilter();
                return 0;
            }
            if (id == kIdList && code == LBN_DBLCLK) {
                if (Selected().has_value()) {
                    Close(true);
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

std::optional<std::wstring> FontPicker::Show(HWND owner, POINT screen,
                                             const std::vector<FontEntry>& fonts,
                                             const std::wstring& current,
                                             int scalePercent) noexcept {
    fonts_ = &fonts;
    current_ = current;
    finished_ = false;
    accepted_ = false;
    ready_ = false;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &FontPicker::WndProcThunk;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClass;
        registered = ::RegisterClassExW(&wc) != 0 ||
                     ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered) {
            return std::nullopt;
        }
    }

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"",
                              WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, screen.x,
                              screen.y, 10, 10, owner, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return std::nullopt;
    }

    Build(screen, scalePercent);
    Refilter();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(filter_);
    ready_ = true;

    MSG msg;
    while (!finished_ && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            Close(false);
            break;
        }
        // Enter is taken before IsDialogMessageW, which would otherwise swallow
        // it looking for a default button this window does not have.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (Selected().has_value()) {
                Close(true);
                break;
            }
            continue;
        }
        if (::IsDialogMessageW(hwnd_, &msg)) {
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    std::optional<std::wstring> chosen;
    if (accepted_) {
        chosen = Selected();
    }

    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    filter_ = nullptr;
    list_ = nullptr;
    fonts_ = nullptr;
    return chosen;
}

}  // namespace ccl::ui

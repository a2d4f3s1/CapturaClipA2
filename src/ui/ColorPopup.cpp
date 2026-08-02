#include "ui/ColorPopup.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "tool/ToolState.h"
#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kPopupClass[] = L"CapturaClipA2.ColorPopup";

// Logical (96-DPI) layout.
constexpr int kPadding = 8;
constexpr int kFieldWidth = 208;
constexpr int kFieldHeight = 132;
constexpr int kHueHeight = 14;
constexpr int kGap = 6;
constexpr int kSwatchHeight = 20;
constexpr int kSwatchColumns = 8;

struct Hsv {
    float h = 0.0f;  // 0..360
    float s = 0.0f;
    float v = 0.0f;
};

Hsv ToHsv(const ccl::doc::Color& color) noexcept {
    const float max = std::max({color.r, color.g, color.b});
    const float min = std::min({color.r, color.g, color.b});
    const float delta = max - min;

    Hsv hsv;
    hsv.v = max;
    hsv.s = max > 0.0f ? delta / max : 0.0f;

    if (delta > 0.0f) {
        if (max == color.r) {
            hsv.h = 60.0f * std::fmod((color.g - color.b) / delta, 6.0f);
        } else if (max == color.g) {
            hsv.h = 60.0f * (((color.b - color.r) / delta) + 2.0f);
        } else {
            hsv.h = 60.0f * (((color.r - color.g) / delta) + 4.0f);
        }
    }
    if (hsv.h < 0.0f) {
        hsv.h += 360.0f;
    }
    return hsv;
}

ccl::doc::Color FromHsv(float h, float s, float v) noexcept {
    const float c = v * s;
    const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (h < 60.0f)        { r = c; g = x; }
    else if (h < 120.0f)  { r = x; g = c; }
    else if (h < 180.0f)  { g = c; b = x; }
    else if (h < 240.0f)  { g = x; b = c; }
    else if (h < 300.0f)  { r = x; b = c; }
    else                  { r = c; b = x; }

    return ccl::doc::Color{r + m, g + m, b + m, 1.0f};
}

COLORREF ToColorRef(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

void FrameWith(HDC dc, const RECT& area, COLORREF color) noexcept {
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FrameRect(dc, &area, brush);
        ::DeleteObject(brush);
    }
}

void FillWith(HDC dc, const RECT& area, COLORREF color) noexcept {
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FillRect(dc, &area, brush);
        ::DeleteObject(brush);
    }
}

}  // namespace

LRESULT CALLBACK ColorPopup::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<ColorPopup*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        self->hwnd_ = hwnd;
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ColorPopup::Close(bool accepted) noexcept {
    accepted_ = accepted;
    finished_ = true;
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
    }
}

void ColorPopup::NotifyChange() noexcept {
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    if (onChange_) {
        onChange_(FromHsv(hue_, saturation_, value_));
    }
}

void ColorPopup::HandlePress(POINT point) noexcept {
    if (::PtInRect(&fieldRect_, point)) {
        const float width = static_cast<float>(fieldRect_.right - fieldRect_.left);
        const float height = static_cast<float>(fieldRect_.bottom - fieldRect_.top);
        saturation_ = std::clamp((point.x - fieldRect_.left) / width, 0.0f, 1.0f);
        value_ = std::clamp(1.0f - (point.y - fieldRect_.top) / height, 0.0f, 1.0f);
        NotifyChange();
        return;
    }

    if (::PtInRect(&hueRect_, point)) {
        const float width = static_cast<float>(hueRect_.right - hueRect_.left);
        hue_ = std::clamp((point.x - hueRect_.left) / width, 0.0f, 1.0f) * 359.9f;
        NotifyChange();
        return;
    }

    // A swatch loads the colour but does not close the popup, so it can serve
    // as a starting point that is then adjusted in the field above.
    const auto swatchAt = [&](const RECT& row, const auto& colors) -> bool {
        if (!::PtInRect(&row, point)) {
            return false;
        }
        const int index = (point.x - row.left) / std::max(1, swatchWidth_);
        if (index < 0 || static_cast<size_t>(index) >= colors.size()) {
            return false;
        }
        const Hsv hsv = ToHsv(colors[static_cast<size_t>(index)]);
        hue_ = hsv.h;
        saturation_ = hsv.s;
        value_ = hsv.v;
        NotifyChange();
        return true;
    };

    if (swatchAt(templateRect_, ccl::tool::kQuickColors)) {
        return;
    }
    swatchAt(recentRect_, RecentRow());
}

LRESULT ColorPopup::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = ::BeginPaint(hwnd_, &ps);

            // Drawn into an off-screen bitmap and blitted in one go. Painting
            // straight to the window made the swatches flicker, since dragging
            // repaints the whole popup on every mouse move.
            RECT client{};
            ::GetClientRect(hwnd_, &client);

            const HDC memory = ::CreateCompatibleDC(dc);
            const HBITMAP buffer =
                ::CreateCompatibleBitmap(dc, client.right, client.bottom);

            if (memory != nullptr && buffer != nullptr) {
                const HGDIOBJ previous = ::SelectObject(memory, buffer);
                Paint(memory);
                ::BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0,
                         SRCCOPY);
                ::SelectObject(memory, previous);
            } else {
                Paint(dc);
            }

            if (buffer != nullptr) {
                ::DeleteObject(buffer);
            }
            if (memory != nullptr) {
                ::DeleteDC(memory);
            }

            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (::PtInRect(&fieldRect_, point)) {
                draggingField_ = true;
                ::SetCapture(hwnd_);
            } else if (::PtInRect(&hueRect_, point)) {
                draggingHue_ = true;
                ::SetCapture(hwnd_);
            }
            HandlePress(point);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (draggingField_ || draggingHue_) {
                HandlePress(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            }
            return 0;

        case WM_LBUTTONUP:
            if (draggingField_ || draggingHue_) {
                draggingField_ = false;
                draggingHue_ = false;
                ::ReleaseCapture();
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                Close(false);
            } else if (wParam == VK_RETURN) {
                Close(true);
            }
            return 0;

        case WM_KILLFOCUS:
            // Clicking away is how the choice is confirmed; the popup has no
            // button to press. Ignored until the popup has settled, since
            // taking focus during opening also reports a loss and closed it
            // before it was ever seen.
            if (ready_ && !finished_) {
                Close(true);
            }
            return 0;

        case WM_NCDESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            finished_ = true;
            break;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void ColorPopup::RebuildFieldBitmap() noexcept {
    const int hueKey = static_cast<int>(hue_);
    if (fieldBitmap_ != nullptr && builtForHue_ == hueKey) {
        return;
    }

    if (fieldBitmap_ != nullptr) {
        ::DeleteObject(fieldBitmap_);
        fieldBitmap_ = nullptr;
    }

    const int width = fieldRect_.right - fieldRect_.left;
    const int height = fieldRect_.bottom - fieldRect_.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    fieldBitmap_ =
        ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (fieldBitmap_ == nullptr || pixels == nullptr) {
        return;
    }

    auto* row = static_cast<unsigned char*>(pixels);
    for (int y = 0; y < height; ++y) {
        const float v = 1.0f - static_cast<float>(y) / static_cast<float>(height - 1);
        for (int x = 0; x < width; ++x) {
            const float s = static_cast<float>(x) / static_cast<float>(width - 1);
            const ccl::doc::Color color = FromHsv(hue_, s, v);
            unsigned char* pixel = row + static_cast<size_t>(x) * 4u;
            pixel[0] = static_cast<unsigned char>(color.b * 255.0f);
            pixel[1] = static_cast<unsigned char>(color.g * 255.0f);
            pixel[2] = static_cast<unsigned char>(color.r * 255.0f);
            pixel[3] = 255;
        }
        row += static_cast<size_t>(width) * 4u;
    }
    builtForHue_ = hueKey;
}

void ColorPopup::Paint(HDC dc) noexcept {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    FillWith(dc, client, ::GetSysColor(COLOR_MENU));
    FrameWith(dc, client, ::GetSysColor(COLOR_WINDOWFRAME));

    RebuildFieldBitmap();
    if (fieldBitmap_ != nullptr) {
        const HDC memory = ::CreateCompatibleDC(dc);
        const HGDIOBJ previous = ::SelectObject(memory, fieldBitmap_);
        ::BitBlt(dc, fieldRect_.left, fieldRect_.top,
                 fieldRect_.right - fieldRect_.left,
                 fieldRect_.bottom - fieldRect_.top, memory, 0, 0, SRCCOPY);
        ::SelectObject(memory, previous);
        ::DeleteDC(memory);
    }
    FrameWith(dc, fieldRect_, ::GetSysColor(COLOR_WINDOWFRAME));

    // Marker for the current point in the field, drawn in both black and white
    // so it survives any background.
    {
        const int width = fieldRect_.right - fieldRect_.left;
        const int height = fieldRect_.bottom - fieldRect_.top;
        const int x = fieldRect_.left + static_cast<int>(saturation_ * width);
        const int y = fieldRect_.top + static_cast<int>((1.0f - value_) * height);
        RECT marker{x - 4, y - 4, x + 5, y + 5};
        FrameWith(dc, marker, RGB(0, 0, 0));
        ::InflateRect(&marker, -1, -1);
        FrameWith(dc, marker, RGB(255, 255, 255));
    }

    for (int x = hueRect_.left; x < hueRect_.right; ++x) {
        const float h = 359.9f * static_cast<float>(x - hueRect_.left) /
                        static_cast<float>(hueRect_.right - hueRect_.left - 1);
        RECT column{x, hueRect_.top, x + 1, hueRect_.bottom};
        FillWith(dc, column, ToColorRef(FromHsv(h, 1.0f, 1.0f)));
    }
    FrameWith(dc, hueRect_, ::GetSysColor(COLOR_WINDOWFRAME));

    {
        const int x = hueRect_.left +
                      static_cast<int>(hue_ / 359.9f *
                                       (hueRect_.right - hueRect_.left - 1));
        RECT marker{x - 2, hueRect_.top - 2, x + 3, hueRect_.bottom + 2};
        FrameWith(dc, marker, RGB(0, 0, 0));
        ::InflateRect(&marker, -1, -1);
        FrameWith(dc, marker, RGB(255, 255, 255));
    }

    const auto drawSwatches = [&](const RECT& row, const auto& colors) {
        for (size_t i = 0; i < colors.size() && i < kSwatchColumns; ++i) {
            RECT cell{row.left + static_cast<int>(i) * swatchWidth_, row.top,
                      row.left + static_cast<int>(i + 1) * swatchWidth_,
                      row.bottom};
            FillWith(dc, cell, ToColorRef(colors[i]));
            FrameWith(dc, cell, ::GetSysColor(COLOR_WINDOWFRAME));
        }
    };
    drawSwatches(templateRect_, ccl::tool::kQuickColors);
    drawSwatches(recentRect_, RecentRow());
}

std::vector<ccl::doc::Color> ColorPopup::RecentRow() const {
    std::vector<ccl::doc::Color> row;
    row.reserve(kSwatchColumns);

    // Leftmost cell tracks what is being mixed right now, which is also how
    // the colour will sit in the history once it is committed.
    const ccl::doc::Color current = FromHsv(hue_, saturation_, value_);
    row.push_back(current);

    for (const ccl::doc::Color& entry : recent_) {
        if (row.size() >= kSwatchColumns) {
            break;
        }
        // Skipped rather than shown twice while it is still the live colour.
        if (ccl::tool::SameColor(entry, current)) {
            continue;
        }
        row.push_back(entry);
    }
    return row;
}

std::optional<ccl::doc::Color> ColorPopup::Show(
    HWND owner, POINT screen, const ccl::doc::Color& current,
    const std::vector<ccl::doc::Color>& recent, int scalePercent,
    ColorChanged onChange) noexcept {
    recent_ = recent;
    onChange_ = std::move(onChange);

    const Hsv hsv = ToHsv(current);
    hue_ = hsv.h;
    saturation_ = hsv.s;
    value_ = hsv.v;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ColorPopup::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return std::nullopt;
    }

    const UINT dpi = ccl::dpi::ForPoint(screen);
    const int percent = std::clamp(scalePercent, 50, 300);
    const auto scaled = [dpi, percent](int logical) {
        return ccl::dpi::Scale(logical * percent / 100, dpi);
    };

    const int padding = scaled(kPadding);
    const int fieldWidth = scaled(kFieldWidth);
    const int width = fieldWidth + padding * 2;

    int y = padding;
    fieldRect_ = {padding, y, padding + fieldWidth, y + scaled(kFieldHeight)};
    y = fieldRect_.bottom + scaled(kGap);

    hueRect_ = {padding, y, padding + fieldWidth, y + scaled(kHueHeight)};
    y = hueRect_.bottom + scaled(kGap);

    swatchWidth_ = fieldWidth / kSwatchColumns;
    swatchHeight_ = scaled(kSwatchHeight);

    templateRect_ = {padding, y, padding + swatchWidth_ * kSwatchColumns,
                     y + swatchHeight_};
    y = templateRect_.bottom;

    recentRect_ = {padding, y, padding + swatchWidth_ * kSwatchColumns,
                   y + swatchHeight_};
    y = recentRect_.bottom + padding;

    const int height = y;

    // Nudged on screen if it would otherwise open past the monitor edge.
    RECT work{};
    const HMONITOR monitor = ::MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
        screen.x = std::min<LONG>(screen.x, work.right - width);
        screen.y = std::min<LONG>(screen.y, work.bottom - height);
        screen.x = std::max<LONG>(screen.x, work.left);
        screen.y = std::max<LONG>(screen.y, work.top);
    }

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"",
                              WS_POPUP, screen.x, screen.y, width, height, owner,
                              nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return std::nullopt;
    }

    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(hwnd_);
    ready_ = true;

    MSG msg{};
    while (!finished_ && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (fieldBitmap_ != nullptr) {
        ::DeleteObject(fieldBitmap_);
        fieldBitmap_ = nullptr;
    }

    if (!accepted_) {
        return std::nullopt;
    }
    return FromHsv(hue_, saturation_, value_);
}

}  // namespace ccl::ui

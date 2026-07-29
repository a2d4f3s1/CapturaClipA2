#include "overlay/SelectionOverlay.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#include "util/Timing.h"

namespace ccl::overlay {
namespace {

constexpr wchar_t kOverlayClass[] = L"CapturaClipA2.SelectionOverlay";

// Width of the rubber band outline, in pixels.
constexpr int kFrameWidth = 1;

// Movement below this counts as a click rather than a drag. Zero would mean a
// slight tremor turns a window pick into an empty selection.
constexpr int kClickThreshold = 3;

bool Intersect(const RECT& a, const RECT& b, RECT& out) noexcept {
    return ::IntersectRect(&out, &a, &b) != FALSE;
}

// The four edge rectangles that make up an outline drawn just outside `inner`.
void OutlineEdges(const RECT& inner, RECT (&edges)[4]) noexcept {
    const LONG w = kFrameWidth;
    edges[0] = {inner.left - w, inner.top - w, inner.right + w, inner.top};
    edges[1] = {inner.left - w, inner.bottom, inner.right + w, inner.bottom + w};
    edges[2] = {inner.left - w, inner.top, inner.left, inner.bottom};
    edges[3] = {inner.right, inner.top, inner.right + w, inner.bottom};
}

class Overlay {
public:
    Overlay(const ccl::capture::ScreenSnapshot& snapshot,
            const ccl::capture::WindowList& windows, LONGLONG launchStart)
        : snapshot_(snapshot), windows_(windows), launchStart_(launchStart) {}

    ~Overlay();

    SelectionResult Run() noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateOverlayWindow(HINSTANCE instance) noexcept;
    bool CreateSurfaces() noexcept;

    void EraseOutline(const RECT& selection) noexcept;
    void DrawOutline(const RECT& selection) noexcept;
    void RedrawSelection() noexcept;

    RECT NormalizedSelection() const noexcept;
    RECT ScreenRect() const noexcept;
    void Finish(bool accepted) noexcept;

    // Whole-window pick. Returns false when no recorded window covers the
    // point, in which case the click is ignored.
    bool PickWindowAt(POINT point, bool excludeFrame) noexcept;
    std::wstring TitleAt(POINT point) const noexcept;
    POINT ToScreen(POINT point) const noexcept;

    const ccl::capture::ScreenSnapshot& snapshot_;
    const ccl::capture::WindowList& windows_;
    LONGLONG launchStart_ = 0;

    HWND hwnd_ = nullptr;
    HDC windowDc_ = nullptr;
    HDC snapshotDc_ = nullptr;
    HGDIOBJ previousSnapshot_ = nullptr;
    HBRUSH outlineBrush_ = nullptr;

    bool dragging_ = false;
    bool hasSelection_ = false;
    POINT anchor_{};
    POINT cursor_{};

    RECT drawnSelection_{};
    bool hasDrawn_ = false;

    bool finished_ = false;
    bool accepted_ = false;
    bool reportedFirstFrame_ = false;
    LONGLONG releasedAt_ = 0;
    ccl::timing::FrameStats frameStats_;

    bool pickedWindow_ = false;
    RECT pickedArea_{};
    std::wstring title_;
};

Overlay::~Overlay() {
    if (outlineBrush_ != nullptr) {
        ::DeleteObject(outlineBrush_);
    }
    if (snapshotDc_ != nullptr) {
        ::SelectObject(snapshotDc_, previousSnapshot_);
        ::DeleteDC(snapshotDc_);
    }
    if (windowDc_ != nullptr && hwnd_ != nullptr) {
        ::ReleaseDC(hwnd_, windowDc_);
    }
}

LRESULT CALLBACK Overlay::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        self->hwnd_ = hwnd;
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Overlay::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            ccl::timing::Stopwatch watch;
            PAINTSTRUCT ps{};
            const HDC dc = ::BeginPaint(hwnd_, &ps);
            if (snapshotDc_ != nullptr) {
                const RECT& r = ps.rcPaint;
                ::BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         snapshotDc_, r.left, r.top, SRCCOPY);
                if (hasDrawn_) {
                    DrawOutline(drawnSelection_);
                }
            }
            ::EndPaint(hwnd_, &ps);

            if (!reportedFirstFrame_) {
                reportedFirstFrame_ = true;
                watch.Lap(L"  overlay first paint");
                ccl::timing::Report(L"launch -> selection ready", launchStart_);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
            dragging_ = true;
            hasSelection_ = true;
            anchor_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            cursor_ = anchor_;
            ::SetCapture(hwnd_);
            RedrawSelection();
            return 0;

        case WM_MOUSEMOVE:
            if (dragging_) {
                cursor_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RedrawSelection();
            }
            return 0;

        case WM_LBUTTONUP: {
            if (!dragging_) {
                return 0;
            }
            const LONGLONG releasedAt = ccl::timing::Now();
            dragging_ = false;
            ::ReleaseCapture();
            cursor_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // A click rather than a drag picks the whole window under the
            // cursor; with Ctrl held, its client area only.
            if (std::abs(cursor_.x - anchor_.x) <= kClickThreshold &&
                std::abs(cursor_.y - anchor_.y) <= kClickThreshold) {
                if (PickWindowAt(anchor_, (wParam & MK_CONTROL) != 0)) {
                    releasedAt_ = releasedAt;
                    Finish(true);
                } else {
                    hasSelection_ = false;
                    RedrawSelection();
                }
                return 0;
            }

            const RECT area = NormalizedSelection();
            if (area.right - area.left <= 0 || area.bottom - area.top <= 0) {
                hasSelection_ = false;
                RedrawSelection();
                return 0;
            }

            // Name the capture after whatever sits under the middle of the
            // selection.
            title_ = TitleAt(POINT{(area.left + area.right) / 2,
                                   (area.top + area.bottom) / 2});
            releasedAt_ = releasedAt;
            Finish(true);
            return 0;
        }

        case WM_RBUTTONDOWN:
            Finish(false);
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                Finish(false);
            }
            return 0;

        case WM_CAPTURECHANGED:
            dragging_ = false;
            return 0;

        case WM_NCDESTROY:
            // Deliberately no PostQuitMessage here: the selection loop exits on
            // its own flag, and a stray WM_QUIT left in the queue would tear
            // down the capture window's message loop the moment it starts.
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            finished_ = true;
            break;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

RECT Overlay::ScreenRect() const noexcept {
    return RECT{0, 0, static_cast<LONG>(snapshot_.Width()),
                static_cast<LONG>(snapshot_.Height())};
}

RECT Overlay::NormalizedSelection() const noexcept {
    const RECT screen = ScreenRect();

    RECT area{};
    area.left = std::clamp(std::min(anchor_.x, cursor_.x), screen.left, screen.right);
    area.top = std::clamp(std::min(anchor_.y, cursor_.y), screen.top, screen.bottom);
    area.right = std::clamp(std::max(anchor_.x, cursor_.x), screen.left, screen.right);
    area.bottom = std::clamp(std::max(anchor_.y, cursor_.y), screen.top, screen.bottom);
    return area;
}

POINT Overlay::ToScreen(POINT point) const noexcept {
    const ccl::capture::VirtualScreen& bounds = snapshot_.Bounds();
    return POINT{point.x + bounds.left, point.y + bounds.top};
}

std::wstring Overlay::TitleAt(POINT point) const noexcept {
    const ccl::capture::WindowInfo* info = windows_.Hit(ToScreen(point));
    return info != nullptr ? info->title : std::wstring{};
}

bool Overlay::PickWindowAt(POINT point, bool excludeFrame) noexcept {
    const ccl::capture::WindowInfo* info = windows_.Hit(ToScreen(point));
    if (info == nullptr) {
        return false;
    }

    const RECT& source = excludeFrame ? info->client : info->frame;
    const ccl::capture::VirtualScreen& bounds = snapshot_.Bounds();

    RECT area{source.left - bounds.left, source.top - bounds.top,
              source.right - bounds.left, source.bottom - bounds.top};

    const RECT screen = ScreenRect();
    if (!Intersect(area, screen, area) || area.right <= area.left ||
        area.bottom <= area.top) {
        return false;
    }

    pickedArea_ = area;
    pickedWindow_ = true;
    title_ = info->title;
    return true;
}

void Overlay::Finish(bool accepted) noexcept {
    accepted_ = accepted;
    finished_ = true;
    if (dragging_) {
        dragging_ = false;
        ::ReleaseCapture();
    }
    ccl::timing::ReportFrames(L"selection drag", frameStats_);
    ::DestroyWindow(hwnd_);
}

void Overlay::EraseOutline(const RECT& selection) noexcept {
    const RECT screen = ScreenRect();

    RECT edges[4]{};
    OutlineEdges(selection, edges);

    for (const RECT& edge : edges) {
        RECT area{};
        if (Intersect(edge, screen, area)) {
            ::BitBlt(windowDc_, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, snapshotDc_, area.left, area.top,
                     SRCCOPY);
        }
    }
}

void Overlay::DrawOutline(const RECT& selection) noexcept {
    const RECT screen = ScreenRect();

    RECT edges[4]{};
    OutlineEdges(selection, edges);

    for (const RECT& edge : edges) {
        RECT area{};
        if (Intersect(edge, screen, area)) {
            ::FillRect(windowDc_, &area, outlineBrush_);
        }
    }
}

void Overlay::RedrawSelection() noexcept {
    if (windowDc_ == nullptr || snapshotDc_ == nullptr) {
        return;
    }
    const LONGLONG frameStart = ccl::timing::Now();

    // Only the outline changes between frames, so only the outline is touched:
    // restore the pixels the old one covered, then paint the new one. The work
    // per frame is a few thin strips regardless of how big the selection is.
    if (hasDrawn_) {
        EraseOutline(drawnSelection_);
    }

    const RECT selection = NormalizedSelection();
    if (hasSelection_) {
        DrawOutline(selection);
    }

    drawnSelection_ = selection;
    hasDrawn_ = hasSelection_;

    frameStats_.Add(ccl::timing::MillisecondsSince(frameStart));
}

bool Overlay::CreateOverlayWindow(HINSTANCE instance) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_OWNDC keeps one device context alive for the window so that dragging
    // does not pay for GetDC on every mouse move.
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = &Overlay::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kOverlayClass;

    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Created hidden so that the first thing shown is a finished frame.
    const ccl::capture::VirtualScreen& bounds = snapshot_.Bounds();
    hwnd_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass,
                              L"", WS_POPUP, bounds.left, bounds.top,
                              bounds.width, bounds.height, nullptr, nullptr,
                              instance, this);
    return hwnd_ != nullptr;
}

bool Overlay::CreateSurfaces() noexcept {
    windowDc_ = ::GetDC(hwnd_);
    if (windowDc_ == nullptr) {
        return false;
    }

    snapshotDc_ = ::CreateCompatibleDC(windowDc_);
    if (snapshotDc_ == nullptr) {
        return false;
    }
    previousSnapshot_ = ::SelectObject(snapshotDc_, snapshot_.Handle());

    outlineBrush_ = ::CreateSolidBrush(RGB(255, 255, 255));
    return outlineBrush_ != nullptr;
}

SelectionResult Overlay::Run() noexcept {
    SelectionResult result;

    ccl::timing::Stopwatch watch;
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!CreateOverlayWindow(instance)) {
        return result;
    }
    watch.Lap(L"  overlay window create");

    if (!CreateSurfaces()) {
        ::DestroyWindow(hwnd_);
        return result;
    }
    watch.Lap(L"  overlay surfaces");

    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    watch.Lap(L"  overlay show");

    MSG msg{};
    while (!finished_ && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    result.accepted = accepted_;
    if (accepted_) {
        result.area = pickedWindow_ ? pickedArea_ : NormalizedSelection();
        result.releasedAt = releasedAt_;
        result.title = title_;
    }
    return result;
}

}  // namespace

SelectionResult RunSelection(const ccl::capture::ScreenSnapshot& snapshot,
                             const ccl::capture::WindowList& windows,
                             LONGLONG launchStart) noexcept {
    if (!snapshot.IsValid()) {
        return SelectionResult{};
    }
    Overlay overlay(snapshot, windows, launchStart);
    return overlay.Run();
}

}  // namespace ccl::overlay

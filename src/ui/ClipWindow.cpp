#include "ui/ClipWindow.h"

#include <windowsx.h>

#include <algorithm>

#include "doc/Document.h"
#include "render/D2DContext.h"
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

// Title format. Placeholders are documented in NameFormat.h; this becomes a
// setting in a later phase.
constexpr wchar_t kTitleFormat[] = L"%t";

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
            OnKeyDown(wParam);
            return 0;

        case WM_MBUTTONDOWN:
            moving_ = true;
            ::GetCursorPos(&dragOrigin_);
            ::GetWindowRect(hwnd_, &windowOrigin_);
            ::SetCapture(hwnd_);
            return 0;

        case WM_LBUTTONDOWN:
            scrolling_ = true;
            ::GetCursorPos(&scrollOrigin_);
            scrollStart_ = view_.Scroll();
            ::SetCapture(hwnd_);
            return 0;

        case WM_MOUSEMOVE: {
            if (moving_) {
                POINT now{};
                ::GetCursorPos(&now);
                ::SetWindowPos(hwnd_, nullptr,
                               windowOrigin_.left + (now.x - dragOrigin_.x),
                               windowOrigin_.top + (now.y - dragOrigin_.y), 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (scrolling_) {
                POINT now{};
                ::GetCursorPos(&now);
                // Dragging moves the image itself, so the scroll offset goes
                // the opposite way to the cursor.
                view_.SetScroll(
                    POINT{scrollStart_.x - (now.x - scrollOrigin_.x),
                          scrollStart_.y - (now.y - scrollOrigin_.y)},
                    ContentSize(), ViewportSize());
                Draw();
            }
            return 0;
        }

        case WM_MBUTTONUP:
        case WM_LBUTTONUP:
            if (moving_ || scrolling_) {
                moving_ = false;
                scrolling_ = false;
                ::ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            moving_ = false;
            scrolling_ = false;
            return 0;

        case WM_RBUTTONDOWN:
            // Provisional: the context menu takes this over in a later phase.
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
    switch (key) {
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
    std::wstring name =
        ccl::util::ExpandPlaceholders(kTitleFormat, sourceTitle_);
    if (name.empty()) {
        name = L"CapturaClipA2";
    }

    wchar_t title[400];
    ::swprintf_s(title, L"%s  %d%%", name.c_str(),
                 static_cast<int>(std::lround(view_.Zoom() * 100.0f)));
    ::SetWindowTextW(hwnd_, title);
}

void ClipWindow::ApplyOpacity() noexcept {
    ::SetLayeredWindowAttributes(hwnd_, 0, view_.Opacity(), LWA_ALPHA);
}

void ClipWindow::FitToImage() noexcept {
    view_.SetScroll(POINT{0, 0}, ContentSize(), ViewportSize());
    ApplyZoom();
}

void ClipWindow::Draw() noexcept {
    renderer_.Draw(view_);

    if (!reportedFirstFrame_) {
        reportedFirstFrame_ = true;
        ccl::timing::Report(L"release -> window shown", releasedAt_);
    }
}

bool ClipWindow::Create(const ccl::render::D2DContext& context,
                        const ccl::doc::Document& document, POINT position,
                        const std::wstring& sourceTitle,
                        LONGLONG releasedAt) noexcept {
    if (!document.IsValid()) {
        return false;
    }
    releasedAt_ = releasedAt;
    document_ = &document;
    sourceTitle_ = sourceTitle;
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
    // The window is the image plus the outline on each side, and it is placed
    // so that the image itself lands exactly where the selection was.
    // WS_THICKFRAME makes the window sizable; WM_NCCALCSIZE then hides the
    // frame it would otherwise draw, and WM_NCHITTEST supplies the grips.
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
    ApplyOpacity();

    // Keyboard and wheel messages go to the focused window, so the capture has
    // to take focus for its shortcuts to work without clicking it first.
    ::SetForegroundWindow(hwnd_);
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

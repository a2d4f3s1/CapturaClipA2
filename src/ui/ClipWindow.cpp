#include "ui/ClipWindow.h"

#include "doc/Document.h"
#include "render/D2DContext.h"
#include "util/Timing.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kClipWindowClass[] = L"CapturaClipA2.ClipWindow";

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

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            Draw();
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_SIZE:
            renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_MBUTTONDOWN:
            moving_ = true;
            ::GetCursorPos(&dragOrigin_);
            ::GetWindowRect(hwnd_, &windowOrigin_);
            ::SetCapture(hwnd_);
            return 0;

        case WM_MOUSEMOVE: {
            if (!moving_) {
                return 0;
            }
            POINT now{};
            ::GetCursorPos(&now);
            ::SetWindowPos(hwnd_, nullptr,
                           windowOrigin_.left + (now.x - dragOrigin_.x),
                           windowOrigin_.top + (now.y - dragOrigin_.y), 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_MBUTTONUP:
            if (moving_) {
                moving_ = false;
                ::ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            moving_ = false;
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

void ClipWindow::Draw() noexcept {
    renderer_.Draw();

    if (!reportedFirstFrame_) {
        reportedFirstFrame_ = true;
        ccl::timing::Report(L"release -> window shown", releasedAt_);
    }
}

bool ClipWindow::Create(const ccl::render::D2DContext& context,
                        const ccl::doc::Document& document, POINT position,
                        LONGLONG releasedAt) noexcept {
    if (!document.IsValid()) {
        return false;
    }
    releasedAt_ = releasedAt;
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
    // title bar, so it is obvious whether a capture is still alive. Whether to
    // show it becomes a setting alongside the other appearance options.
    //
    // The window is the image plus the outline on each side, and it is placed
    // so that the image itself lands exactly where the selection was.
    const int frame = 2 * ccl::render::kWindowBorder;
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW, kClipWindowClass, L"CapturaClipA2",
        WS_POPUP, position.x - ccl::render::kWindowBorder,
        position.y - ccl::render::kWindowBorder, document.Width() + frame,
        document.Height() + frame, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    watch.Lap(L"  clip window create");

    renderer_.Attach(context, hwnd_);
    renderer_.SetDocument(&document);

    ::ShowWindow(hwnd_, SW_SHOW);
    Draw();
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

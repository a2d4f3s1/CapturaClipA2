#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "app/Settings.h"
#include "capture/ScreenSnapshot.h"
#include "capture/WindowPicker.h"
#include "doc/Document.h"
#include "io/ImageCodec.h"
#include "overlay/SelectionOverlay.h"
#include "render/D2DContext.h"
#include "ui/ClipWindow.h"
#include "util/Timing.h"

namespace {

void ReportFatal(const wchar_t* what) noexcept {
    ::MessageBoxW(nullptr, what, L"CapturaClipA2", MB_ICONERROR | MB_OK);
}

bool HasFlag(const wchar_t* commandLine, const wchar_t* flag) noexcept {
    return commandLine != nullptr && ::wcsstr(commandLine, flag) != nullptr;
}

// Milliseconds to wait before reading the screen, given as --prepare=<ms>.
// Used when the program relaunches itself: the window it just closed is still
// on screen until everything behind it has repainted.
UINT PreparationDelay(UINT configured) noexcept {
    constexpr wchar_t kSwitch[] = L"--prepare=";
    const wchar_t* found = ::wcsstr(::GetCommandLineW(), kSwitch);
    if (found == nullptr) {
        return configured;
    }
    return static_cast<UINT>(
        ::wcstoul(found + ARRAYSIZE(kSwitch) - 1, nullptr, 10));
}

// Waits for the process that started this one to exit, given as --wait=<pid>.
//
// Used when the program relaunches itself to capture again. Hiding the old
// window is not enough: the menu the command came from fades out over the
// screen, and that fade belongs to the old process, so nothing short of it
// exiting will clear it. Once it is gone, whatever it covered still has to
// repaint before the screen is worth reading.
void WaitForCaller() noexcept {
    constexpr wchar_t kSwitch[] = L"--wait=";
    const wchar_t* found = ::wcsstr(::GetCommandLineW(), kSwitch);
    if (found == nullptr) {
        return;
    }

    const auto id = static_cast<DWORD>(
        ::wcstoul(found + ARRAYSIZE(kSwitch) - 1, nullptr, 10));
    if (id == 0) {
        return;
    }

    const LONGLONG waitStart = ccl::timing::Now();
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, id);
    if (process != nullptr) {
        // Bounded, so a process that will not die cannot hang this one.
        ::WaitForSingleObject(process, 2000);
        ::CloseHandle(process);
    }
    ccl::timing::Report(process != nullptr ? L"caller exit wait"
                                           : L"caller already gone",
                        waitStart);

    // A null window is the documented way to ask every window on the desktop to
    // repaint, which is what has to happen before the screen is worth reading.
    ::InvalidateRect(nullptr, nullptr, TRUE);
}

// First argument that is not a switch, which is how a file dropped on the
// executable or opened through the shell arrives.
std::wstring FileArgument() noexcept {
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (argv == nullptr) {
        return {};
    }

    std::wstring path;
    for (int i = 1; i < count; ++i) {
        if (argv[i][0] != L'-' && argv[i][0] != L'/') {
            path = argv[i];
            break;
        }
    }
    ::LocalFree(argv);
    return path;
}

// Where a picture opened from a file should appear: centred on the monitor the
// pointer is on, since there is no selection rectangle to inherit a place from.
POINT CentredPosition(int width, int height) noexcept {
    POINT cursor{};
    ::GetCursorPos(&cursor);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                           &info)) {
        return POINT{0, 0};
    }

    const LONG areaWidth = info.rcWork.right - info.rcWork.left;
    const LONG areaHeight = info.rcWork.bottom - info.rcWork.top;
    return POINT{info.rcWork.left + std::max(0L, (areaWidth - width) / 2),
                 info.rcWork.top + std::max(0L, (areaHeight - height) / 2)};
}

// Shows a picture from disk instead of capturing one. Returns the exit code.
int RunWithFile(const std::wstring& path, ccl::app::Settings& settings,
                LONGLONG launchStart) noexcept {
    ccl::render::D2DContext context;
    if (!context.Initialize()) {
        ReportFatal(L"Failed to initialise Direct2D.");
        return 1;
    }

    ccl::doc::Document document{ccl::io::LoadImageFile(context, path)};
    if (!document.IsValid()) {
        ReportFatal(L"画像を読み込めませんでした。");
        return 1;
    }

    const wchar_t* name = ::wcsrchr(path.c_str(), L'\\');
    const std::wstring title = name != nullptr ? name + 1 : path;

    ccl::ui::ClipWindow window;
    if (!window.Create(context, document, settings,
                       CentredPosition(document.Width(), document.Height()),
                       title, launchStart)) {
        ReportFatal(L"Failed to create the capture window.");
        return 1;
    }

    // A file can be larger than the screen, which a capture never is.
    window.ResizeToImage();
    window.Run();
    return 0;
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                      _In_ int) {
    const LONGLONG launchStart = ccl::timing::Now();
    if (HasFlag(::GetCommandLineW(), L"--timing")) {
        ccl::timing::g_enabled = true;
    }
    const ccl::timing::ScopedFlush flushLogOnExit;
    ccl::timing::Stopwatch watch;

    ccl::app::Settings settings;
    settings.Load();
    watch.Lap(L"  settings load");

    // Started with a file: show that instead of capturing the screen.
    const std::wstring file = FileArgument();
    if (!file.empty()) {
        return RunWithFile(file, settings, launchStart);
    }

    // Before the window list, so that a capture being replaced is not among the
    // windows that a click could pick.
    WaitForCaller();

    // Recorded before the overlay covers the screen, so that clicking can pick
    // the window that was actually under the cursor.
    ccl::capture::WindowList windows;
    windows.Capture();
    watch.Lap(L"  window list");

    ccl::capture::ScreenSnapshot snapshot;
    if (!snapshot.Take(PreparationDelay(settings.preparationMs))) {
        ReportFatal(L"Failed to capture the screen.");
        return 1;
    }
    ccl::timing::Report(L"launch -> snapshot taken", launchStart);

    const ccl::overlay::SelectionResult selection =
        ccl::overlay::RunSelection(snapshot, windows, launchStart);
    if (!selection.accepted) {
        return 0;
    }

    // Direct2D is only needed once there is something to display, and it is not
    // needed at all if the selection is cancelled.
    watch = ccl::timing::Stopwatch{};
    ccl::render::D2DContext context;
    if (!context.Initialize()) {
        ReportFatal(L"Failed to initialise Direct2D.");
        return 1;
    }
    watch.Lap(L"  d2d factory init");

    // The only GPU-to-CPU read in the whole flow, and only for the pixels that
    // were actually selected.
    ccl::doc::Document document{snapshot.ExtractRegion(selection.area)};
    if (!document.IsValid()) {
        ReportFatal(L"Failed to extract the selected region.");
        return 1;
    }
    watch.Lap(L"  extract selection");

    // Place the window exactly where the selection was, so the capture appears
    // to stay put rather than jump somewhere else.
    const ccl::capture::VirtualScreen& bounds = snapshot.Bounds();
    const POINT position{bounds.left + selection.area.left,
                         bounds.top + selection.area.top};

    ccl::ui::ClipWindow window;
    if (!window.Create(context, document, settings, position, selection.title,
                       selection.releasedAt)) {
        ReportFatal(L"Failed to create the capture window.");
        return 1;
    }

    window.Run();
    return 0;
}

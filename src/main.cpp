#include <windows.h>

#include "app/Settings.h"
#include "capture/ScreenSnapshot.h"
#include "capture/WindowPicker.h"
#include "doc/Document.h"
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

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                      _In_ int) {
    const LONGLONG launchStart = ccl::timing::Now();
    if (HasFlag(::GetCommandLineW(), L"--no-timing")) {
        ccl::timing::g_enabled = false;
    }
    const ccl::timing::ScopedFlush flushLogOnExit;
    ccl::timing::Stopwatch watch;

    ccl::app::Settings settings;
    settings.Load();
    watch.Lap(L"  settings load");

    // Recorded before the overlay covers the screen, so that clicking can pick
    // the window that was actually under the cursor.
    ccl::capture::WindowList windows;
    windows.Capture();
    watch.Lap(L"  window list");

    ccl::capture::ScreenSnapshot snapshot;
    if (!snapshot.Take(settings.preparationMs)) {
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
    ccl::doc::Document document(snapshot.ExtractRegion(selection.area));
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

#pragma once

#include <windows.h>

namespace ccl::app {
class Settings;
}

namespace ccl::ui {

// Shows the settings window and blocks until it is closed.
//
// Returns true when the settings were accepted, in which case `settings` holds
// the new values and has already been written to disk. Returns false when it
// was cancelled, leaving `settings` untouched.
bool ShowSettingsDialog(HWND owner, ccl::app::Settings& settings) noexcept;

}  // namespace ccl::ui

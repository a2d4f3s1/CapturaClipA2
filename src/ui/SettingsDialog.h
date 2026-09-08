#pragma once

#include <windows.h>

namespace ccl::app {
class Settings;
}

struct IDWriteFactory;

namespace ccl::ui {

// Shows the settings window and blocks until it is closed.
//
// `writer` fills the font list. It is the same one the drawing uses, so that
// what can be chosen here is what can actually be drawn with -- the list used
// to come from GDI, which names things differently and offered families that
// do not exist as far as the drawing is concerned.
//
// Returns true when the settings were accepted, in which case `settings` holds
// the new values and has already been written to disk. Returns false when it
// was cancelled, leaving `settings` untouched.
bool ShowSettingsDialog(HWND owner, ccl::app::Settings& settings,
                        IDWriteFactory* writer) noexcept;

}  // namespace ccl::ui

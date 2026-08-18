#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "capture/DibBuffer.h"

namespace ccl::render {
class D2DContext;
}

namespace ccl::io {

// Whether the write should also clear out files past their retention.
//
// Skipped when the session is ending: moving files to the Recycle Bin is slow,
// every window would be asking for it on the same folder at the same moment,
// and Windows only waits so long before it stops asking and starts killing.
// The files it would have cleared are picked up by the next ordinary close.
enum class HistoryCleanup {
    Prune,
    Skip,
};

// Writes the image into the configured folder, creating it if needed.
//
// Returns the path written, or an empty string when auto-saving is switched
// off (no folder configured) or the write failed.
std::wstring AutoSaveImage(ccl::render::D2DContext& context,
                           const ccl::capture::DibBuffer& image,
                           const ccl::app::Settings& settings,
                           const std::wstring& title,
                           HistoryCleanup cleanup = HistoryCleanup::Prune) noexcept;

// Moves files older than the configured retention out of the auto-save folder.
//
// Deliberately conservative: it only ever touches the folder auto-save just
// wrote to, only files with the extensions this program writes, never
// subfolders, and it moves them to the Recycle Bin rather than deleting them,
// so a mis-set retention is recoverable.
void PruneAutoSaveHistory(const ccl::app::Settings& settings,
                          const std::wstring& folder) noexcept;

}  // namespace ccl::io

#pragma once

#include <windows.h>

#include "capture/DibBuffer.h"

namespace ccl::io {

// Puts the image on the clipboard as a device-independent bitmap.
bool CopyToClipboard(HWND owner, const ccl::capture::DibBuffer& image) noexcept;

// True when the clipboard holds something that can be pasted as an image.
bool ClipboardHasImage() noexcept;

// Reads an image off the clipboard. Returns an invalid buffer if there is none.
ccl::capture::DibBuffer PasteFromClipboard(HWND owner) noexcept;

}  // namespace ccl::io

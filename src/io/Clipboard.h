#pragma once

#include <windows.h>

#include "capture/DibBuffer.h"

namespace ccl::io {

// Puts the image on the clipboard as a device-independent bitmap.
bool CopyToClipboard(HWND owner, const ccl::capture::DibBuffer& image) noexcept;

}  // namespace ccl::io

#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "capture/DibBuffer.h"

namespace ccl::render {
class D2DContext;
}

namespace ccl::io {

// File extension for a format, without the leading dot.
const wchar_t* ExtensionFor(ccl::app::ImageFormat format) noexcept;

// Writes the image to disk. Returns false if anything goes wrong, including
// the folder not existing.
bool SaveImage(ccl::render::D2DContext& context,
               const ccl::capture::DibBuffer& image, const std::wstring& path,
               ccl::app::ImageFormat format, int jpegQuality) noexcept;

}  // namespace ccl::io

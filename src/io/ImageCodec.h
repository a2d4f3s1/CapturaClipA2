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

// Writes the image to disk, replacing whatever is at that path. Returns false
// if anything goes wrong, including the folder not existing.
bool SaveImage(ccl::render::D2DContext& context,
               const ccl::capture::DibBuffer& image, const std::wstring& path,
               ccl::app::ImageFormat format, int jpegQuality) noexcept;

// What came of trying to write a file that must not already exist.
enum class NewFileResult {
    Written,
    AlreadyExists,
    Failed,
};

// Writes the image only if nothing is at that path yet.
//
// Asking whether a name is free and then writing to it are one operation here,
// which is what makes it safe for two of these programs to be saving into the
// same folder at the same instant: the loser is told the name is taken rather
// than writing over what the winner just put there.
NewFileResult SaveImageAsNewFile(ccl::render::D2DContext& context,
                                 const ccl::capture::DibBuffer& image,
                                 const std::wstring& path,
                                 ccl::app::ImageFormat format,
                                 int jpegQuality) noexcept;

// Reads an image file. Returns an invalid buffer if it could not be read, which
// covers unsupported formats as well as missing files.
// Named LoadImageFile rather than LoadImage because the latter is a macro in
// the Windows headers, and it would silently rewrite the name here.
ccl::capture::DibBuffer LoadImageFile(ccl::render::D2DContext& context,
                                      const std::wstring& path) noexcept;

// File dialog filter for the formats that can be read.
extern const wchar_t* const kOpenFilter;

}  // namespace ccl::io

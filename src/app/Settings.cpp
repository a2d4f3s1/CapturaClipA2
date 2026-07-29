#include "app/Settings.h"

#include <cstdio>
#include <cstdlib>

namespace ccl::app {
namespace {

constexpr wchar_t kFileName[] = L"CapturaClipA2.ini";

std::wstring ReadString(const wchar_t* section, const wchar_t* key,
                        const std::wstring& fallback,
                        const std::wstring& path) noexcept {
    wchar_t buffer[512];
    const DWORD length =
        ::GetPrivateProfileStringW(section, key, fallback.c_str(), buffer,
                                   ARRAYSIZE(buffer), path.c_str());
    return std::wstring(buffer, length);
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback,
                const std::wstring& path) noexcept {
    wchar_t fallbackText[32];
    ::swprintf_s(fallbackText, L"%g", fallback);

    const std::wstring text = ReadString(section, key, fallbackText, path);
    wchar_t* end = nullptr;
    const float value = ::wcstof(text.c_str(), &end);
    return end != text.c_str() ? value : fallback;
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback,
              const std::wstring& path) noexcept {
    return ::GetPrivateProfileIntW(section, key, fallback ? 1 : 0,
                                   path.c_str()) != 0;
}

const wchar_t* FormatName(ImageFormat format) noexcept {
    switch (format) {
        case ImageFormat::Jpeg: return L"JPEG";
        case ImageFormat::Bmp: return L"BMP";
        case ImageFormat::Png:
        default: return L"PNG";
    }
}

ImageFormat ParseFormat(const std::wstring& name, ImageFormat fallback) noexcept {
    if (::_wcsicmp(name.c_str(), L"JPEG") == 0 ||
        ::_wcsicmp(name.c_str(), L"JPG") == 0) {
        return ImageFormat::Jpeg;
    }
    if (::_wcsicmp(name.c_str(), L"BMP") == 0) {
        return ImageFormat::Bmp;
    }
    if (::_wcsicmp(name.c_str(), L"PNG") == 0) {
        return ImageFormat::Png;
    }
    return fallback;
}

}  // namespace

void Settings::ResolvePath() noexcept {
    path_.clear();

    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }

    wchar_t* lastSlash = ::wcsrchr(path, L'\\');
    if (lastSlash == nullptr) {
        return;
    }
    lastSlash[1] = L'\0';

    path_ = path;
    path_ += kFileName;
}

void Settings::Load() noexcept {
    ResolvePath();
    if (path_.empty()) {
        return;
    }

    if (::GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First run: leave the defaults in place and write them out so the
        // file can be edited by hand.
        Save();
        return;
    }

    preparationMs = ::GetPrivateProfileIntW(L"Capture", L"PreparationMs",
                                            static_cast<INT>(preparationMs),
                                            path_.c_str());
    copyOnCapture = ReadBool(L"Capture", L"CopyOnCapture", copyOnCapture, path_);

    titleFormat = ReadString(L"Appearance", L"TitleFormat", titleFormat, path_);
    smoothScaling = ReadBool(L"Appearance", L"SmoothScaling", smoothScaling, path_);
    zoomStepPercent =
        ReadFloat(L"Appearance", L"ZoomStepPercent", zoomStepPercent, path_);

    defaultFormat = ParseFormat(
        ReadString(L"Save", L"DefaultFormat", FormatName(defaultFormat), path_),
        defaultFormat);
    jpegQuality =
        ::GetPrivateProfileIntW(L"Save", L"JpegQuality", jpegQuality, path_.c_str());

    autoSaveFolder = ReadString(L"AutoSave", L"Folder", autoSaveFolder, path_);
    autoSaveHistoryDays = ::GetPrivateProfileIntW(
        L"AutoSave", L"HistoryDays", autoSaveHistoryDays, path_.c_str());

    // Guard against values edited outside their valid range.
    if (jpegQuality < 0) jpegQuality = 0;
    if (jpegQuality > 100) jpegQuality = 100;
    if (autoSaveHistoryDays < 0) autoSaveHistoryDays = 0;
    if (zoomStepPercent < 1.0f) zoomStepPercent = 1.0f;
    if (zoomStepPercent > 100.0f) zoomStepPercent = 100.0f;
}

void Settings::Save() const noexcept {
    if (path_.empty()) {
        return;
    }

    // Written by hand rather than through WritePrivateProfileString so the
    // file can carry comments explaining each setting. UTF-16 with a BOM is
    // what the profile API expects for non-ASCII content.
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path_.c_str(), L"w, ccs=UTF-16LE") != 0 ||
        file == nullptr) {
        return;
    }

    ::fwprintf(file,
               L"; CapturaClipA2 settings\n"
               L"; Edit by hand; the file is re-read on every launch.\n"
               L"\n"
               L"[Capture]\n"
               L"; Milliseconds to wait before grabbing the screen.\n"
               L"; Useful when a menu is still fading out. 0 grabs immediately.\n"
               L"PreparationMs=%u\n"
               L"; Copy every capture to the clipboard automatically.\n"
               L"CopyOnCapture=%d\n"
               L"\n"
               L"[Appearance]\n"
               L"; Placeholders: %%y year, %%Y year (2 digits), %%m month, %%d day,\n"
               L";               %%h hour, %%n minute, %%s second, %%t source name\n"
               L"TitleFormat=%s\n"
               L"; Smooth interpolation when zooming. 0 uses nearest neighbour,\n"
               L"; which is what you want for inspecting individual pixels.\n"
               L"SmoothScaling=%d\n"
               L"; Zoom change per wheel notch, in percent.\n"
               L"ZoomStepPercent=%g\n"
               L"\n"
               L"[Save]\n"
               L"; PNG, JPEG or BMP\n"
               L"DefaultFormat=%s\n"
               L"; JPEG quality, 0-100. Higher is better looking and larger.\n"
               L"JpegQuality=%d\n"
               L"\n"
               L"[AutoSave]\n"
               L"; Folder to save captures into automatically. Empty disables it.\n"
               L"; Accepts the same placeholders as TitleFormat, so a value like\n"
               L"; C:\\Captures\\%%y-%%m files captures by month.\n"
               L"Folder=%s\n"
               L"; Days to keep automatically saved files. 0 keeps them forever.\n"
               L"; Expired files go to the Recycle Bin rather than being deleted.\n"
               L"HistoryDays=%d\n",
               preparationMs, copyOnCapture ? 1 : 0, titleFormat.c_str(),
               smoothScaling ? 1 : 0, zoomStepPercent, FormatName(defaultFormat),
               jpegQuality, autoSaveFolder.c_str(), autoSaveHistoryDays);

    ::fclose(file);
}

}  // namespace ccl::app

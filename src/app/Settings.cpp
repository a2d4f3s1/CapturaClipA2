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

// Colours are stored as #RRGGBB, which is the form anyone editing the file by
// hand will already know.
std::wstring ColorText(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) -> int {
        const float scaled = (value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value);
        return static_cast<int>(scaled * 255.0f + 0.5f);
    };
    wchar_t text[16];
    ::swprintf_s(text, L"#%02X%02X%02X", channel(color.r), channel(color.g),
                 channel(color.b));
    return text;
}

ccl::doc::Color ParseColor(const std::wstring& text,
                           const ccl::doc::Color& fallback) noexcept {
    const wchar_t* digits = text.c_str();
    if (*digits == L'#') {
        ++digits;
    }

    wchar_t* end = nullptr;
    const unsigned long packed = ::wcstoul(digits, &end, 16);
    if (end == digits || (end - digits) != 6) {
        return fallback;
    }

    return ccl::doc::Color{
        static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
        static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
        static_cast<float>(packed & 0xFF) / 255.0f, 1.0f};
}

const wchar_t* FrameName(WindowFrame frame) noexcept {
    switch (frame) {
        case WindowFrame::Normal: return L"Normal";
        case WindowFrame::ThinTitleBar: return L"ThinTitleBar";
        case WindowFrame::NoFrame: return L"NoFrame";
        case WindowFrame::NoTitleBar:
        default: return L"NoTitleBar";
    }
}

WindowFrame ParseFrame(const std::wstring& name, WindowFrame fallback) noexcept {
    if (::_wcsicmp(name.c_str(), L"Normal") == 0) return WindowFrame::Normal;
    if (::_wcsicmp(name.c_str(), L"ThinTitleBar") == 0) {
        return WindowFrame::ThinTitleBar;
    }
    if (::_wcsicmp(name.c_str(), L"NoTitleBar") == 0) {
        return WindowFrame::NoTitleBar;
    }
    if (::_wcsicmp(name.c_str(), L"NoFrame") == 0) return WindowFrame::NoFrame;
    return fallback;
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

    textFontFamily = ReadString(L"Text", L"FontFamily", textFontFamily, path_);
    textFontSize = ReadFloat(L"Text", L"FontSize", textFontSize, path_);
    textShadow = ReadBool(L"Text", L"Shadow", textShadow, path_);
    textOutline = ReadBool(L"Text", L"Outline", textOutline, path_);

    usePenPressure = ReadBool(L"Drawing", L"UsePenPressure", usePenPressure, path_);
    pressureMinScale =
        ReadFloat(L"Drawing", L"PressureMinScale", pressureMinScale, path_);
    penWidth = ReadFloat(L"Drawing", L"PenWidth", penWidth, path_);
    penColor = ParseColor(
        ReadString(L"Drawing", L"PenColor", ColorText(penColor), path_), penColor);
    eraserWidth = ReadFloat(L"Drawing", L"EraserWidth", eraserWidth, path_);

    for (size_t i = 0; i < quickColors.size(); ++i) {
        wchar_t key[16];
        ::swprintf_s(key, L"Color%zu", i + 1);
        quickColors[i] = ParseColor(
            ReadString(L"Colors", key, ColorText(quickColors[i]), path_),
            quickColors[i]);
    }

    titleFormat = ReadString(L"Appearance", L"TitleFormat", titleFormat, path_);
    smoothScaling = ReadBool(L"Appearance", L"SmoothScaling", smoothScaling, path_);
    zoomStepPercent =
        ReadFloat(L"Appearance", L"ZoomStepPercent", zoomStepPercent, path_);
    paletteScalePercent = ::GetPrivateProfileIntW(
        L"Appearance", L"PaletteScalePercent", paletteScalePercent, path_.c_str());
    windowFrame = ParseFrame(
        ReadString(L"Appearance", L"WindowFrame", FrameName(windowFrame), path_),
        windowFrame);

    defaultFormat = ParseFormat(
        ReadString(L"Save", L"DefaultFormat", FormatName(defaultFormat), path_),
        defaultFormat);
    jpegQuality =
        ::GetPrivateProfileIntW(L"Save", L"JpegQuality", jpegQuality, path_.c_str());

    shortcuts.Load(path_);

    autoSaveFolder = ReadString(L"AutoSave", L"Folder", autoSaveFolder, path_);
    autoSaveHistoryDays = ::GetPrivateProfileIntW(
        L"AutoSave", L"HistoryDays", autoSaveHistoryDays, path_.c_str());

    Clamp();
}

void Settings::ResetToDefaults() noexcept {
    // Everything except where the file lives, which is not a setting.
    std::wstring path = std::move(path_);
    *this = Settings{};
    path_ = std::move(path);
}

void Settings::Clamp() noexcept {
    if (jpegQuality < 0) jpegQuality = 0;
    if (jpegQuality > 100) jpegQuality = 100;
    if (autoSaveHistoryDays < 0) autoSaveHistoryDays = 0;
    if (zoomStepPercent < 1.0f) zoomStepPercent = 1.0f;
    if (zoomStepPercent > 100.0f) zoomStepPercent = 100.0f;
    if (paletteScalePercent < 50) paletteScalePercent = 50;
    if (paletteScalePercent > 300) paletteScalePercent = 300;
    if (pressureMinScale < 0.0f) pressureMinScale = 0.0f;
    if (pressureMinScale > 1.0f) pressureMinScale = 1.0f;

    // Same bounds the brush enforces at runtime.
    const auto clampWidth = [](float& width) {
        if (width < 1.0f) width = 1.0f;
        if (width > 200.0f) width = 200.0f;
    };
    clampWidth(penWidth);
    clampWidth(eraserWidth);

    if (textFontSize < 4.0f) textFontSize = 4.0f;
    if (textFontSize > 400.0f) textFontSize = 400.0f;
    if (textFontFamily.empty()) textFontFamily = L"Meiryo";
    // A capture that waits minutes before grabbing the screen is a hang, not a
    // setting.
    if (preparationMs > 10000) preparationMs = 10000;
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
               L"[Text]\n"
               L"; Font used for text annotations.\n"
               L"FontFamily=%s\n"
               L"; Size in image pixels.\n"
               L"FontSize=%g\n"
               L"; A shadow or an outline keeps text readable over a busy\n"
               L"; screenshot. Both can be on at once.\n"
               L"Shadow=%d\n"
               L"Outline=%d\n"
               L"\n"
               L"[Drawing]\n"
               L"; Vary stroke width with pen pressure. Needs a pressure-\n"
               L"; sensitive pen; a mouse always draws at full width.\n"
               L"UsePenPressure=%d\n"
               L"; Width at the lightest touch, as a fraction of the brush\n"
               L"; width. 0.15 keeps a faint line rather than nothing at all.\n"
               L"PressureMinScale=%g\n"
               L"; Starting size and colour of the brush. The eraser keeps its\n"
               L"; own size, since it is usually wanted much wider than the\n"
               L"; line it is rubbing out.\n"
               L"PenWidth=%g\n"
               L"PenColor=%s\n"
               L"EraserWidth=%g\n"
               L"\n"
               L"[Colors]\n"
               L"; The eight colours on Shift+1..8, which are also the fixed\n"
               L"; top row of the palette.\n"
               L"Color1=%s\n"
               L"Color2=%s\n"
               L"Color3=%s\n"
               L"Color4=%s\n"
               L"Color5=%s\n"
               L"Color6=%s\n"
               L"Color7=%s\n"
               L"Color8=%s\n"
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
               L"; Size of the colour palette popup, in percent. 50-300.\n"
               L"PaletteScalePercent=%d\n"
               L"; How much window frame a capture gets. Takes effect on the\n"
               L"; next capture rather than the one already on screen.\n"
               L";   Normal        ordinary caption and resizing frame\n"
               L";   ThinTitleBar  narrow caption, close button only, no\n"
               L";                 taskbar button\n"
               L";   NoTitleBar    outline only\n"
               L";   NoFrame       nothing at all\n"
               L"; Without a title bar there is nothing to drag, so the middle\n"
               L"; button moves the window instead.\n"
               L"WindowFrame=%s\n"
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
               L"HistoryDays=%d\n"
               L"\n"
               L"[Shortcuts]\n"
               L"; Written as Ctrl+S, Shift+F1, B and so on. An empty value\n"
               L"; leaves the command with no key at all.\n"
               L"%s",
               preparationMs, copyOnCapture ? 1 : 0, textFontFamily.c_str(),
               textFontSize, textShadow ? 1 : 0, textOutline ? 1 : 0,
               usePenPressure ? 1 : 0, pressureMinScale, penWidth,
               ColorText(penColor).c_str(), eraserWidth,
               ColorText(quickColors[0]).c_str(),
               ColorText(quickColors[1]).c_str(),
               ColorText(quickColors[2]).c_str(),
               ColorText(quickColors[3]).c_str(),
               ColorText(quickColors[4]).c_str(),
               ColorText(quickColors[5]).c_str(),
               ColorText(quickColors[6]).c_str(),
               ColorText(quickColors[7]).c_str(), titleFormat.c_str(),
               smoothScaling ? 1 : 0, zoomStepPercent, paletteScalePercent,
               FrameName(windowFrame), FormatName(defaultFormat), jpegQuality,
               autoSaveFolder.c_str(), autoSaveHistoryDays,
               shortcuts.ToFileText().c_str());

    ::fclose(file);
}

}  // namespace ccl::app

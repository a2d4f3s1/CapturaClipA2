#pragma once

#include <windows.h>

#include <string>

namespace ccl::app {

enum class ImageFormat {
    Png,
    Jpeg,
    Bmp,
};

// User settings, stored in an ini file next to the executable so the whole
// thing stays portable.
//
// The file is written out with comments on first run, which makes it editable
// by hand until the settings dialog exists.
class Settings {
public:
    // Capture
    UINT preparationMs = 0;
    bool copyOnCapture = false;

    // Appearance
    std::wstring titleFormat = L"%t";
    bool smoothScaling = true;
    float zoomStepPercent = 10.0f;
    // Size of the colour palette popup, as a percentage of its normal size.
    int paletteScalePercent = 100;

    // Saving
    ImageFormat defaultFormat = ImageFormat::Png;
    int jpegQuality = 75;  // 0-100
    // No PNG compression setting: the imaging component built into Windows,
    // which is what removes the need to ship a PNG library, does not expose a
    // compression level. It would take bundling one to offer the setting.

    // Automatic saving. An empty folder disables it entirely.
    std::wstring autoSaveFolder;
    // Days to keep automatically saved files; 0 keeps them forever.
    int autoSaveHistoryDays = 0;

    // Loads from disk, writing a commented default file if none exists.
    void Load() noexcept;
    void Save() const noexcept;

    const std::wstring& FilePath() const noexcept { return path_; }

private:
    void ResolvePath() noexcept;

    std::wstring path_;
};

}  // namespace ccl::app

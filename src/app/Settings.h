#pragma once

#include <windows.h>

#include <array>
#include <string>

#include "app/MouseBindings.h"
#include "app/Shortcuts.h"
#include "doc/Annotation.h"

namespace ccl::app {

enum class ImageFormat {
    Png,
    Jpeg,
    Bmp,
};

// What stays still while the zoom changes.
//
// Not a matter of one being right: holding the corner keeps the window where
// it is and is the quietest, while holding the pointer is what lets you aim at
// a detail and go in on it, at the cost of the window moving to make it true.
enum class ZoomAnchor {
    TopLeft,  // the corner of the visible area, so nothing moves but the size
    Cursor,   // the pixel under the pointer, for going in on something
    Center,   // the middle of the visible area
};

// How much of a window frame the capture gets. Applied when the window is
// created, so a change takes effect on the next capture rather than this one.
enum class WindowFrame {
    Normal,        // ordinary caption and resizing frame
    ThinTitleBar,  // narrow caption with only a close button, no taskbar button
    NoTitleBar,    // outline only, which is what suits a capture pinned on top
    NoFrame,       // nothing at all, so the capture floats without an edge
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

    // Text
    std::wstring textFontFamily = L"Meiryo";
    float textFontSize = 30.0f;
    // An outline reads better than a shadow over a screenshot, and does not
    // shift the text's apparent position the way a shadow does.
    bool textShadow = false;
    bool textOutline = true;

    // Drawing
    bool usePenPressure = true;
    // Width at zero pressure, as a fraction of the nominal brush width. Keeps a
    // light touch from thinning to nothing.
    float pressureMinScale = 0.15f;
    float penWidth = 4.0f;
    ccl::doc::Color penColor = ccl::doc::kDefaultQuickColors[1];  // green
    // The eraser keeps its own size: it is usually wanted much wider than the
    // line it is rubbing out.
    float eraserWidth = 24.0f;

    // Colours
    ccl::doc::QuickColors quickColors = ccl::doc::kDefaultQuickColors;

    // Keys and mouse gestures
    Shortcuts shortcuts;
    MouseBindings mouse;

    // Appearance
    std::wstring titleFormat = L"%t";
    // How long the window stays out of the way when asked to hide. It comes
    // back on a timer rather than when a key is released: a hidden window has
    // no keyboard focus, so nothing would tell it the key had been let go.
    UINT hideDurationMs = 3000;
    bool smoothScaling = true;
    float zoomStepPercent = 10.0f;
    // Size of the colour palette popup, as a percentage of its normal size.
    int paletteScalePercent = 100;
    WindowFrame windowFrame = WindowFrame::NoTitleBar;
    ZoomAnchor zoomAnchor = ZoomAnchor::TopLeft;

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

    // Pulls every value back into its valid range. Applied after loading, since
    // the file can be edited by hand, and after the settings window, since a
    // typed number can be anything at all.
    void Clamp() noexcept;

    // Back to the shipped defaults, keeping the file this was loaded from.
    void ResetToDefaults() noexcept;

    const std::wstring& FilePath() const noexcept { return path_; }

private:
    void ResolvePath() noexcept;

    std::wstring path_;
};

}  // namespace ccl::app

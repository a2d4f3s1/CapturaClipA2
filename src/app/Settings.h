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

// What stays still when turning the picture by a free angle makes it larger.
//
// No pointer option here, unlike the zoom: a turn is not aimed anywhere, so
// there is nothing under the cursor for it to hold onto.
enum class RotateAnchor {
    TopLeft,  // the corner of the window; it grows down and to the right
    Center,   // the middle of the window; it grows evenly on all sides
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
    // Whole pixels at 100%, independent of the size of the text: an edge that
    // grew with the font came out at two different weights depending on where
    // the size had been changed from.
    //
    // The shadow is thrown one of nine ways: 0 to 7 clockwise from straight up,
    // and 8 straight underneath, where only its spread shows. How far it
    // spreads follows the distance thrown, so there is nothing to set for it.
    float textOutlineWidth = 2.0f;
    float textShadowLength = 2.0f;
    int textShadowDirection = 3;
    // The colour is held without its strength because the settings file writes
    // colours as six hex digits and has nowhere to put an alpha. The two are
    // put together when a piece of text is given them.
    ccl::doc::Color textShadowColor{0.0f, 0.0f, 0.0f, 1.0f};
    float textShadowOpacity = 100.0f;  // percent
    // The edge is drawn in this. Kept whole, unlike the shadow's, because an
    // edge is either there or it is not: a half-transparent one lets the busy
    // background it is there to hide come through.
    ccl::doc::Color textOutlineColor{0.0f, 0.0f, 0.0f, 1.0f};

    // The two halves above put back together, which is the form everything
    // outside this class wants.
    ccl::doc::Color ShadowColor() const noexcept {
        ccl::doc::Color colour = textShadowColor;
        colour.a = textShadowOpacity / 100.0f;
        return colour;
    }

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
    // Angle a straight line is pulled to while Alt is held, in degrees. Zero is
    // a real value: it leaves the line free at any angle, which is what it does
    // without Alt anyway.
    float lineSnapDegrees = 15.0f;
    // The arrowhead R puts on a line, measured against its own width so that
    // changing the brush moves all three together.
    //   scale     how much wider than the line the head is
    //   aspect    how long the head is for its width
    //   rounding  how far its corners are taken off
    float arrowScale = 3.0f;
    float arrowAspect = 1.2f;
    float arrowRounding = 0.15f;
    // How far one press of Ctrl with an up or down arrow turns the head just
    // placed.
    float arrowTurnDegrees = 5.0f;

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
    // How far past a piece a press still takes hold of it, in screen pixels.
    // Screen rather than picture pixels so that reaching for a line feels the
    // same however far the picture is zoomed. The mark drawn round what is
    // picked is exactly this wide, so what is shown is what can be held --
    // widening the reach widens the mark with it.
    float grabSlack = 3.0f;
    // The gap left between the two pictures when one is joined onto another,
    // in picture pixels. Only the starting value: the join itself is set up in
    // its own window, where it can be changed for that one join.
    float concatMargin = 0.0f;
    ZoomAnchor zoomAnchor = ZoomAnchor::TopLeft;
    // The middle by default: a turn works about the middle of the picture, so
    // keeping that still is what matches what was just watched happening.
    RotateAnchor rotateAnchor = RotateAnchor::Center;

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

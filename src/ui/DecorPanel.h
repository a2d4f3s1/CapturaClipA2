#pragma once

#include <windows.h>

#include <functional>

#include "doc/Annotation.h"

namespace ccl::ui {

// What a piece of text is dressed in: the edge round it and the shadow under
// it. The panel shows these and hands back whichever one was touched.
struct DecorValues {
    bool outline = true;
    float outlineWidth = 2.0f;
    ccl::doc::Color outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    bool shadow = false;
    float shadowLength = 2.0f;
    int shadowDirection = 3;
    ccl::doc::Color shadowColor{0.0f, 0.0f, 0.0f, 1.0f};
    float shadowOpacity = 100.0f;  // percent, kept out of the colour's alpha
};

// The eight settings on one small panel, so that dressing a piece of text does
// not mean opening the menu eight times.
//
// Deliberately not a dialog and deliberately not hand-painted: it is opened in
// the middle of drawing, so it has no buttons, and every row is a stock control
// so that a checkbox behaves like a checkbox and a list drops down like a list.
// It stays open while things are adjusted and closes when the pointer goes
// somewhere else, which is what the colour palette does.
//
// Nothing is applied here. Each change is handed back, and the caller applies
// it exactly as it applies the same change from the menu -- which is where the
// undo step is recorded and where "the piece being pointed at, failing that the
// next one" is decided. That keeps one rule in one place.
class DecorPanel {
public:
    enum class Field {
        Outline,
        OutlineWidth,
        OutlineColor,
        Shadow,
        ShadowLength,
        ShadowDirection,
        ShadowColor,
        ShadowOpacity,
    };

    // Fired as soon as a row is touched, with everything the panel is showing.
    using Changed = std::function<void(Field, const DecorValues&)>;
    // The two colour rows. The caller opens the palette, and must open it with
    // the handle given here as its owner: a window owned by this one counts as
    // part of it, and anything else counts as the pointer having gone away.
    using PickColour = std::function<void(Field, HWND owner)>;

    // Runs until the panel closes. `screen` is where to put its top-left
    // corner, nudged to stay on the monitor it lands on.
    void Show(HWND owner, POINT screen, const DecorValues& values,
              int scalePercent, Changed onChange, PickColour onPick) noexcept;

    // Puts the controls back in step with the caller's values, for after a
    // change the caller adjusted -- a number pulled into range, or a colour
    // chosen through the palette.
    void Refresh(const DecorValues& values) noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    static LRESULT CALLBACK NumberProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR data);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Build(int scalePercent) noexcept;
    void ReadNumbers() noexcept;
    void Notify(Field field) noexcept;
    void Step(int id, int by) noexcept;
    // Whether the window taking over is one of ours: this panel, one of its
    // controls, or something a control put up (a dropped list, the palette).
    bool Ours(HWND taking) const noexcept;
    void Close() noexcept;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    DecorValues values_{};
    Changed onChange_;
    PickColour onPick_;
    bool closing_ = false;
    // Set while the panel itself writes into the boxes, so that the change
    // notifications that causes are not reported back as the user typing.
    bool filling_ = false;
};

}  // namespace ccl::ui

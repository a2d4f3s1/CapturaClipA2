#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

struct IDWriteFactory;

namespace ccl::ui {

// One installed family, under both of the names it answers to.
//
// Both are matched against, so that a family written in Japanese can be reached
// without opening the IME: of the 63 names that carry Japanese, 62 have an
// English one that differs -- "yu" finds 游ゴシック, "meiryo" finds メイリオ.
struct FontEntry {
    std::wstring shown;    // what the list shows, in the user's language
    std::wstring english;  // en-us name, empty when the family offers none
};

// Every family DirectWrite can lay text out with, sorted by the shown name.
//
// The one list, used by both the picker window and the settings dialog. They
// each had their own before -- the dialog asked GDI, which answers with faces
// rather than families and cuts names at 31 characters, so it offered 372
// names DirectWrite has no family for and left out 44 families that it does
// (源ノ角ゴシック JP and 小塚ゴシック Pro among them). Drawing goes through
// DirectWrite, so DirectWrite decides what can be chosen.
//
// Built once and kept: the families do not change while the program runs, and
// the first call is what pays for it. Not called on the way to the first
// capture -- the list is only wanted once someone asks for a font.
const std::vector<FontEntry>& InstalledFonts(IDWriteFactory* writer);

// The installed fonts as a small window: a box to type into over a list.
//
// Deliberately not a menu. A menu item cannot hold something to type into, so
// the most a menu can offer is jumping by first letter, which does not reach a
// name like "Yu Gothic". Laid out in columns it ran to thirteen of them and
// filled the screen; dropped to one column it became a list of hundreds to
// scroll. Typing part of the name is the way out of both.
//
// Deliberately built from stock controls rather than painted. A LISTBOX brings
// its own scrolling, arrow keys, wheel and IME, and there are several hundred
// rows to bring them to.
class FontPicker {
public:
    // Runs until the window closes. Returns the family chosen, or nothing if it
    // was dismissed. `current` is ticked and scrolled to when it is in the
    // list; empty means nothing is in force -- which is what a selection
    // spanning two faces reports.
    std::optional<std::wstring> Show(HWND owner, POINT screen,
                                     const std::vector<FontEntry>& fonts,
                                     const std::wstring& current,
                                     int scalePercent) noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    static LRESULT CALLBACK FilterProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR data);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Lays the window out around the longest name in the list, then pulls it
    // back onto the monitor it landed on.
    void Build(POINT screen, int scalePercent) noexcept;
    // Fills the list with whatever matches what has been typed, keeping the
    // row that was picked out if it survives the filter.
    void Refilter() noexcept;
    // The row under the caret in the list, or nothing when the filter left the
    // list empty -- pressing Enter against no rows must not choose anything.
    std::optional<std::wstring> Selected() const noexcept;
    // Whether the window taking over is one of ours: this window, one of its
    // controls, or something a control put up.
    bool Ours(HWND taking) const noexcept;
    void Close(bool accepted) noexcept;

    HWND hwnd_ = nullptr;
    HWND filter_ = nullptr;
    HWND list_ = nullptr;
    HFONT font_ = nullptr;

    const std::vector<FontEntry>* fonts_ = nullptr;
    std::wstring current_;
    // Focus moving before the window has settled is part of opening it, not the
    // user clicking away, and must not close it again immediately.
    bool ready_ = false;
    bool finished_ = false;
    bool accepted_ = false;
};

}  // namespace ccl::ui

#pragma once

#include <windows.h>

#include <array>
#include <string>

namespace ccl::app {

// Commands that can be reached by a key on their own.
//
// Deliberately not every key the window responds to. Some keys are modal rather
// than a command -- the size keys retarget while a straight line is being drawn,
// space scrolls only while it is held, the digits are zoom presets and Shift
// with a digit picks a colour. Those read the keyboard directly and are not
// listed here, because "which command is this key bound to" has no single
// answer for them.
enum class Command {
    Undo,
    Redo,
    Save,
    Copy,
    Open,
    Paste,
    ToolView,
    ToolPen,
    ToolEraser,
    ToolText,
    ToolSelect,
    Eyedropper,
    ColorPicker,
    Highlighter,
    Antialias,
    FitToImage,
    HideWindow,
    Count,
};

// A key with its modifiers. A key of zero means the command has no binding.
struct Binding {
    UINT key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool IsSet() const noexcept { return key != 0; }
    bool operator==(const Binding& other) const noexcept {
        return key == other.key && ctrl == other.ctrl && shift == other.shift &&
               alt == other.alt;
    }
};

// What a binding is written as, both in the settings file and in the menu:
// "Ctrl+S", "Shift+F1", "B". Empty for an unbound command.
std::wstring BindingText(const Binding& binding) noexcept;
// The inverse. Returns an unset binding for anything it cannot make sense of.
Binding ParseBinding(const std::wstring& text) noexcept;

// Name shown in the settings list.
const wchar_t* CommandLabel(Command command) noexcept;
// Key used in the settings file, which stays stable even if the label changes.
const wchar_t* CommandKey(Command command) noexcept;

class Shortcuts {
public:
    Shortcuts() noexcept { ResetToDefaults(); }

    const Binding& For(Command command) const noexcept {
        return bindings_[static_cast<size_t>(command)];
    }

    // Plain assignment: a key already in use is left where it is and the clash
    // is reported instead. Taking it off the other command quietly is a change
    // no one sees until that other command is missed.
    void Set(Command command, const Binding& binding) noexcept;
    void Clear(Command command) noexcept;

    // True when another command has been given the same key. Two commands
    // sharing a binding would make one of them unreachable, and which one is
    // pure luck, so this is what the settings window refuses to accept.
    bool Conflicts(Command command) const noexcept;

    // The command a key press means, or Count for none.
    Command Lookup(const Binding& pressed) const noexcept;

    // Ready to append to a menu label: a tab and the key, or nothing at all.
    std::wstring MenuSuffix(Command command) const noexcept;

    void ResetToDefaults() noexcept;

    void Load(const std::wstring& path) noexcept;
    // Written by the caller as part of the settings file, so this only produces
    // the section body.
    std::wstring ToFileText() const noexcept;

private:
    std::array<Binding, static_cast<size_t>(Command::Count)> bindings_{};
};

}  // namespace ccl::app

#pragma once

#include <windows.h>

#include <array>
#include <string>

namespace ccl::app {

// Mouse actions whose gesture is a matter of taste rather than of meaning.
//
// Deliberately not everything the mouse does. What the left button does belongs
// to the selected tool, and the right button opens the menu, which is the one
// way to reach everything else -- neither is listed here, because taking them
// away would leave the window with no way to draw or no way out.
enum class MouseCommand {
    Scroll,
    MoveWindow,
    Zoom,
    Opacity,
    Count,
};

// Press-and-move gestures. Scrolling and moving the window are both drags, so
// they are chosen from the same set.
//
// LeftDrag is the bare left button, which keeps the meaning it already has:
// it scrolls when the tool is not using it -- the view tool, or space held
// down. It is offered for scrolling alone. Anywhere else it would take the
// left button off the tools and leave the pen with nothing to draw with.
enum class DragGesture {
    None,
    LeftDrag,
    MiddleDrag,
    RightDrag,
    CtrlLeftDrag,
    CtrlMiddleDrag,
    ShiftMiddleDrag,
    Count,
};

// Turns of the wheel. Zoom and opacity cannot sensibly be put on a drag, so
// they are chosen from a set of their own rather than from the drags above.
enum class WheelGesture {
    None,
    Wheel,
    CtrlWheel,
    ShiftWheel,
    AltWheel,
    Count,
};

// Which of the two sets a command is chosen from.
bool IsWheelCommand(MouseCommand command) noexcept;

// Name shown in the assignment list.
const wchar_t* MouseCommandLabel(MouseCommand command) noexcept;
// Key used in the settings file, which stays stable even if the label changes.
const wchar_t* MouseCommandKey(MouseCommand command) noexcept;

// How a gesture reads in the assignment list. Unlike a key, which is written
// the same way wherever it appears, a gesture needs words, so the list and the
// settings file say it differently: this one is for the list.
const wchar_t* DragGestureLabel(DragGesture gesture) noexcept;
const wchar_t* WheelGestureLabel(WheelGesture gesture) noexcept;

// How it is written in the settings file, which stays plain ASCII so the file
// keeps reading the same way as the rest of it. Empty for unassigned.
const wchar_t* DragGestureText(DragGesture gesture) noexcept;
const wchar_t* WheelGestureText(WheelGesture gesture) noexcept;
// The inverse. Returns None for anything it cannot make sense of.
DragGesture ParseDragGesture(const std::wstring& text) noexcept;
WheelGesture ParseWheelGesture(const std::wstring& text) noexcept;

// Whether a gesture may be chosen for a command. Keeps the bare left button
// out of everything except scrolling; see DragGesture above.
bool DragGestureAllowed(MouseCommand command, DragGesture gesture) noexcept;

class MouseBindings {
public:
    MouseBindings() noexcept { ResetToDefaults(); }

    DragGesture Drag(MouseCommand command) const noexcept {
        return drag_[static_cast<size_t>(command)];
    }
    WheelGesture Wheel(MouseCommand command) const noexcept {
        return wheel_[static_cast<size_t>(command)];
    }

    // Plain assignment: a gesture already in use is left where it is and the
    // clash is reported instead. Quietly taking it off the other command is a
    // change no one sees until the other command is missed.
    void SetDrag(MouseCommand command, DragGesture gesture) noexcept {
        drag_[static_cast<size_t>(command)] = gesture;
    }
    void SetWheel(MouseCommand command, WheelGesture gesture) noexcept {
        wheel_[static_cast<size_t>(command)] = gesture;
    }

    // The command a gesture means, or Count for none.
    MouseCommand LookupDrag(DragGesture gesture) const noexcept;
    MouseCommand LookupWheel(WheelGesture gesture) const noexcept;

    // How the assignment reads in the list, whichever set it came from.
    std::wstring Text(MouseCommand command) const noexcept;

    // True when another command has been given the same gesture.
    bool Conflicts(MouseCommand command) const noexcept;

    void ResetToDefaults() noexcept;

    void Load(const std::wstring& path) noexcept;
    // Written by the caller as part of the settings file, so this only produces
    // the section body.
    std::wstring ToFileText() const noexcept;

private:
    std::array<DragGesture, static_cast<size_t>(MouseCommand::Count)> drag_{};
    std::array<WheelGesture, static_cast<size_t>(MouseCommand::Count)> wheel_{};
};

}  // namespace ccl::app

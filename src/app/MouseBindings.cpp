#include "app/MouseBindings.h"

#include <cstring>
#include <cwchar>

namespace ccl::app {
namespace {

struct MouseCommandInfo {
    const wchar_t* key;
    const wchar_t* label;
    bool wheel;
    DragGesture dragFallback;
    WheelGesture wheelFallback;
};

// Indexed by MouseCommand, so the order has to match the enum.
constexpr MouseCommandInfo kCommands[] = {
    {L"Scroll", L"画像をスクロール", false, DragGesture::LeftDrag,
     WheelGesture::None},
    {L"MoveWindow", L"ウィンドウを移動", false, DragGesture::MiddleDrag,
     WheelGesture::None},
    {L"Zoom", L"拡大・縮小", true, DragGesture::None, WheelGesture::CtrlWheel},
    {L"Opacity", L"不透明度を変える", true, DragGesture::None,
     WheelGesture::Wheel},
};

static_assert(ARRAYSIZE(kCommands) == static_cast<size_t>(MouseCommand::Count),
              "every mouse command needs a name and a default gesture");

struct DragInfo {
    const wchar_t* label;
    const wchar_t* text;
};

// Indexed by DragGesture.
constexpr DragInfo kDrags[] = {
    {L"なし", L""},
    {L"左ドラッグ", L"LeftDrag"},
    {L"中ドラッグ", L"MiddleDrag"},
    {L"右ドラッグ", L"RightDrag"},
    {L"Ctrl + 左ドラッグ", L"Ctrl+LeftDrag"},
    {L"Ctrl + 中ドラッグ", L"Ctrl+MiddleDrag"},
    {L"Shift + 中ドラッグ", L"Shift+MiddleDrag"},
};

static_assert(ARRAYSIZE(kDrags) == static_cast<size_t>(DragGesture::Count),
              "every drag gesture needs a name");

struct WheelInfo {
    const wchar_t* label;
    const wchar_t* text;
};

// Indexed by WheelGesture.
constexpr WheelInfo kWheels[] = {
    {L"なし", L""},
    {L"ホイール", L"Wheel"},
    {L"Ctrl + ホイール", L"Ctrl+Wheel"},
    {L"Shift + ホイール", L"Shift+Wheel"},
    {L"Alt + ホイール", L"Alt+Wheel"},
};

static_assert(ARRAYSIZE(kWheels) == static_cast<size_t>(WheelGesture::Count),
              "every wheel gesture needs a name");

}  // namespace

bool IsWheelCommand(MouseCommand command) noexcept {
    return kCommands[static_cast<size_t>(command)].wheel;
}

const wchar_t* MouseCommandLabel(MouseCommand command) noexcept {
    return kCommands[static_cast<size_t>(command)].label;
}

const wchar_t* MouseCommandKey(MouseCommand command) noexcept {
    return kCommands[static_cast<size_t>(command)].key;
}

const wchar_t* DragGestureLabel(DragGesture gesture) noexcept {
    return kDrags[static_cast<size_t>(gesture)].label;
}

const wchar_t* WheelGestureLabel(WheelGesture gesture) noexcept {
    return kWheels[static_cast<size_t>(gesture)].label;
}

const wchar_t* DragGestureText(DragGesture gesture) noexcept {
    return kDrags[static_cast<size_t>(gesture)].text;
}

const wchar_t* WheelGestureText(WheelGesture gesture) noexcept {
    return kWheels[static_cast<size_t>(gesture)].text;
}

DragGesture ParseDragGesture(const std::wstring& text) noexcept {
    for (size_t i = 1; i < ARRAYSIZE(kDrags); ++i) {
        if (::_wcsicmp(kDrags[i].text, text.c_str()) == 0) {
            return static_cast<DragGesture>(i);
        }
    }
    return DragGesture::None;
}

WheelGesture ParseWheelGesture(const std::wstring& text) noexcept {
    for (size_t i = 1; i < ARRAYSIZE(kWheels); ++i) {
        if (::_wcsicmp(kWheels[i].text, text.c_str()) == 0) {
            return static_cast<WheelGesture>(i);
        }
    }
    return WheelGesture::None;
}

bool DragGestureAllowed(MouseCommand command, DragGesture gesture) noexcept {
    if (IsWheelCommand(command)) {
        return false;
    }
    // The bare left button is the tool's, and scrolling only borrows it while
    // the tool is not using it. Offering it anywhere else would let the window
    // be set up so that nothing can be drawn.
    if (gesture == DragGesture::LeftDrag) {
        return command == MouseCommand::Scroll;
    }
    return true;
}

MouseCommand MouseBindings::LookupDrag(DragGesture gesture) const noexcept {
    if (gesture == DragGesture::None) {
        return MouseCommand::Count;
    }
    for (size_t i = 0; i < drag_.size(); ++i) {
        if (drag_[i] == gesture) {
            return static_cast<MouseCommand>(i);
        }
    }
    return MouseCommand::Count;
}

MouseCommand MouseBindings::LookupWheel(WheelGesture gesture) const noexcept {
    if (gesture == WheelGesture::None) {
        return MouseCommand::Count;
    }
    for (size_t i = 0; i < wheel_.size(); ++i) {
        if (wheel_[i] == gesture) {
            return static_cast<MouseCommand>(i);
        }
    }
    return MouseCommand::Count;
}

std::wstring MouseBindings::Text(MouseCommand command) const noexcept {
    return IsWheelCommand(command) ? WheelGestureLabel(Wheel(command))
                                   : DragGestureLabel(Drag(command));
}

bool MouseBindings::Conflicts(MouseCommand command) const noexcept {
    const size_t self = static_cast<size_t>(command);
    if (IsWheelCommand(command)) {
        const WheelGesture mine = wheel_[self];
        if (mine == WheelGesture::None) {
            return false;
        }
        for (size_t i = 0; i < wheel_.size(); ++i) {
            if (i != self && IsWheelCommand(static_cast<MouseCommand>(i)) &&
                wheel_[i] == mine) {
                return true;
            }
        }
        return false;
    }

    const DragGesture mine = drag_[self];
    if (mine == DragGesture::None) {
        return false;
    }
    for (size_t i = 0; i < drag_.size(); ++i) {
        if (i != self && !IsWheelCommand(static_cast<MouseCommand>(i)) &&
            drag_[i] == mine) {
            return true;
        }
    }
    return false;
}

void MouseBindings::ResetToDefaults() noexcept {
    for (size_t i = 0; i < drag_.size(); ++i) {
        drag_[i] = kCommands[i].dragFallback;
        wheel_[i] = kCommands[i].wheelFallback;
    }
}

void MouseBindings::Load(const std::wstring& path) noexcept {
    if (path.empty()) {
        return;
    }

    for (size_t i = 0; i < drag_.size(); ++i) {
        const auto command = static_cast<MouseCommand>(i);
        const wchar_t* fallback = IsWheelCommand(command)
                                      ? WheelGestureText(kCommands[i].wheelFallback)
                                      : DragGestureText(kCommands[i].dragFallback);

        wchar_t buffer[64];
        const DWORD length = ::GetPrivateProfileStringW(
            L"Mouse", kCommands[i].key, fallback, buffer, ARRAYSIZE(buffer),
            path.c_str());

        // An empty value is a deliberate "no gesture", not a missing entry:
        // the fallback above already covers the entry being absent.
        const std::wstring text(buffer, length);
        if (IsWheelCommand(command)) {
            wheel_[i] = ParseWheelGesture(text);
        } else {
            drag_[i] = ParseDragGesture(text);
        }
    }
}

std::wstring MouseBindings::ToFileText() const noexcept {
    std::wstring text;
    for (size_t i = 0; i < drag_.size(); ++i) {
        const auto command = static_cast<MouseCommand>(i);
        text += kCommands[i].key;
        text += L"=";
        text += IsWheelCommand(command) ? WheelGestureText(wheel_[i])
                                        : DragGestureText(drag_[i]);
        text += L"\n";
    }
    return text;
}

}  // namespace ccl::app

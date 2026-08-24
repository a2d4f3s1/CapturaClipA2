#include "app/Shortcuts.h"

#include <cstdio>

namespace ccl::app {
namespace {

struct CommandInfo {
    const wchar_t* key;
    const wchar_t* label;
    Binding fallback;
};

// Indexed by Command, so the order has to match the enum.
constexpr CommandInfo kCommands[] = {
    {L"Undo", L"元に戻す", {'Z', true, false, false}},
    {L"Redo", L"やり直し", {'Y', true, false, false}},
    {L"Save", L"保存", {'S', true, false, false}},
    {L"Copy", L"クリップボードにコピー", {'C', true, false, false}},
    {L"Open", L"開く", {'O', true, false, false}},
    {L"Paste", L"貼り付け", {'V', true, false, false}},
    {L"ToolView", L"ツール: ビュー", {'V', false, false, false}},
    {L"ToolPen", L"ツール: ペン", {'B', false, false, false}},
    {L"ToolEraser", L"ツール: 消しゴム", {'E', false, false, false}},
    {L"ToolText", L"ツール: テキスト", {'T', false, false, false}},
    {L"ToolSelect", L"ツール: 範囲選択", {'W', false, false, false}},
    // Kept next to the one above in both this table and the enum: the two are
    // matched by position, and a name landing against the wrong entry would
    // load someone's saved key onto a different command.
    {L"ToolLasso", L"ツール: 投げ縄", {'L', false, false, false}},
    {L"ToolObjectSelect",
     L"ツール: オブジェクト選択",
     {'W', true, false, false}},
    {L"ToolObjectLasso",
     L"ツール: オブジェクト投げ縄",
     {'L', true, false, false}},
    // Shift+I rather than I, which the palette already has. Both are about
    // choosing a colour, so they are kept on the same key.
    {L"Eyedropper", L"ツール: スポイト", {'I', false, true, false}},
    // PageUp and PageDown, which nothing else uses. The arrow keys were the
    // other candidate and were left alone: they scroll, and Ctrl with up or
    // down already turns an arrowhead while there is one to turn.
    {L"ObjectRaise", L"前へ出す", {VK_PRIOR, false, false, false}},
    {L"ObjectLower", L"後ろへ送る", {VK_NEXT, false, false, false}},
    {L"ObjectToFront", L"最前面へ", {VK_PRIOR, true, false, false}},
    {L"ObjectToBack", L"最背面へ", {VK_NEXT, true, false, false}},
    // Ctrl+R, next to the R that puts an arrowhead on a line: both are about
    // which way something points.
    {L"ObjectRotate", L"回転...", {'R', true, false, false}},
    {L"ColorPicker", L"色を選ぶ", {'I', false, false, false}},
    {L"Highlighter", L"蛍光マーカー", {'H', false, false, false}},
    {L"Antialias", L"なめらかにする", {'A', false, false, false}},
    {L"FitToImage", L"画像サイズに合わせる", {'F', false, false, false}},
    {L"HideWindow", L"しばらく隠す", {'X', false, false, false}},
    // Shift is the lighter of each pair, as it is on the pen: the same colour
    // laid down as a wash rather than solid.
    {L"FillSelection", L"塗りつぶし", {'G', false, false, false}},
    {L"FillSelectionMarker", L"マーカー塗りつぶし", {'G', false, true, false}},
    {L"OutlineSelection", L"境界線を描く", {'O', false, false, false}},
    {L"OutlineSelectionMarker",
     L"マーカーで境界線を描く",
     {'O', false, true, false}},
    {L"Mosaic", L"モザイク", {'M', false, false, false}},
    {L"Blur", L"ぼかし", {'M', false, true, false}},
    {L"InsertArrowhead", L"矢印の頭を挿入", {'R', false, false, false}},
    {L"TextDecor", L"文字の飾り", {'D', true, false, false}},
};

static_assert(ARRAYSIZE(kCommands) == static_cast<size_t>(Command::Count),
              "every command needs a name and a default binding");

// Keys whose name is not simply the character they produce.
struct NamedKey {
    UINT key;
    const wchar_t* name;
};

constexpr NamedKey kNamedKeys[] = {
    {VK_OEM_4, L"["},        {VK_OEM_6, L"]"},
    {VK_OEM_1, L";"},        {VK_OEM_PLUS, L"="},
    {VK_OEM_COMMA, L","},    {VK_OEM_MINUS, L"-"},
    {VK_OEM_PERIOD, L"."},   {VK_OEM_2, L"/"},
    {VK_OEM_3, L"`"},        {VK_OEM_5, L"\\"},
    {VK_OEM_7, L"'"},        {VK_SPACE, L"Space"},
    {VK_RETURN, L"Enter"},   {VK_ESCAPE, L"Esc"},
    {VK_TAB, L"Tab"},        {VK_BACK, L"Backspace"},
    {VK_DELETE, L"Delete"},  {VK_INSERT, L"Insert"},
    {VK_HOME, L"Home"},      {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},   {VK_NEXT, L"PageDown"},
    {VK_LEFT, L"Left"},      {VK_RIGHT, L"Right"},
    {VK_UP, L"Up"},          {VK_DOWN, L"Down"},
};

const wchar_t* NameOfKey(UINT key) noexcept {
    for (const NamedKey& named : kNamedKeys) {
        if (named.key == key) {
            return named.name;
        }
    }
    return nullptr;
}

UINT KeyOfName(const std::wstring& name) noexcept {
    for (const NamedKey& named : kNamedKeys) {
        if (::_wcsicmp(named.name, name.c_str()) == 0) {
            return named.key;
        }
    }
    if (name.size() == 1) {
        const wchar_t letter = name[0];
        if ((letter >= L'A' && letter <= L'Z') ||
            (letter >= L'0' && letter <= L'9')) {
            return static_cast<UINT>(letter);
        }
        if (letter >= L'a' && letter <= L'z') {
            return static_cast<UINT>(letter - L'a' + L'A');
        }
    }
    if (name.size() >= 2 && (name[0] == L'F' || name[0] == L'f')) {
        const int number = ::_wtoi(name.c_str() + 1);
        if (number >= 1 && number <= 24) {
            return static_cast<UINT>(VK_F1 + number - 1);
        }
    }
    return 0;
}

}  // namespace

std::wstring BindingText(const Binding& binding) noexcept {
    if (!binding.IsSet()) {
        return {};
    }

    std::wstring text;
    if (binding.ctrl) text += L"Ctrl+";
    if (binding.shift) text += L"Shift+";
    if (binding.alt) text += L"Alt+";

    if (const wchar_t* named = NameOfKey(binding.key); named != nullptr) {
        text += named;
    } else if (binding.key >= VK_F1 && binding.key <= VK_F24) {
        wchar_t number[8];
        ::swprintf_s(number, L"F%u", binding.key - VK_F1 + 1);
        text += number;
    } else if ((binding.key >= 'A' && binding.key <= 'Z') ||
               (binding.key >= '0' && binding.key <= '9')) {
        text += static_cast<wchar_t>(binding.key);
    } else {
        // Anything else is shown by its number, so that an odd key set by hand
        // at least round-trips rather than vanishing.
        wchar_t number[16];
        ::swprintf_s(number, L"#%u", binding.key);
        text += number;
    }
    return text;
}

Binding ParseBinding(const std::wstring& text) noexcept {
    Binding binding;
    if (text.empty()) {
        return binding;
    }

    size_t start = 0;
    while (true) {
        const size_t plus = text.find(L'+', start);
        // A trailing '+' is the key itself rather than a separator.
        if (plus == std::wstring::npos || plus + 1 >= text.size()) {
            break;
        }
        const std::wstring part = text.substr(start, plus - start);
        if (::_wcsicmp(part.c_str(), L"Ctrl") == 0) {
            binding.ctrl = true;
        } else if (::_wcsicmp(part.c_str(), L"Shift") == 0) {
            binding.shift = true;
        } else if (::_wcsicmp(part.c_str(), L"Alt") == 0) {
            binding.alt = true;
        } else {
            break;  // not a modifier, so the rest is the key
        }
        start = plus + 1;
    }

    const std::wstring name = text.substr(start);
    if (!name.empty() && name[0] == L'#') {
        binding.key = static_cast<UINT>(::_wtoi(name.c_str() + 1));
    } else {
        binding.key = KeyOfName(name);
    }
    if (binding.key == 0) {
        return Binding{};
    }
    return binding;
}

const wchar_t* CommandLabel(Command command) noexcept {
    return kCommands[static_cast<size_t>(command)].label;
}

const wchar_t* CommandKey(Command command) noexcept {
    return kCommands[static_cast<size_t>(command)].key;
}

void Shortcuts::ResetToDefaults() noexcept {
    for (size_t i = 0; i < bindings_.size(); ++i) {
        bindings_[i] = kCommands[i].fallback;
    }
}

void Shortcuts::Set(Command command, const Binding& binding) noexcept {
    bindings_[static_cast<size_t>(command)] = binding;
}

void Shortcuts::Clear(Command command) noexcept {
    bindings_[static_cast<size_t>(command)] = Binding{};
}

bool Shortcuts::Conflicts(Command command) const noexcept {
    const size_t self = static_cast<size_t>(command);
    const Binding& mine = bindings_[self];
    if (!mine.IsSet()) {
        return false;
    }
    for (size_t i = 0; i < bindings_.size(); ++i) {
        if (i != self && bindings_[i] == mine) {
            return true;
        }
    }
    return false;
}

Command Shortcuts::Lookup(const Binding& pressed) const noexcept {
    if (!pressed.IsSet()) {
        return Command::Count;
    }
    for (size_t i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i].IsSet() && bindings_[i] == pressed) {
            return static_cast<Command>(i);
        }
    }
    return Command::Count;
}

std::wstring Shortcuts::MenuSuffix(Command command) const noexcept {
    const std::wstring text = BindingText(For(command));
    return text.empty() ? std::wstring{} : L"\t" + text;
}

void Shortcuts::Load(const std::wstring& path) noexcept {
    if (path.empty()) {
        return;
    }

    for (size_t i = 0; i < bindings_.size(); ++i) {
        const std::wstring fallback = BindingText(kCommands[i].fallback);

        wchar_t buffer[64];
        const DWORD length = ::GetPrivateProfileStringW(
            L"Shortcuts", kCommands[i].key, fallback.c_str(), buffer,
            ARRAYSIZE(buffer), path.c_str());

        const std::wstring text(buffer, length);
        // An empty value is a deliberate "no key", not a missing entry: the
        // fallback above already covers the entry being absent.
        bindings_[i] = text.empty() ? Binding{} : ParseBinding(text);
    }
}

std::wstring Shortcuts::ToFileText() const noexcept {
    std::wstring text;
    for (size_t i = 0; i < bindings_.size(); ++i) {
        text += kCommands[i].key;
        text += L"=";
        text += BindingText(bindings_[i]);
        text += L"\n";
    }
    return text;
}

}  // namespace ccl::app

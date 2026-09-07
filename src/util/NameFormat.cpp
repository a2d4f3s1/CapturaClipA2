#include "util/NameFormat.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace ccl::util {
namespace {

void AppendNumber(std::wstring& out, int value, int digits) noexcept {
    wchar_t buffer[8];
    ::swprintf_s(buffer, L"%0*d", digits, value);
    out.append(buffer);
}

// CON and the like name devices at every level of a path, so a folder cannot
// be called one. The extension is ignored: "CON.png" is taken as well.
bool IsReservedDeviceName(const std::wstring& name) noexcept {
    static const wchar_t* const kReserved[] = {
        L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3",
        L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1",
        L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8",
        L"LPT9"};

    const std::wstring stem = name.substr(0, name.find(L'.'));
    for (const wchar_t* const candidate : kReserved) {
        if (::_wcsicmp(stem.c_str(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::wstring ExpandPlaceholders(const std::wstring& format,
                                const std::wstring& title,
                                const SYSTEMTIME& time) noexcept {
    std::wstring result;
    result.reserve(format.size() + title.size() + 16);

    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != L'%' || i + 1 >= format.size()) {
            result.push_back(format[i]);
            continue;
        }

        const wchar_t code = format[++i];
        switch (code) {
            case L'y': AppendNumber(result, time.wYear, 4); break;
            case L'Y': AppendNumber(result, time.wYear % 100, 2); break;
            case L'm': AppendNumber(result, time.wMonth, 2); break;
            case L'd': AppendNumber(result, time.wDay, 2); break;
            case L'h': AppendNumber(result, time.wHour, 2); break;
            case L'n': AppendNumber(result, time.wMinute, 2); break;
            case L's': AppendNumber(result, time.wSecond, 2); break;
            case L't': result.append(title); break;
            case L'%': result.push_back(L'%'); break;
            default:
                // Unrecognised: keep it visible instead of dropping it.
                result.push_back(L'%');
                result.push_back(code);
                break;
        }
    }
    return result;
}

std::wstring ExpandPlaceholders(const std::wstring& format,
                                const std::wstring& title) noexcept {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    return ExpandPlaceholders(format, title, now);
}

std::wstring SanitizePathComponent(const std::wstring& text) noexcept {
    // Long enough to still recognise which window a folder came from, short
    // enough that a title cannot push the path past what the shell handles.
    constexpr size_t kMaxLength = 64;

    std::wstring result;
    result.reserve((std::min)(text.size(), kMaxLength));

    for (const wchar_t c : text) {
        if (result.size() >= kMaxLength) {
            break;
        }
        // Everything Windows forbids in a name, which is also everything that
        // would let a title reach outside the folder it was meant to name.
        const bool forbidden = c < 0x20 || c == L'\\' || c == L'/' ||
                               c == L':' || c == L'*' || c == L'?' ||
                               c == L'"' || c == L'<' || c == L'>' || c == L'|';
        result.push_back(forbidden ? L'_' : c);
    }

    // Windows drops trailing dots and spaces from a name, so a component made
    // only of those would vanish and leave the path pointing at the parent.
    // ".." goes this way too, which is the case worth stopping.
    const size_t last = result.find_last_not_of(L". ");
    result.erase(last == std::wstring::npos ? 0 : last + 1);
    const size_t first = result.find_first_not_of(L" ");
    result.erase(0, first == std::wstring::npos ? result.size() : first);

    if (!result.empty() && IsReservedDeviceName(result)) {
        result.insert(result.begin(), L'_');
    }
    return result;
}

}  // namespace ccl::util

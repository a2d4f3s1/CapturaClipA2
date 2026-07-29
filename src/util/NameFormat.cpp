#include "util/NameFormat.h"

#include <cstdio>

namespace ccl::util {
namespace {

void AppendNumber(std::wstring& out, int value, int digits) noexcept {
    wchar_t buffer[8];
    ::swprintf_s(buffer, L"%0*d", digits, value);
    out.append(buffer);
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

}  // namespace ccl::util

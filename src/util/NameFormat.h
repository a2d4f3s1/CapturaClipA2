#pragma once

#include <windows.h>

#include <string>

namespace ccl::util {

// Expands the placeholders used in the title and file name settings.
//
//   %y  year, four digits      %h  hour     %t  title of the capture
//   %Y  year, last two digits  %n  minute   %%  a literal percent sign
//   %m  month                  %s  second
//   %d  day
//
// An unknown placeholder is left as-is rather than swallowed, so a typo is
// visible instead of silently deleting text.
std::wstring ExpandPlaceholders(const std::wstring& format,
                                const std::wstring& title,
                                const SYSTEMTIME& time) noexcept;

// Same, using the current local time.
std::wstring ExpandPlaceholders(const std::wstring& format,
                                const std::wstring& title) noexcept;

}  // namespace ccl::util

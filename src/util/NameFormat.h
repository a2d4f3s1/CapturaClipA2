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

// Reduces a string to something usable as one name inside a path: anything
// Windows forbids in a name becomes an underscore, and leading or trailing
// spaces and dots are dropped.
//
// Meant for the title, which is whatever the captured window calls itself and
// so can hold separators and "..". The format around it is written by the
// user and needs to keep its "C:\", which is why this is applied to the title
// before expansion rather than to the result.
std::wstring SanitizePathComponent(const std::wstring& text) noexcept;

}  // namespace ccl::util

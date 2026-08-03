#pragma once

#include <windows.h>

#include <cstdio>
#include <string>

namespace ccl::timing {

// Latency is the headline requirement for this tool, so the numbers that define
// the feel -- "launch until the selection is usable", "mouse release until the
// window is on screen", and per-frame drawing cost while dragging -- are
// measured rather than eyeballed.
//
// Lines are buffered in memory and written once at exit. Writing each line
// straight to disk costs milliseconds and lands inside the very intervals being
// measured, which made the log distort its own numbers.
//
// Off unless asked for: this is a development aid, and leaving it on drops a
// log file beside the executable on every run of a copy someone was given.
inline bool g_enabled = false;
inline std::wstring g_buffer;

inline LONGLONG Now() noexcept {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

inline double MillisecondsSince(LONGLONG start) noexcept {
    LARGE_INTEGER frequency{};
    ::QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) {
        return 0.0;
    }
    return static_cast<double>(Now() - start) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

inline void Write(const wchar_t* line) noexcept {
    if (!g_enabled) {
        return;
    }
    g_buffer.append(line);
}

inline void Flush() noexcept {
    if (!g_enabled || g_buffer.empty()) {
        return;
    }

    ::OutputDebugStringW(g_buffer.c_str());

    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        wchar_t* lastSlash = ::wcsrchr(path, L'\\');
        if (lastSlash != nullptr) {
            lastSlash[1] = L'\0';
            ::wcsncat_s(path, L"timing.log", _TRUNCATE);

            FILE* file = nullptr;
            if (::_wfopen_s(&file, path, L"a, ccs=UTF-8") == 0 &&
                file != nullptr) {
                ::fwprintf(file, L"%s", g_buffer.c_str());
                ::fclose(file);
            }
        }
    }
    g_buffer.clear();
}

// Flushes the log however the process leaves main.
struct ScopedFlush {
    ~ScopedFlush() { Flush(); }
};

inline void Report(const wchar_t* label, LONGLONG start) noexcept {
    if (!g_enabled) {
        return;
    }
    wchar_t line[256];
    ::swprintf_s(line, L"[timing] %-28s %8.2f ms\n", label,
                 MillisecondsSince(start));
    Write(line);
}

// Measures consecutive intervals, so a slow startup can be attributed to the
// step that actually costs the time rather than guessed at.
class Stopwatch {
public:
    Stopwatch() noexcept : last_(Now()) {}

    void Lap(const wchar_t* label) noexcept {
        Report(label, last_);
        last_ = Now();
    }

private:
    LONGLONG last_;
};

// Per-frame cost while dragging. Averages hide stutter, so the worst frame is
// tracked separately.
struct FrameStats {
    int count = 0;
    double total = 0.0;
    double worst = 0.0;

    void Add(double milliseconds) noexcept {
        ++count;
        total += milliseconds;
        if (milliseconds > worst) {
            worst = milliseconds;
        }
    }
};

inline void ReportFrames(const wchar_t* label, const FrameStats& stats) noexcept {
    if (!g_enabled || stats.count == 0) {
        return;
    }
    wchar_t line[256];
    ::swprintf_s(line, L"[timing] %-28s %4d frames, avg %6.2f ms, worst %6.2f ms\n",
                 label, stats.count, stats.total / stats.count, stats.worst);
    Write(line);
}

}  // namespace ccl::timing

#include "io/AutoSave.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <vector>

#include "io/ImageCodec.h"
#include "util/NameFormat.h"

namespace ccl::io {
namespace {

// Extensions this program writes. Nothing else is ever considered for pruning.
bool IsOwnExtension(const wchar_t* name) noexcept {
    const wchar_t* dot = ::wcsrchr(name, L'.');
    if (dot == nullptr) {
        return false;
    }
    return ::_wcsicmp(dot, L".png") == 0 || ::_wcsicmp(dot, L".jpg") == 0 ||
           ::_wcsicmp(dot, L".bmp") == 0;
}

std::wstring JoinPath(const std::wstring& folder, const std::wstring& name) {
    std::wstring path = folder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += name;
    return path;
}

// The moment the picture is being written out, to the second.
std::wstring TimeStamp() noexcept {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    wchar_t stamp[64];
    ::swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth,
                 now.wDay, now.wHour, now.wMinute, now.wSecond);
    return stamp;
}

// The name a given attempt would use: the time, with a counter appended once
// that name turns out to be spoken for.
std::wstring CandidateName(const std::wstring& folder,
                           const std::wstring& stamp, const wchar_t* extension,
                           int attempt) noexcept {
    wchar_t name[96];
    if (attempt == 0) {
        ::swprintf_s(name, L"%s.%s", stamp.c_str(), extension);
    } else {
        ::swprintf_s(name, L"%s-%d.%s", stamp.c_str(), attempt + 1, extension);
    }
    return JoinPath(folder, name);
}

}  // namespace

std::wstring AutoSaveImage(ccl::render::D2DContext& context,
                           const ccl::capture::DibBuffer& image,
                           const ccl::app::Settings& settings,
                           const std::wstring& title,
                           HistoryCleanup cleanup) noexcept {
    if (settings.autoSaveFolder.empty() || !image.IsValid()) {
        return std::wstring{};
    }

    const std::wstring folder =
        ccl::util::ExpandPlaceholders(settings.autoSaveFolder, title);
    if (folder.empty()) {
        return std::wstring{};
    }

    const int created = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS &&
        created != ERROR_FILE_EXISTS) {
        return std::wstring{};
    }

    const wchar_t* extension = ExtensionFor(settings.defaultFormat);
    const std::wstring stamp = TimeStamp();

    // Which name to use is settled by creating the file, not by looking to see
    // whether it is free. Several of these programs closing at the same
    // instant -- which is what happens when a whole group is closed from the
    // taskbar, or when the session ends -- would otherwise all decide on the
    // same second and write over one another.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::wstring path =
            CandidateName(folder, stamp, extension, attempt);
        const NewFileResult result = SaveImageAsNewFile(
            context, image, path, settings.defaultFormat, settings.jpegQuality);
        if (result == NewFileResult::AlreadyExists) {
            continue;
        }
        if (result == NewFileResult::Failed) {
            return std::wstring{};
        }
        if (cleanup == HistoryCleanup::Prune) {
            PruneAutoSaveHistory(settings, folder);
        }
        return path;
    }
    return std::wstring{};
}

void PruneAutoSaveHistory(const ccl::app::Settings& settings,
                          const std::wstring& folder) noexcept {
    if (settings.autoSaveHistoryDays <= 0 || folder.empty()) {
        return;
    }

    // Cut-off as a FILETIME, so comparisons are plain 64-bit integers.
    FILETIME nowFileTime{};
    ::GetSystemTimeAsFileTime(&nowFileTime);

    ULARGE_INTEGER now{};
    now.LowPart = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;

    constexpr ULONGLONG kIntervalsPerDay = 24ull * 60ull * 60ull * 10'000'000ull;
    const ULONGLONG span =
        static_cast<ULONGLONG>(settings.autoSaveHistoryDays) * kIntervalsPerDay;
    if (now.QuadPart <= span) {
        return;
    }
    const ULONGLONG cutoff = now.QuadPart - span;

    const std::wstring pattern = JoinPath(folder, L"*");

    WIN32_FIND_DATAW entry{};
    const HANDLE search = ::FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }

    // SHFileOperation takes a double-null-terminated list of paths.
    std::vector<wchar_t> victims;

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!IsOwnExtension(entry.cFileName)) {
            continue;
        }

        ULARGE_INTEGER written{};
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart >= cutoff) {
            continue;
        }

        const std::wstring path = JoinPath(folder, entry.cFileName);
        victims.insert(victims.end(), path.begin(), path.end());
        victims.push_back(L'\0');
    } while (::FindNextFileW(search, &entry) != 0);

    ::FindClose(search);

    if (victims.empty()) {
        return;
    }
    victims.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = victims.data();
    // FOF_ALLOWUNDO is what sends these to the Recycle Bin instead of erasing
    // them, so a retention set too aggressively can be undone.
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI |
                       FOF_SILENT | FOF_NOCONFIRMMKDIR;
    ::SHFileOperationW(&operation);
}

}  // namespace ccl::io

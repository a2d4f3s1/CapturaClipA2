#include "io/Clipboard.h"

#include <cstring>

namespace ccl::io {

bool CopyToClipboard(HWND owner, const ccl::capture::DibBuffer& image) noexcept {
    if (!image.IsValid()) {
        return false;
    }

    const int width = image.Width();
    const int height = image.Height();

    // 24-bit, bottom-up. The capture has no meaningful alpha channel, and a
    // 32-bit DIB would be read as fully transparent by anything that honours
    // it; bottom-up because some readers ignore a negative height.
    const size_t sourceStride = image.Stride();
    const size_t targetStride =
        (static_cast<size_t>(width) * 3u + 3u) & ~static_cast<size_t>(3);
    const size_t pixelBytes = targetStride * static_cast<size_t>(height);

    const HGLOBAL handle =
        ::GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
    if (handle == nullptr) {
        return false;
    }

    auto* memory = static_cast<unsigned char*>(::GlobalLock(handle));
    if (memory == nullptr) {
        ::GlobalFree(handle);
        return false;
    }

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(memory);
    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height;  // positive: rows run bottom to top
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    const auto* source = static_cast<const unsigned char*>(image.Pixels());
    unsigned char* target = memory + sizeof(BITMAPINFOHEADER);

    for (int y = 0; y < height; ++y) {
        const unsigned char* sourceRow =
            source + static_cast<size_t>(height - 1 - y) * sourceStride;
        unsigned char* targetRow = target + static_cast<size_t>(y) * targetStride;

        for (int x = 0; x < width; ++x) {
            targetRow[x * 3 + 0] = sourceRow[x * 4 + 0];
            targetRow[x * 3 + 1] = sourceRow[x * 4 + 1];
            targetRow[x * 3 + 2] = sourceRow[x * 4 + 2];
        }
    }

    ::GlobalUnlock(handle);

    if (!::OpenClipboard(owner)) {
        ::GlobalFree(handle);
        return false;
    }
    ::EmptyClipboard();

    if (::SetClipboardData(CF_DIB, handle) == nullptr) {
        ::CloseClipboard();
        ::GlobalFree(handle);
        return false;
    }

    // Ownership passes to the clipboard on success.
    ::CloseClipboard();
    return true;
}

}  // namespace ccl::io

#include "io/Clipboard.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ccl::io {
namespace {

// Wide enough for any screen anyone is pasting from, narrow enough that the
// arithmetic below cannot run away.
constexpr long long kMaxDimension = 65535;

// Everything in the clipboard's copy of a bitmap was chosen by whichever
// program put it there, so none of it can be taken on faith. This works out
// how many bytes the header claims to need and turns down anything that does
// not fit inside what was actually handed over -- otherwise the pixel pointer
// computed from biSize and biClrUsed lands outside the block, and GDI reads
// whatever happens to be there.
bool DibFitsInBuffer(const BITMAPINFOHEADER* header,
                     SIZE_T available) noexcept {
    if (available < sizeof(BITMAPINFOHEADER)) {
        return false;
    }
    if (header->biSize < sizeof(BITMAPINFOHEADER) ||
        header->biSize > available) {
        return false;
    }
    if (header->biPlanes != 1) {
        return false;
    }
    switch (header->biBitCount) {
        case 1:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32:
            break;
        default:
            return false;
    }
    // RLE and the JPEG/PNG pass-through formats hand the decoding to GDI with
    // bytes this side has not checked. Only the plain layouts are taken.
    if (header->biCompression != BI_RGB &&
        header->biCompression != BI_BITFIELDS) {
        return false;
    }

    // Negating the smallest LONG is undefined, so it goes before the sign is
    // taken off anywhere.
    if (header->biHeight == (std::numeric_limits<LONG>::min)()) {
        return false;
    }
    const long long width = header->biWidth;
    const long long height = header->biHeight < 0
                                 ? -static_cast<long long>(header->biHeight)
                                 : header->biHeight;
    if (width <= 0 || height <= 0 || width > kMaxDimension ||
        height > kMaxDimension) {
        return false;
    }

    // What sits between the header and the pixels: the colour table, or the
    // three masks BI_BITFIELDS puts there instead.
    long long between = 0;
    if (header->biClrUsed != 0) {
        between = static_cast<long long>(header->biClrUsed) * sizeof(RGBQUAD);
    } else if (header->biBitCount <= 8) {
        between = (1ll << header->biBitCount) * sizeof(RGBQUAD);
    }
    if (header->biCompression == BI_BITFIELDS &&
        header->biSize == sizeof(BITMAPINFOHEADER)) {
        between += 3 * sizeof(DWORD);
    }

    const long long stride = ((width * header->biBitCount + 31) / 32) * 4;
    const long long needed =
        static_cast<long long>(header->biSize) + between + stride * height;
    return needed <= static_cast<long long>(available);
}

}  // namespace

bool ClipboardHasImage() noexcept {
    return ::IsClipboardFormatAvailable(CF_DIB) ||
           ::IsClipboardFormatAvailable(CF_BITMAP);
}

ccl::capture::DibBuffer PasteFromClipboard(HWND owner) noexcept {
    ccl::capture::DibBuffer result;
    if (!ClipboardHasImage() || !::OpenClipboard(owner)) {
        return result;
    }

    const HANDLE handle = ::GetClipboardData(CF_DIB);
    const auto* header =
        handle != nullptr
            ? static_cast<const BITMAPINFOHEADER*>(::GlobalLock(handle))
            : nullptr;

    // Checked before a single field is used for arithmetic: the size of the
    // block is the only thing here this side actually knows.
    if (header != nullptr && !DibFitsInBuffer(header, ::GlobalSize(handle))) {
        ::GlobalUnlock(handle);
        ::CloseClipboard();
        return result;
    }

    if (header != nullptr) {
        const int width = header->biWidth;
        // A negative height means the rows already run top-down.
        const bool topDown = header->biHeight < 0;
        const int height = std::abs(header->biHeight);

        if (width > 0 && height > 0 && result.Create(width, height)) {
            // Colour tables sit between the header and the pixels, so the
            // offset cannot be assumed.
            const auto* info = reinterpret_cast<const BITMAPINFO*>(header);
            const auto* bits = reinterpret_cast<const BYTE*>(header) +
                               header->biSize +
                               header->biClrUsed * sizeof(RGBQUAD);

            const HDC screen = ::GetDC(nullptr);
            const HDC memory = ::CreateCompatibleDC(screen);
            const HGDIOBJ previous = ::SelectObject(memory, result.Handle());

            // Let GDI do the conversion; clipboard bitmaps arrive at any depth
            // and compression the source application felt like using.
            if (::StretchDIBits(memory, 0, 0, width, height, 0, 0, width, height,
                                bits, info, DIB_RGB_COLORS, SRCCOPY) == 0) {
                result.Reset();
            }

            ::SelectObject(memory, previous);
            ::DeleteDC(memory);
            ::ReleaseDC(nullptr, screen);

            // StretchDIBits works bottom-up unless told otherwise, so a
            // top-down source comes out inverted and has to be flipped back.
            if (result.IsValid() && topDown) {
                const size_t stride = result.Stride();
                std::vector<unsigned char> row(stride);
                auto* pixels = static_cast<unsigned char*>(result.Pixels());
                for (int y = 0; y < height / 2; ++y) {
                    auto* upper = pixels + static_cast<size_t>(y) * stride;
                    auto* lower =
                        pixels + static_cast<size_t>(height - 1 - y) * stride;
                    std::memcpy(row.data(), upper, stride);
                    std::memcpy(upper, lower, stride);
                    std::memcpy(lower, row.data(), stride);
                }
            }
        }
        ::GlobalUnlock(handle);
    }

    ::CloseClipboard();
    return result;
}

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

#include "io/ImageCodec.h"

#include <shlwapi.h>
#include <wrl/client.h>

#include "render/D2DContext.h"

using Microsoft::WRL::ComPtr;

namespace ccl::io {
namespace {

GUID ContainerFor(ccl::app::ImageFormat format) noexcept {
    switch (format) {
        case ccl::app::ImageFormat::Jpeg: return GUID_ContainerFormatJpeg;
        case ccl::app::ImageFormat::Bmp: return GUID_ContainerFormatBmp;
        case ccl::app::ImageFormat::Png:
        default: return GUID_ContainerFormatPng;
    }
}

}  // namespace

// Everything the imaging component decodes out of the box, so the list is not
// limited to the formats this program writes.
const wchar_t* const kOpenFilter =
    L"画像ファイル\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.ico\0"
    L"すべてのファイル\0*.*\0\0";

ccl::capture::DibBuffer LoadImageFile(ccl::render::D2DContext& context,
                                      const std::wstring& path) noexcept {
    ccl::capture::DibBuffer result;
    if (path.empty()) {
        return result;
    }

    IWICImagingFactory* factory = context.Imaging();
    if (factory == nullptr) {
        return result;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
        return result;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return result;
    }

    // Converted to the layout the rest of the program works in, which also
    // flattens any transparency onto the background rather than leaving it to
    // be interpreted later.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return result;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
        height == 0) {
        return result;
    }

    if (!result.Create(static_cast<int>(width), static_cast<int>(height))) {
        return result;
    }

    if (FAILED(converter->CopyPixels(
            nullptr, result.Stride(), result.Stride() * height,
            static_cast<BYTE*>(result.Pixels())))) {
        result.Reset();
    }
    return result;
}

const wchar_t* ExtensionFor(ccl::app::ImageFormat format) noexcept {
    switch (format) {
        case ccl::app::ImageFormat::Jpeg: return L"jpg";
        case ccl::app::ImageFormat::Bmp: return L"bmp";
        case ccl::app::ImageFormat::Png:
        default: return L"png";
    }
}

namespace {

// Writes the image into a stream that is already open. Both ways of saving
// share this; they differ only in how the file behind the stream is opened,
// which is the whole of the difference between replacing a file and refusing
// to touch one that is already there.
bool EncodeTo(IWICImagingFactory* factory, IStream* stream,
              const ccl::capture::DibBuffer& image,
              ccl::app::ImageFormat format, int jpegQuality) noexcept {
    // 32bppBGR rather than BGRA: the screen grab leaves the alpha channel at
    // zero, so treating it as alpha would save a fully transparent image.
    const UINT width = static_cast<UINT>(image.Width());
    const UINT height = static_cast<UINT>(image.Height());
    const UINT stride = image.Stride();
    const UINT size = stride * height;

    ComPtr<IWICBitmap> source;
    if (FAILED(factory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppBGR, stride, size,
            static_cast<BYTE*>(const_cast<void*>(image.Pixels())), &source))) {
        return false;
    }

    // 24bpp for every format: none of them need an alpha channel here, and
    // some readers cope badly with 32-bit BMP.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return false;
    }
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(ContainerFor(format), nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options))) {
        return false;
    }

    if (format == ccl::app::ImageFormat::Jpeg && options) {
        PROPBAG2 property{};
        property.pstrName = const_cast<LPOLESTR>(L"ImageQuality");

        VARIANT value{};
        ::VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(jpegQuality) / 100.0f;
        options->Write(1, &property, &value);
    }

    if (FAILED(frame->Initialize(options.Get()))) {
        return false;
    }
    if (FAILED(frame->SetSize(width, height))) {
        return false;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&pixelFormat))) {
        return false;
    }
    if (FAILED(frame->WriteSource(converter.Get(), nullptr))) {
        return false;
    }

    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

}  // namespace

bool SaveImage(ccl::render::D2DContext& context,
               const ccl::capture::DibBuffer& image, const std::wstring& path,
               ccl::app::ImageFormat format, int jpegQuality) noexcept {
    if (!image.IsValid() || path.empty()) {
        return false;
    }

    IWICImagingFactory* factory = context.Imaging();
    if (factory == nullptr) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }
    return EncodeTo(factory, stream.Get(), image, format, jpegQuality);
}

NewFileResult SaveImageAsNewFile(ccl::render::D2DContext& context,
                                 const ccl::capture::DibBuffer& image,
                                 const std::wstring& path,
                                 ccl::app::ImageFormat format,
                                 int jpegQuality) noexcept {
    if (!image.IsValid() || path.empty()) {
        return NewFileResult::Failed;
    }

    IWICImagingFactory* factory = context.Imaging();
    if (factory == nullptr) {
        return NewFileResult::Failed;
    }

    // STGM_FAILIFTHERE together with a request to create is CREATE_NEW: either
    // this call brings the file into being, or it fails because someone else
    // already has. Nothing in between, which is the point.
    ComPtr<IStream> stream;
    const HRESULT opened = ::SHCreateStreamOnFileEx(
        path.c_str(), STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_FAILIFTHERE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (opened == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) ||
        opened == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        return NewFileResult::AlreadyExists;
    }
    if (FAILED(opened)) {
        return NewFileResult::Failed;
    }

    if (EncodeTo(factory, stream.Get(), image, format, jpegQuality)) {
        return NewFileResult::Written;
    }

    // Nothing was at this path before, since the open above would have failed;
    // so removing the remains of a write that went wrong cannot take anything
    // that was already there.
    stream.Reset();
    ::DeleteFileW(path.c_str());
    return NewFileResult::Failed;
}

}  // namespace ccl::io

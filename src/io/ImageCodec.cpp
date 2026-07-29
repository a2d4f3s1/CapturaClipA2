#include "io/ImageCodec.h"

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

const wchar_t* ExtensionFor(ccl::app::ImageFormat format) noexcept {
    switch (format) {
        case ccl::app::ImageFormat::Jpeg: return L"jpg";
        case ccl::app::ImageFormat::Bmp: return L"bmp";
        case ccl::app::ImageFormat::Png:
        default: return L"png";
    }
}

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

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(ContainerFor(format), nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
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

}  // namespace ccl::io

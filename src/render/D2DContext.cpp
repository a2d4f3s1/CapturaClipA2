#include "render/D2DContext.h"

namespace ccl::render {

D2DContext::~D2DContext() {
    imaging_.Reset();
    text_.Reset();
    factory_.Reset();

    if (comInitialized_) {
        ::CoUninitialize();
    }
}

bool D2DContext::Initialize() noexcept {
    if (FAILED(::CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return false;
    }
    comInitialized_ = true;

    D2D1_FACTORY_OPTIONS options{};
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;

    const HRESULT hr = ::D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(factory_.GetAddressOf()));

    return SUCCEEDED(hr);
}

IDWriteFactory* D2DContext::Text() noexcept {
    if (text_) {
        return text_.Get();
    }

    const HRESULT hr = ::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(text_.GetAddressOf()));

    return SUCCEEDED(hr) ? text_.Get() : nullptr;
}

Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> D2DContext::CreateHwndTarget(
    HWND hwnd, UINT width, UINT height) const noexcept {
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target;
    if (!factory_) {
        return target;
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        // Force 96 DPI: the process works in physical pixels, so letting D2D
        // apply its own DPI scaling would double-scale the capture.
        96.0f, 96.0f);

    const D2D1_HWND_RENDER_TARGET_PROPERTIES windowProperties =
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height),
                                         D2D1_PRESENT_OPTIONS_IMMEDIATELY);

    if (FAILED(factory_->CreateHwndRenderTarget(properties, windowProperties,
                                                &target))) {
        target.Reset();
    }
    return target;
}

IWICImagingFactory* D2DContext::Imaging() noexcept {
    if (imaging_) {
        return imaging_.Get();
    }

    const HRESULT hr =
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging_));

    return SUCCEEDED(hr) ? imaging_.Get() : nullptr;
}

}  // namespace ccl::render

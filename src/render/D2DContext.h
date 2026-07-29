#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace ccl::render {

// Owns the process-wide Direct2D / DirectWrite / WIC factories.
//
// Only the Direct2D factory is created up front. DirectWrite and WIC are built
// on first use, because neither is needed on the launch path (the snapshot is
// taken with GDI) and creating them eagerly would show up in the launch
// latency budget.
class D2DContext {
public:
    D2DContext() = default;
    ~D2DContext();

    D2DContext(const D2DContext&) = delete;
    D2DContext& operator=(const D2DContext&) = delete;

    bool Initialize() noexcept;

    ID2D1Factory1* Factory() const noexcept { return factory_.Get(); }

    // Created on demand; returns nullptr if creation fails.
    IDWriteFactory* Text() noexcept;
    IWICImagingFactory* Imaging() noexcept;

    // Render target bound to a window, in physical pixels (DPI forced to 96 so
    // that one DIP equals one pixel) and presenting without waiting for vsync
    // so that dragging tracks the mouse with no added latency.
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> CreateHwndTarget(
        HWND hwnd, UINT width, UINT height) const noexcept;

private:
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> text_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging_;
    bool comInitialized_ = false;
};

}  // namespace ccl::render

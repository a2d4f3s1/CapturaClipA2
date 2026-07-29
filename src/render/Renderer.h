#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

namespace ccl::doc {
class Document;
}

namespace ccl::render {

// A thin outline around the capture. Without it a screenshot of a dark UI has
// no visible edge and simply blends into whatever is behind it.
inline constexpr int kWindowBorder = 1;

class D2DContext;

// Draws a document into a window. Phase 1 renders 1:1; zoom, scrolling and
// annotation drawing land on top of this in later phases.
class Renderer {
public:
    void Attach(const D2DContext& context, HWND hwnd) noexcept;
    void SetDocument(const ccl::doc::Document* document) noexcept;

    void Resize(UINT width, UINT height) noexcept;
    void Draw() noexcept;

private:
    bool EnsureTarget() noexcept;
    bool EnsureImageBitmap() noexcept;
    void DiscardDeviceResources() noexcept;

    const D2DContext* context_ = nullptr;
    HWND hwnd_ = nullptr;
    const ccl::doc::Document* document_ = nullptr;

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> image_;

    bool measuredFirstDraw_ = false;
};

}  // namespace ccl::render

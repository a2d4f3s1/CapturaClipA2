#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

#include "view/ViewState.h"

namespace ccl::doc {
class Document;
}

namespace ccl::render {

// A thin outline around the capture. Without it a screenshot of a dark UI has
// no visible edge and simply blends into whatever is behind it.
inline constexpr int kWindowBorder = 1;

class D2DContext;

// Draws a document into a window, honouring the view state (zoom, scroll).
// Annotation drawing lands on top of this in later phases.
class Renderer {
public:
    void Attach(const D2DContext& context, HWND hwnd) noexcept;
    void SetDocument(const ccl::doc::Document* document) noexcept;

    void Resize(UINT width, UINT height) noexcept;
    void Draw(const ccl::view::ViewState& view) noexcept;

    // Smooth interpolation looks better for photographs and text, nearest
    // neighbour is what you want when inspecting individual pixels.
    void SetSmoothScaling(bool smooth) noexcept { smoothScaling_ = smooth; }
    bool SmoothScaling() const noexcept { return smoothScaling_; }

private:
    bool EnsureTarget() noexcept;
    bool EnsureImageBitmap() noexcept;
    void DiscardDeviceResources() noexcept;

    const D2DContext* context_ = nullptr;
    HWND hwnd_ = nullptr;
    const ccl::doc::Document* document_ = nullptr;

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> image_;

    bool smoothScaling_ = true;
    bool measuredFirstDraw_ = false;
};

}  // namespace ccl::render

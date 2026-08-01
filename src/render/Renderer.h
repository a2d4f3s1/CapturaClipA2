#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

#include "doc/Annotation.h"
#include "view/ViewState.h"

namespace ccl::doc {
class Document;
}

namespace ccl::render {

// A thin outline around the capture. Without it a screenshot of a dark UI has
// no visible edge and simply blends into whatever is behind it.
inline constexpr int kWindowBorder = 1;

class D2DContext;

// Outline showing the size of the brush or eraser, drawn at the cursor.
struct BrushCursor {
    D2D1_POINT_2F position{};  // image coordinates
    float radius = 0.0f;       // image units
    // Drawn with the same antialiasing the brush will use, so the ring itself
    // shows whether strokes will come out smooth or hard-edged.
    bool antialias = true;
};

// Draws a document into a window, honouring the view state (zoom, scroll).
class Renderer {
public:
    void Attach(const D2DContext& context, HWND hwnd) noexcept;
    void SetDocument(const ccl::doc::Document* document) noexcept;

    void Resize(UINT width, UINT height) noexcept;

    // `active` is the stroke currently being drawn, which is not yet part of
    // the document. `cursor` draws the brush size outline when set.
    void Draw(const ccl::view::ViewState& view,
              const ccl::doc::Stroke* active = nullptr,
              const BrushCursor* cursor = nullptr) noexcept;

    // Smooth interpolation looks better for photographs and text, nearest
    // neighbour is what you want when inspecting individual pixels.
    void SetSmoothScaling(bool smooth) noexcept { smoothScaling_ = smooth; }
    bool SmoothScaling() const noexcept { return smoothScaling_; }


private:
    bool EnsureTarget() noexcept;
    bool EnsureImageBitmap() noexcept;
    void DiscardDeviceResources() noexcept;

    void DrawStroke(const ccl::doc::Stroke& stroke) noexcept;
    void DrawVariableStroke(const ccl::doc::Stroke& stroke) noexcept;

    const D2DContext* context_ = nullptr;
    HWND hwnd_ = nullptr;
    const ccl::doc::Document* document_ = nullptr;

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> image_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle_;

    bool smoothScaling_ = true;
    bool measuredFirstDraw_ = false;
};

}  // namespace ccl::render

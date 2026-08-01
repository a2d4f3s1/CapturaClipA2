#include "render/Renderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "doc/Document.h"
#include "render/D2DContext.h"
#include "util/Timing.h"

namespace ccl::render {
namespace {

D2D1_COLOR_F ToD2D(const ccl::doc::Color& color) noexcept {
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

}  // namespace

void Renderer::Attach(D2DContext& context, HWND hwnd) noexcept {
    context_ = &context;
    hwnd_ = hwnd;
    DiscardDeviceResources();
}

void Renderer::SetDocument(const ccl::doc::Document* document) noexcept {
    document_ = document;
    image_.Reset();
}

void Renderer::Resize(UINT width, UINT height) noexcept {
    if (target_ && width > 0 && height > 0) {
        target_->Resize(D2D1::SizeU(width, height));
    }
}

void Renderer::DiscardDeviceResources() noexcept {
    brush_.Reset();
    image_.Reset();
    target_.Reset();
}

bool Renderer::EnsureTarget() noexcept {
    if (target_) {
        return true;
    }
    if (context_ == nullptr || hwnd_ == nullptr) {
        return false;
    }

    RECT client{};
    if (!::GetClientRect(hwnd_, &client)) {
        return false;
    }

    target_ = context_->CreateHwndTarget(
        hwnd_, static_cast<UINT>(client.right - client.left),
        static_cast<UINT>(client.bottom - client.top));
    if (!target_) {
        return false;
    }

    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                              &brush_))) {
        DiscardDeviceResources();
        return false;
    }

    if (!strokeStyle_ && context_->Factory() != nullptr) {
        // Round caps and joins: without them a thick stroke shows a notch at
        // every direction change.
        const D2D1_STROKE_STYLE_PROPERTIES properties =
            D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        context_->Factory()->CreateStrokeStyle(properties, nullptr, 0,
                                               &strokeStyle_);
    }
    return true;
}

bool Renderer::EnsureImageBitmap() noexcept {
    if (image_) {
        return true;
    }
    if (!target_ || document_ == nullptr || !document_->IsValid()) {
        return false;
    }

    const auto& buffer = document_->Image();
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);

    const HRESULT hr = target_->CreateBitmap(
        D2D1::SizeU(static_cast<UINT>(buffer.Width()),
                    static_cast<UINT>(buffer.Height())),
        buffer.Pixels(), buffer.Stride(), properties, &image_);

    return SUCCEEDED(hr);
}

void Renderer::DrawVariableStroke(const ccl::doc::Stroke& stroke) noexcept {
    // Direct2D strokes a path at one width, so a stroke whose width changes has
    // to be drawn segment by segment. Long segments are subdivided so the width
    // ramps smoothly -- that is what gives a straight line, which is only two
    // points, a taper from its start pressure to its end pressure.
    constexpr float kSubdivisionLength = 4.0f;
    constexpr int kMaxSubdivisions = 64;

    for (size_t i = 1; i < stroke.points.size(); ++i) {
        const ccl::doc::StrokePoint& from = stroke.points[i - 1];
        const ccl::doc::StrokePoint& to = stroke.points[i];

        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float length = std::sqrt(dx * dx + dy * dy);

        int steps = static_cast<int>(length / kSubdivisionLength);
        steps = std::clamp(steps, 1, kMaxSubdivisions);

        for (int step = 0; step < steps; ++step) {
            const float t0 = static_cast<float>(step) / static_cast<float>(steps);
            const float t1 =
                static_cast<float>(step + 1) / static_cast<float>(steps);
            const float mid = (t0 + t1) * 0.5f;

            const float width = from.width + (to.width - from.width) * mid;

            target_->DrawLine(
                D2D1::Point2F(from.x + dx * t0, from.y + dy * t0),
                D2D1::Point2F(from.x + dx * t1, from.y + dy * t1), brush_.Get(),
                width, strokeStyle_.Get());
        }
    }
}

void Renderer::DrawStroke(const ccl::doc::Stroke& stroke) noexcept {
    if (stroke.points.empty() || !brush_) {
        return;
    }

    brush_->SetColor(ToD2D(stroke.color));
    target_->SetAntialiasMode(stroke.antialias
                                  ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                  : D2D1_ANTIALIAS_MODE_ALIASED);

    // A press without movement should still leave a mark.
    if (stroke.points.size() == 1) {
        const auto& point = stroke.points.front();
        const float radius = point.width * 0.5f;
        target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(point.x, point.y), radius, radius),
            brush_.Get());
        return;
    }

    if (stroke.HasVariableWidth()) {
        DrawVariableStroke(stroke);
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    if (context_->Factory() == nullptr ||
        FAILED(context_->Factory()->CreatePathGeometry(&geometry))) {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) {
        return;
    }

    sink->BeginFigure(D2D1::Point2F(stroke.points[0].x, stroke.points[0].y),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < stroke.points.size(); ++i) {
        sink->AddLine(D2D1::Point2F(stroke.points[i].x, stroke.points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    target_->DrawGeometry(geometry.Get(), brush_.Get(),
                          stroke.points.front().width, strokeStyle_.Get());
}

namespace {

// Builds the layout used for both drawing and measuring, so the two can never
// disagree about where the text sits.
Microsoft::WRL::ComPtr<IDWriteTextLayout> BuildLayout(
    IDWriteFactory* writer, const ccl::doc::TextAnnotation& text) noexcept {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (writer == nullptr || text.text.empty()) {
        return layout;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(writer->CreateTextFormat(
            text.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            text.fontSize, L"", &format))) {
        return layout;
    }

    // Wrapping is left to the line breaks that were actually typed.
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // A large but finite box. FLT_MAX invites overflow in the layout maths;
    // this is far beyond any capture yet still arithmetic-safe.
    constexpr float kUnbounded = 1.0e6f;

    if (FAILED(writer->CreateTextLayout(
            text.text.c_str(), static_cast<UINT32>(text.text.size()),
            format.Get(), kUnbounded, kUnbounded, &layout))) {
        layout.Reset();
    }
    return layout;
}

}  // namespace

bool Renderer::MeasureText(const ccl::doc::TextAnnotation& text,
                           D2D1_RECT_F& bounds) noexcept {
    if (context_ == nullptr) {
        return false;
    }

    const auto layout = BuildLayout(context_->Text(), text);
    if (!layout) {
        return false;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return false;
    }

    bounds = D2D1::RectF(text.x, text.y, text.x + metrics.width,
                         text.y + metrics.height);
    return true;
}

void Renderer::DrawText(const ccl::doc::TextAnnotation& text) noexcept {
    if (text.text.empty() || !brush_ || context_ == nullptr) {
        return;
    }

    const auto layout = BuildLayout(context_->Text(), text);
    if (!layout) {
        return;
    }

    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const D2D1_POINT_2F origin = D2D1::Point2F(text.x, text.y);

    // Screenshots are busy backgrounds, so the shadow and outline exist to keep
    // text readable rather than for decoration. Both are drawn by offsetting
    // the same layout, which costs a few extra draws but needs no geometry.
    if (text.shadow) {
        const float offset = std::max(1.0f, text.fontSize * 0.06f);
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
        target_->DrawTextLayout(
            D2D1::Point2F(origin.x + offset, origin.y + offset), layout.Get(),
            brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    if (text.outline) {
        const float offset = std::max(1.0f, text.fontSize * 0.05f);
        brush_->SetColor(ToD2D(text.outlineColor));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                target_->DrawTextLayout(
                    D2D1::Point2F(origin.x + static_cast<float>(dx) * offset,
                                  origin.y + static_cast<float>(dy) * offset),
                    layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }

    brush_->SetColor(ToD2D(text.color));
    target_->DrawTextLayout(origin, layout.Get(), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::Draw(const ccl::view::ViewState& view,
                    const ccl::doc::Stroke* active,
                    const BrushCursor* cursor) noexcept {
    const bool measure = !measuredFirstDraw_;
    ccl::timing::Stopwatch watch;

    if (!EnsureTarget()) {
        return;
    }
    if (measure) {
        watch.Lap(L"  d2d hwnd target");
    }

    EnsureImageBitmap();
    if (measure) {
        watch.Lap(L"  d2d image bitmap");
    }

    target_->BeginDraw();

    // Clearing to the outline colour and insetting the content by the border
    // width leaves exactly a one pixel frame around the capture.
    target_->Clear(D2D1::ColorF(0.29f, 0.29f, 0.29f));

    // Zoom and scroll are applied as a transform, so annotations are stored in
    // image coordinates and stay locked to the picture at any zoom level.
    const float zoom = view.Zoom();
    const POINT scroll = view.Scroll();
    const auto inset = static_cast<float>(kWindowBorder);

    target_->SetTransform(
        D2D1::Matrix3x2F::Scale(zoom, zoom) *
        D2D1::Matrix3x2F::Translation(inset - static_cast<float>(scroll.x),
                                      inset - static_cast<float>(scroll.y)));

    if (image_) {
        const D2D1_SIZE_F size = image_->GetSize();
        const D2D1_BITMAP_INTERPOLATION_MODE interpolation =
            smoothScaling_ ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                           : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        target_->DrawBitmap(image_.Get(),
                            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                            1.0f, interpolation);
    }

    if (document_ != nullptr) {
        for (const auto& annotation : document_->Annotations()) {
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke:
                    DrawStroke(annotation.stroke);
                    break;
                case ccl::doc::AnnotationKind::Text:
                    DrawText(annotation.text);
                    break;
            }
        }
    }
    if (active != nullptr) {
        DrawStroke(*active);
    }

    if (cursor != nullptr && brush_ && cursor->radius > 0.0f) {
        // Two rings, dark outside and light inside, so the outline stays
        // visible whatever it happens to be sitting on. The width is divided
        // by the zoom because the transform would otherwise scale it too.
        //
        // Always drawn smooth: at typical brush sizes a jagged ring is too
        // small to read, so whether the brush antialiases is shown by the ring
        // changing colour instead.
        const float lineWidth = 1.0f / zoom;
        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f));
        target_->DrawEllipse(D2D1::Ellipse(cursor->position,
                                           cursor->radius + lineWidth,
                                           cursor->radius + lineWidth),
                             brush_.Get(), lineWidth);

        brush_->SetColor(cursor->antialias
                             ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f)
                             : D2D1::ColorF(1.0f, 0.72f, 0.25f, 0.95f));
        target_->DrawEllipse(
            D2D1::Ellipse(cursor->position, cursor->radius, cursor->radius),
            brush_.Get(), lineWidth);
    }

    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }

    if (measure) {
        watch.Lap(L"  d2d present");
        measuredFirstDraw_ = true;
    }
}

}  // namespace ccl::render

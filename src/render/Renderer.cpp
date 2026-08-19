#include "render/Renderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <vector>

#include "doc/Document.h"
#include "render/D2DContext.h"
#include "render/SelectionGeometry.h"
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
    // Nothing worked out for the old picture's annotations means anything for
    // the new one's.
    effectCache_.clear();
    effectSource_.clear();
    effectMask_.clear();
    geometryCache_.clear();
    layoutCache_.clear();
}

void Renderer::Resize(UINT width, UINT height) noexcept {
    if (windowTarget_ && width > 0 && height > 0) {
        windowTarget_->Resize(D2D1::SizeU(width, height));
    }
}

void Renderer::InvalidateEffect(unsigned int id) noexcept {
    effectCache_.erase(id);
}

void Renderer::InvalidateText(unsigned int id) noexcept {
    layoutCache_.erase(id);
}

void Renderer::ReportStats() const noexcept {
    ccl::timing::ReportFrames(L"  picture", pictureStats_);
    ccl::timing::ReportFrames(L"  annotations", annotationStats_);
    ccl::timing::ReportFrames(L"  raster", rasterStats_);
    ccl::timing::ReportFrames(L"  present", presentStats_);
}

// Everything cached is tied to a device that has gone, apart from the text
// layouts, which belong to DirectWrite rather than to the render target.
void Renderer::DiscardDeviceResources() noexcept {
    effectCache_.clear();
    geometryCache_.clear();
    highlightLayer_.Reset();
    brush_.Reset();
    image_.Reset();
    target_.Reset();
    windowTarget_.Reset();
}

void Renderer::PruneCaches() noexcept {
    if (document_ == nullptr) {
        return;
    }
    const size_t count = document_->Annotations().size();

    // Undoing leaves results behind for annotations that are no longer in the
    // list, and redoing wants them straight back, so they are not dropped the
    // moment they go unused. Only once there are clearly more than the picture
    // could account for is the lot thrown away and built again as needed.
    const size_t limit = count * 2 + 32;
    if (effectCache_.size() > limit) {
        effectCache_.clear();
    }
    // The sources go with them: without one an effect cannot be processed at
    // all, and they are the largest thing kept here.
    if (effectSource_.size() > limit) {
        effectSource_.clear();
    }
    // The shapes are taken at the same moment as the sources and are no use
    // without them, so they go on the same terms.
    if (effectMask_.size() > limit) {
        effectMask_.clear();
    }
    if (geometryCache_.size() > limit) {
        geometryCache_.clear();
    }
    if (layoutCache_.size() > limit) {
        layoutCache_.clear();
    }
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

    windowTarget_ = context_->CreateHwndTarget(
        hwnd_, static_cast<UINT>(client.right - client.left),
        static_cast<UINT>(client.bottom - client.top));
    if (!windowTarget_) {
        return false;
    }
    target_ = windowTarget_;

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

namespace {

// The area a stroke covers, in image coordinates, widened by the thickest it
// gets so that nothing is clipped off the ends or the sides.
// `arrowReach` is how far an arrowhead extends from the point it sits on, as a
// multiple of the width there. Passed in because it comes from the settings,
// which this has no way of its own to reach.
D2D1_RECT_F StrokeBounds(const ccl::doc::Stroke& stroke,
                         float arrowReach) noexcept {
    float left = stroke.points.front().x;
    float top = stroke.points.front().y;
    float right = left;
    float bottom = top;
    float widest = 0.0f;

    for (const ccl::doc::StrokePoint& point : stroke.points) {
        left = (std::min)(left, point.x);
        top = (std::min)(top, point.y);
        right = (std::max)(right, point.x);
        bottom = (std::max)(bottom, point.y);
        widest = (std::max)(widest, point.width);
    }

    float margin = widest * 0.5f + 1.0f;

    // An arrowhead reaches further from its point than the line does: ahead of
    // it by the head's length, and out to either side by half its width. The
    // highlighter draws into a layer bounded by this, so a box that only
    // allowed for the line would cut the heads off.
    if (!stroke.arrows.empty()) {
        margin = (std::max)(margin, widest * arrowReach + 1.0f);
    }

    return D2D1::RectF(left - margin, top - margin, right + margin,
                       bottom + margin);
}

}  // namespace

void Renderer::DrawStroke(const ccl::doc::Stroke& stroke,
                          unsigned int id) noexcept {
    if (stroke.points.empty() || !brush_) {
        return;
    }

    if (!stroke.highlighter) {
        DrawStrokeShape(stroke, id);
        return;
    }

    // Drawn opaque into a layer and composited once, so the wash comes out even
    // wherever the stroke crosses itself. Drawing the segments translucent
    // instead would darken every overlap, which is exactly what a highlighter
    // does not do.
    //
    // The layer object is made once and reused, and it is bounded to the
    // stroke rather than left unbounded: an unbounded layer needs an
    // intermediate the size of the whole window, for every highlighter stroke,
    // on every frame.
    if (!highlightLayer_ && FAILED(target_->CreateLayer(nullptr,
                                                        &highlightLayer_))) {
        DrawStrokeShape(stroke, id);
        return;
    }

    target_->PushLayer(
        D2D1::LayerParameters(
            StrokeBounds(stroke,
                         arrowScale_ * (std::max)(1.0f, arrowAspect_)),
            nullptr,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::IdentityMatrix(),
                              ccl::doc::kHighlighterOpacity),
        highlightLayer_.Get());
    DrawStrokeShape(stroke, id);
    target_->PopLayer();
}

ID2D1Geometry* Renderer::StrokeGeometry(const ccl::doc::Stroke& stroke,
                                        unsigned int id) noexcept {
    if (id != 0) {
        const auto cached = geometryCache_.find(id);
        if (cached != geometryCache_.end()) {
            return cached->second.Get();
        }
    }

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    if (context_->Factory() == nullptr ||
        FAILED(context_->Factory()->CreatePathGeometry(&geometry))) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) {
        return nullptr;
    }

    sink->BeginFigure(D2D1::Point2F(stroke.points[0].x, stroke.points[0].y),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < stroke.points.size(); ++i) {
        sink->AddLine(D2D1::Point2F(stroke.points[i].x, stroke.points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    // The stroke being drawn is a different shape every frame, so it is built
    // and thrown away; a finished one is kept, since it never changes again.
    if (id == 0) {
        transientGeometry_ = geometry;
        return transientGeometry_.Get();
    }
    return (geometryCache_[id] = geometry).Get();
}

void Renderer::DrawStrokeShape(const ccl::doc::Stroke& stroke,
                               unsigned int id) noexcept {
    brush_->SetColor(ToD2D(stroke.color));
    target_->SetAntialiasMode(stroke.antialias
                                  ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                  : D2D1_ANTIALIAS_MODE_ALIASED);

    // The line first, then whatever is placed along it. Split in two because
    // the line has three ways of being drawn, each leaving early, and anything
    // added after the last of them would never be reached by the other two.
    DrawStrokeLine(stroke, id);
    DrawStrokeArrows(stroke);
}

namespace {

// The head at the origin, pointing along +x: the base sits on the point it
// was placed at and the tip reaches ahead of it, the way the line was going.
//
// `radius` takes the corners off with an arc tangent to both edges. Done that
// way rather than by tracing the outline with a thick round-jointed pen: that
// pushes every edge outward, not just the corners, and what comes out is a
// fatter head rather than a blunter one.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> ArrowGeometry(ID2D1Factory* factory,
                                                        float width,
                                                        float length,
                                                        float radius) noexcept {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (factory == nullptr || FAILED(factory->CreatePathGeometry(&path))) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return nullptr;
    }

    const float half = width * 0.5f;
    const D2D1_POINT_2F corners[3] = {D2D1::Point2F(length, 0.0f),
                                      D2D1::Point2F(0.0f, half),
                                      D2D1::Point2F(0.0f, -half)};

    if (radius <= 0.0f) {
        sink->BeginFigure(corners[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(corners[2]);
        sink->AddLine(corners[1]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        return SUCCEEDED(sink->Close()) ? path : nullptr;
    }

    // Where each corner's arc meets the two edges running into it.
    struct Cut {
        D2D1_POINT_2F from;
        D2D1_POINT_2F to;
    };
    Cut cuts[3];

    for (int i = 0; i < 3; ++i) {
        const D2D1_POINT_2F& here = corners[i];
        const D2D1_POINT_2F& previous = corners[(i + 2) % 3];
        const D2D1_POINT_2F& next = corners[(i + 1) % 3];

        const auto toward = [&here](const D2D1_POINT_2F& target) {
            const float dx = target.x - here.x;
            const float dy = target.y - here.y;
            const float span = std::sqrt(dx * dx + dy * dy);
            return span > 0.0f ? D2D1::Point2F(dx / span, dy / span)
                               : D2D1::Point2F(0.0f, 0.0f);
        };
        const auto spanTo = [&here](const D2D1_POINT_2F& target) {
            const float dx = target.x - here.x;
            const float dy = target.y - here.y;
            return std::sqrt(dx * dx + dy * dy);
        };

        const D2D1_POINT_2F back = toward(previous);
        const D2D1_POINT_2F ahead = toward(next);

        const float cosine = std::clamp(back.x * ahead.x + back.y * ahead.y,
                                        -1.0f, 1.0f);
        const float tangent = std::tan(std::acos(cosine) * 0.5f);
        float distance = tangent > 0.0001f ? radius / tangent : 0.0f;

        // Never past the middle of either edge, so two corners cannot eat into
        // one another and turn the head inside out.
        distance = (std::min)(distance, spanTo(previous) * 0.5f);
        distance = (std::min)(distance, spanTo(next) * 0.5f);

        cuts[i].from = D2D1::Point2F(here.x + back.x * distance,
                                     here.y + back.y * distance);
        cuts[i].to = D2D1::Point2F(here.x + ahead.x * distance,
                                   here.y + ahead.y * distance);
    }

    sink->BeginFigure(cuts[0].to, D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i <= 3; ++i) {
        const Cut& cut = cuts[i % 3];
        sink->AddLine(cut.from);
        sink->AddArc(D2D1::ArcSegment(cut.to, D2D1::SizeF(radius, radius), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    return SUCCEEDED(sink->Close()) ? path : nullptr;
}

// The same head, grown until it measures `width` by `length` on the outside.
//
// Taking the corners off pulls them back, and the tip is sharp enough that it
// moves a long way -- so the size asked for and the size drawn come apart, and
// the two settings start interfering with one another. Measured rather than
// worked out with trigonometry, because the angles change as the core grows.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> SizedArrowGeometry(
    ID2D1Factory* factory, float width, float length, float radius) noexcept {
    if (radius <= 0.0f) {
        return ArrowGeometry(factory, width, length, 0.0f);
    }

    float coreWidth = width;
    float coreLength = length;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> head;

    // Three passes settle it to within a fraction of a pixel.
    for (int pass = 0; pass < 3; ++pass) {
        head = ArrowGeometry(factory, coreWidth, coreLength, radius);
        if (!head) {
            return nullptr;
        }
        D2D1_RECT_F bounds{};
        if (FAILED(head->GetBounds(nullptr, &bounds))) {
            return head;
        }
        const float drawnLength = bounds.right - bounds.left;
        const float drawnWidth = bounds.bottom - bounds.top;
        if (drawnLength <= 0.0f || drawnWidth <= 0.0f) {
            return head;
        }
        if (std::abs(drawnLength - length) < 0.25f &&
            std::abs(drawnWidth - width) < 0.25f) {
            break;
        }
        coreLength += length - drawnLength;
        coreWidth += width - drawnWidth;
    }
    return head;
}

}  // namespace

void Renderer::DrawStrokeArrows(const ccl::doc::Stroke& stroke) noexcept {
    if (stroke.arrows.empty() || context_ == nullptr || !brush_ || !target_) {
        return;
    }
    ID2D1Factory* factory = context_->Factory();
    if (factory == nullptr) {
        return;
    }

    // The colour and the antialias mode are the line's; whoever called has
    // already set them, and a head is part of the same mark.
    for (const ccl::doc::StrokeArrow& arrow : stroke.arrows) {
        const unsigned int index = arrow.at;
        if (index == 0 || index >= stroke.points.size()) {
            continue;
        }
        const ccl::doc::StrokePoint& at = stroke.points[index];
        const ccl::doc::StrokePoint& behind = stroke.points[index - 1];

        const float dx = at.x - behind.x;
        const float dy = at.y - behind.y;
        const float travelled = std::sqrt(dx * dx + dy * dy);
        if (travelled < 0.001f) {
            continue;
        }

        const float width = at.width * arrowScale_;
        const Microsoft::WRL::ComPtr<ID2D1PathGeometry> head =
            SizedArrowGeometry(factory, width, width * arrowAspect_,
                               width * arrowRounding_);
        if (!head) {
            continue;
        }

        // Composed with whatever is already in place rather than replacing it:
        // the zoom and the scroll live in that transform, and a head drawn
        // outside them would sit at the wrong size in the wrong place.
        D2D1_MATRIX_3X2_F view{};
        target_->GetTransform(&view);

        // The way the line was going, plus however far the head has been
        // nudged from it.
        const float degrees =
            std::atan2(dy, dx) * 180.0f / std::numbers::pi_v<float> + arrow.turn;
        target_->SetTransform(
            D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(0.0f, 0.0f)) *
            D2D1::Matrix3x2F::Translation(at.x, at.y) * view);
        target_->FillGeometry(head.Get(), brush_.Get());
        target_->SetTransform(view);
    }
}

void Renderer::DrawStrokeLine(const ccl::doc::Stroke& stroke,
                              unsigned int id) noexcept {
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

    ID2D1Geometry* geometry = StrokeGeometry(stroke, id);
    if (geometry == nullptr) {
        return;
    }

    target_->DrawGeometry(geometry, brush_.Get(),
                          stroke.points.front().width, strokeStyle_.Get());
}

ID2D1Geometry* Renderer::AreaGeometry(const ccl::doc::AreaAnnotation& area,
                                      unsigned int id) noexcept {
    const auto cached = geometryCache_.find(id);
    if (cached != geometryCache_.end()) {
        return cached->second.Get();
    }
    if (context_ == nullptr) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID2D1Geometry> geometry =
        BuildSelectionGeometry(context_->Factory(), area.shape);
    if (!geometry) {
        return nullptr;
    }
    return (geometryCache_[id] = geometry).Get();
}

void Renderer::DrawArea(const ccl::doc::AreaAnnotation& area,
                        unsigned int id) noexcept {
    if (!brush_) {
        return;
    }
    ID2D1Geometry* geometry = AreaGeometry(area, id);
    if (geometry == nullptr) {
        return;
    }

    D2D1_COLOR_F color = ToD2D(area.color);
    color.a *= area.opacity;

    brush_->SetColor(color);
    target_->SetAntialiasMode(area.antialias
                                  ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                  : D2D1_ANTIALIAS_MODE_ALIASED);

    if (area.width > 0.0f) {
        // Round joins and caps, as the pen uses, so a line round a hand-drawn
        // shape does not sprout spikes at its corners.
        target_->DrawGeometry(geometry, brush_.Get(), area.width,
                              strokeStyle_.Get());
        return;
    }
    target_->FillGeometry(geometry, brush_.Get());
}

namespace {

// Builds the layout used for both drawing and measuring, so the two can never
// disagree about where the text sits.
//
// Range styling that affects metrics -- weight, slant, decorations -- is
// applied here. Per-range colour needs a render target to make brushes from,
// so it is applied separately by the caller that draws.
Microsoft::WRL::ComPtr<IDWriteTextLayout> BuildLayout(
    IDWriteFactory* writer, const ccl::doc::TextAnnotation& text) noexcept {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (writer == nullptr || text.text.empty()) {
        return layout;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(writer->CreateTextFormat(
            text.fontFamily.c_str(), nullptr,
            text.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            text.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, text.fontSize, L"", &format))) {
        return layout;
    }

    // A large but finite box. FLT_MAX invites overflow in the layout maths;
    // this is far beyond any capture yet still arithmetic-safe.
    constexpr float kUnbounded = 1.0e6f;

    // Wrapped at the width it was typed at, so the drawn result breaks in the
    // same places the editor did.
    const bool wraps = text.wrapWidth > 0.0f;
    format->SetWordWrapping(wraps ? DWRITE_WORD_WRAPPING_WRAP
                                  : DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(writer->CreateTextLayout(
            text.text.c_str(), static_cast<UINT32>(text.text.size()),
            format.Get(), wraps ? text.wrapWidth : kUnbounded, kUnbounded,
            &layout))) {
        layout.Reset();
        return layout;
    }

    // Underline and strikethrough are properties of a range rather than of the
    // format, so they are applied after the fact.
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.text.size())};
    if (text.underline) {
        layout->SetUnderline(TRUE, all);
    }
    if (text.strikethrough) {
        layout->SetStrikethrough(TRUE, all);
    }

    for (const ccl::doc::TextRun& run : text.runs) {
        const DWRITE_TEXT_RANGE range{run.start, run.length};
        layout->SetFontWeight(
            run.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            range);
        layout->SetFontStyle(
            run.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            range);
        layout->SetUnderline(run.underline ? TRUE : FALSE, range);
        layout->SetStrikethrough(run.strikethrough ? TRUE : FALSE, range);
        if (run.fontSize > 0.0f) {
            layout->SetFontSize(run.fontSize, range);
        }
        if (!run.fontFamily.empty()) {
            layout->SetFontFamilyName(run.fontFamily.c_str(), range);
        }
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

namespace {

// Averages each block down to one colour. Cheap, and the result is obviously
// deliberate rather than looking like a rendering fault.
void ApplyMosaic(std::vector<unsigned char>& pixels, int width, int height,
                 int block) noexcept {
    block = std::max(2, block);

    for (int blockTop = 0; blockTop < height; blockTop += block) {
        for (int blockLeft = 0; blockLeft < width; blockLeft += block) {
            const int right = std::min(blockLeft + block, width);
            const int bottom = std::min(blockTop + block, height);

            unsigned int totals[3] = {0, 0, 0};
            unsigned int count = 0;
            for (int y = blockTop; y < bottom; ++y) {
                for (int x = blockLeft; x < right; ++x) {
                    const size_t at = (static_cast<size_t>(y) * width + x) * 4u;
                    totals[0] += pixels[at + 0];
                    totals[1] += pixels[at + 1];
                    totals[2] += pixels[at + 2];
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }

            const unsigned char average[3] = {
                static_cast<unsigned char>(totals[0] / count),
                static_cast<unsigned char>(totals[1] / count),
                static_cast<unsigned char>(totals[2] / count)};

            for (int y = blockTop; y < bottom; ++y) {
                for (int x = blockLeft; x < right; ++x) {
                    const size_t at = (static_cast<size_t>(y) * width + x) * 4u;
                    pixels[at + 0] = average[0];
                    pixels[at + 1] = average[1];
                    pixels[at + 2] = average[2];
                }
            }
        }
    }
}

// Three box passes, which approximates a Gaussian closely enough and stays
// linear in the radius rather than quadratic.
void ApplyBlur(std::vector<unsigned char>& pixels, int width, int height,
               int radius) noexcept {
    radius = std::clamp(radius, 1, 64);
    std::vector<unsigned char> scratch(pixels.size());

    for (int pass = 0; pass < 3; ++pass) {
        // Horizontal.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned int totals[3] = {0, 0, 0};
                int count = 0;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const int sample = std::clamp(x + offset, 0, width - 1);
                    const size_t at =
                        (static_cast<size_t>(y) * width + sample) * 4u;
                    totals[0] += pixels[at + 0];
                    totals[1] += pixels[at + 1];
                    totals[2] += pixels[at + 2];
                    ++count;
                }
                const size_t at = (static_cast<size_t>(y) * width + x) * 4u;
                scratch[at + 0] = static_cast<unsigned char>(totals[0] / count);
                scratch[at + 1] = static_cast<unsigned char>(totals[1] / count);
                scratch[at + 2] = static_cast<unsigned char>(totals[2] / count);
                scratch[at + 3] = 255;
            }
        }

        // Vertical.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned int totals[3] = {0, 0, 0};
                int count = 0;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const int sample = std::clamp(y + offset, 0, height - 1);
                    const size_t at =
                        (static_cast<size_t>(sample) * width + x) * 4u;
                    totals[0] += scratch[at + 0];
                    totals[1] += scratch[at + 1];
                    totals[2] += scratch[at + 2];
                    ++count;
                }
                const size_t at = (static_cast<size_t>(y) * width + x) * 4u;
                pixels[at + 0] = static_cast<unsigned char>(totals[0] / count);
                pixels[at + 1] = static_cast<unsigned char>(totals[1] / count);
                pixels[at + 2] = static_cast<unsigned char>(totals[2] / count);
                pixels[at + 3] = 255;
            }
        }
    }
}

}  // namespace

// Draws the annotations below `limit` onto a cropped piece of the picture,
// which sits at (left, top) in image coordinates. Returns false if it could
// not be done, in which case the piece is left as the picture alone.
bool Renderer::OverlayAnnotations(ccl::capture::DibBuffer& region, int left,
                                  int top, size_t limit) noexcept {
    if (context_ == nullptr || document_ == nullptr || !region.IsValid()) {
        return false;
    }
    IWICImagingFactory* imaging = context_->Imaging();
    if (imaging == nullptr || context_->Factory() == nullptr) {
        return false;
    }

    const UINT width = static_cast<UINT>(region.Width());
    const UINT height = static_cast<UINT>(region.Height());
    const UINT stride = region.Stride();
    const UINT bytes = stride * height;

    // The screen grab leaves the alpha bytes at zero. Read as premultiplied
    // that is a fully transparent picture, and everything drawn on top of it
    // would come back as a smear, so the piece is made opaque first.
    auto* pixels = static_cast<unsigned char*>(region.Pixels());
    for (UINT i = 3; i < bytes; i += 4) {
        pixels[i] = 255;
    }

    // Made the same way the off-screen render makes its surface, which is the
    // call known to produce something this can draw into, and seeded with the
    // piece of picture through a lock.
    Microsoft::WRL::ComPtr<IWICBitmap> surface;
    if (FAILED(imaging->CreateBitmap(width, height,
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &surface))) {
        return false;
    }
    {
        const WICRect whole{0, 0, static_cast<INT>(width),
                            static_cast<INT>(height)};
        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
        if (FAILED(surface->Lock(&whole, WICBitmapLockWrite, &lock))) {
            return false;
        }
        UINT lockStride = 0;
        UINT lockSize = 0;
        BYTE* lockPixels = nullptr;
        if (FAILED(lock->GetStride(&lockStride)) ||
            FAILED(lock->GetDataPointer(&lockSize, &lockPixels))) {
            return false;
        }
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(lockPixels + static_cast<size_t>(y) * lockStride,
                        pixels + static_cast<size_t>(y) * stride, stride);
        }
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> offscreen;
    if (FAILED(context_->Factory()->CreateWicBitmapRenderTarget(
            surface.Get(), properties, &offscreen))) {
        return false;
    }

    // Deliberately no picture bitmap: the piece already holds the picture, so
    // there is nothing here that grows with the size of the capture.
    auto savedTarget = target_;
    auto savedBrush = brush_;
    auto savedCache = std::move(effectCache_);
    effectCache_.clear();

    target_ = offscreen;
    brush_.Reset();

    bool ok = SUCCEEDED(target_->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::White), &brush_));

    if (ok) {
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(left), -static_cast<float>(top)));

        const ccl::doc::AnnotationList& annotations = document_->Annotations();
        const size_t drawn = (std::min)(limit, annotations.size());
        for (size_t i = 0; i < drawn; ++i) {
            const ccl::doc::Annotation& annotation = annotations[i];
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke:
                    DrawStroke(annotation.stroke, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Text:
                    DrawText(annotation.text, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Effect:
                    DrawEffect(annotation.effect, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Area:
                    DrawArea(annotation.area, annotation.id);
                    break;
            }
        }

        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        ok = SUCCEEDED(target_->EndDraw());
    }

    target_ = savedTarget;
    brush_ = savedBrush;
    effectCache_ = std::move(savedCache);

    return ok && SUCCEEDED(surface->CopyPixels(nullptr, stride, bytes, pixels));
}

void Renderer::CaptureEffectSources() noexcept {
    if (document_ == nullptr || !document_->IsValid()) {
        return;
    }

    const ccl::doc::AnnotationList& annotations = document_->Annotations();
    for (size_t i = 0; i < annotations.size(); ++i) {
        const ccl::doc::Annotation& annotation = annotations[i];
        if (annotation.kind != ccl::doc::AnnotationKind::Effect ||
            effectSource_.count(annotation.id) != 0) {
            continue;
        }

        const ccl::doc::EffectAnnotation& effect = annotation.effect;
        const RECT area{static_cast<LONG>(effect.left),
                        static_cast<LONG>(effect.top),
                        static_cast<LONG>(effect.right),
                        static_cast<LONG>(effect.bottom)};

        // Only the region, taken straight out of the picture. Working through
        // a full-size off-screen render instead put the whole capture through
        // software rendering for the sake of a small rectangle.
        ccl::capture::DibBuffer region = document_->Image().Crop(area);
        if (!region.IsValid()) {
            continue;
        }

        // Only the annotations below this one, so an effect never takes its
        // own output as its input, and anything drawn afterwards stays on top
        // of it rather than being baked into it. If this cannot be done, the
        // picture on its own still hides the area, which matters more than
        // the strokes being included in what is hidden.
        const bool overlaid = OverlayAnnotations(
            region, static_cast<int>(area.left), static_cast<int>(area.top), i);

        if (ccl::timing::g_enabled) {
            wchar_t line[160];
            ::swprintf_s(line,
                         L"[timing] effect source          %4dx%-4d  %2zu below"
                         L"  overlay %s\n",
                         region.Width(), region.Height(), i,
                         overlaid ? L"ok" : L"FAILED");
            ccl::timing::Write(line);
        }

        // Stored either way. Retrying every frame is how a failure here turned
        // into the effect never appearing at all.
        effectSource_[annotation.id] = std::move(region);

        // Worked out alongside the source and kept for as long: what the
        // effect hides is settled when it is placed.
        effectMask_[annotation.id] = BuildEffectMask(effect);
    }
}

std::vector<unsigned char> Renderer::BuildEffectMask(
    const ccl::doc::EffectAnnotation& effect) noexcept {
    std::vector<unsigned char> mask;
    if (context_ == nullptr || effect.mask.empty()) {
        return mask;
    }

    IWICImagingFactory* imaging = context_->Imaging();
    ID2D1Factory* factory = context_->Factory();
    if (imaging == nullptr || factory == nullptr) {
        return mask;
    }

    const auto width = static_cast<UINT>(effect.right - effect.left);
    const auto height = static_cast<UINT>(effect.bottom - effect.top);
    if (width == 0 || height == 0) {
        return mask;
    }

    const Microsoft::WRL::ComPtr<ID2D1Geometry> shape =
        BuildSelectionGeometry(factory, effect.mask);
    if (!shape) {
        return mask;
    }

    // Drawn on a surface of its own, the size of the effect's box. This runs
    // before the frame opens, for the same reason the sources are taken then:
    // Direct2D will not be told to draw somewhere else part way through.
    Microsoft::WRL::ComPtr<IWICBitmap> surface;
    if (FAILED(imaging->CreateBitmap(width, height,
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &surface))) {
        return mask;
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> offscreen;
    if (FAILED(factory->CreateWicBitmapRenderTarget(surface.Get(), properties,
                                                    &offscreen))) {
        return mask;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(offscreen->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), &brush))) {
        return mask;
    }

    offscreen->BeginDraw();
    offscreen->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    // The shape is in image coordinates and the surface starts at the corner
    // of the box, so the one goes over to the other here. This is the only
    // place the two coordinate systems meet.
    offscreen->SetTransform(
        D2D1::Matrix3x2F::Translation(-effect.left, -effect.top));
    offscreen->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    offscreen->FillGeometry(shape.Get(), brush.Get());
    offscreen->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(offscreen->EndDraw())) {
        return mask;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4u);
    if (FAILED(surface->CopyPixels(nullptr, width * 4u,
                                   static_cast<UINT>(pixels.size()),
                                   pixels.data()))) {
        return mask;
    }

    // Only the coverage is wanted; the colour was never anything but white.
    mask.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = pixels[i * 4u + 3u];
    }
    return mask;
}

ID2D1Bitmap* Renderer::EffectBitmap(const ccl::doc::EffectAnnotation& effect,
                                    unsigned int id) noexcept {
    const auto cached = effectCache_.find(id);
    if (cached != effectCache_.end()) {
        return cached->second.Get();
    }

    if (!target_ || document_ == nullptr || !document_->IsValid()) {
        return nullptr;
    }

    // What was under the area when the effect was placed, picture and drawing
    // together. Taken from the bare capture instead, the area would come back
    // showing pixels that never had the pen strokes in them -- which read as
    // the strokes having been rubbed out rather than obscured.
    const auto stored = effectSource_.find(id);
    if (stored == effectSource_.end() || !stored->second.IsValid()) {
        return nullptr;
    }
    const ccl::capture::DibBuffer& source = stored->second;

    const int width = source.Width();
    const int height = source.Height();
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4u);
    const auto* origin = static_cast<const unsigned char*>(source.Pixels());
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4u,
                    origin + static_cast<size_t>(y) * source.Stride(),
                    static_cast<size_t>(width) * 4u);
    }

    if (effect.kind == ccl::doc::EffectKind::Mosaic) {
        ApplyMosaic(pixels, width, height, static_cast<int>(effect.strength));
    } else {
        ApplyBlur(pixels, width, height, static_cast<int>(effect.strength));
    }

    // Alpha is written here and never read. What these pixels came from has
    // none worth having -- a screen grab leaves it at zero -- so taking it as
    // it stands would make the whole area vanish. The shape is the only thing
    // that gets to say what shows.
    const auto shape = effectMask_.find(id);
    const bool masked = shape != effectMask_.end() &&
                        shape->second.size() ==
                            static_cast<size_t>(width) * height;

    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const unsigned int coverage = masked ? shape->second[i] : 255u;
        unsigned char* pixel = pixels.data() + i * 4u;
        // Premultiplied, so the colour is scaled by the coverage: at a soft
        // edge the two have to agree or the join shows as a bright fringe.
        for (int channel = 0; channel < 3; ++channel) {
            pixel[channel] =
                static_cast<unsigned char>(pixel[channel] * coverage / 255u);
        }
        pixel[3] = static_cast<unsigned char>(coverage);
    }

    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height)),
            pixels.data(), static_cast<UINT>(width) * 4u, properties,
            &bitmap))) {
        return nullptr;
    }

    return (effectCache_[id] = bitmap).Get();
}

void Renderer::DrawEffect(const ccl::doc::EffectAnnotation& effect,
                          unsigned int id) noexcept {
    // Zero strength means the area is left as it is; the annotation stays so
    // the effect can be turned back up.
    if (effect.strength <= 0.0f) {
        return;
    }

    ID2D1Bitmap* bitmap = EffectBitmap(effect, id);
    if (bitmap == nullptr) {
        return;
    }

    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    target_->DrawBitmap(
        bitmap, D2D1::RectF(effect.left, effect.top, effect.right, effect.bottom),
        1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

bool Renderer::MeasureLine(const ccl::doc::TextAnnotation& text,
                           float& lineHeight, float& baseline) noexcept {
    if (context_ == nullptr) {
        return false;
    }

    ccl::doc::TextAnnotation probe = text;
    // Needs some content to report metrics for; the styling is what matters.
    if (probe.text.empty()) {
        probe.text = L"A";
        probe.runs.clear();
    }

    const auto layout = BuildLayout(context_->Text(), probe);
    if (!layout) {
        return false;
    }

    DWRITE_LINE_METRICS metrics{};
    UINT32 count = 0;
    if (FAILED(layout->GetLineMetrics(&metrics, 1, &count)) || count == 0) {
        return false;
    }

    lineHeight = metrics.height;
    baseline = metrics.baseline;
    return true;
}

IDWriteTextLayout* Renderer::TextLayout(const ccl::doc::TextAnnotation& text,
                                        unsigned int id) noexcept {
    if (id != 0) {
        const auto cached = layoutCache_.find(id);
        if (cached != layoutCache_.end()) {
            return cached->second.Get();
        }
    }

    // Deliberately without the per-range colours. Those are attached by the
    // caller, after the shadow and the outline have been laid down, because a
    // layout that already carried them would colour those too and turn them
    // into a blurred copy of the text rather than a backing for it.
    auto layout = BuildLayout(context_->Text(), text);
    if (!layout) {
        return nullptr;
    }

    if (id == 0) {
        transientLayout_ = layout;
        return transientLayout_.Get();
    }
    return (layoutCache_[id] = layout).Get();
}

void Renderer::DrawText(const ccl::doc::TextAnnotation& text,
                        unsigned int id) noexcept {
    if (text.text.empty() || !brush_ || context_ == nullptr) {
        return;
    }

    IDWriteTextLayout* layout = TextLayout(text, id);
    if (layout == nullptr) {
        return;
    }

    // A kept layout still carries the colours the last pass attached to it, so
    // they are taken off before the shadow and outline go down. Without this
    // the second frame onwards would draw a coloured shadow.
    if (!text.runs.empty()) {
        layout->SetDrawingEffect(
            nullptr,
            DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.text.size())});
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
            D2D1::Point2F(origin.x + offset, origin.y + offset), layout,
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
                    layout, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }

    // Per-range colour is attached only for the final pass, so the shadow and
    // outline above stay flat. Cheap next to laying the text out, which is
    // what the layout is kept for.
    for (const ccl::doc::TextRun& run : text.runs) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> runBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(ToD2D(run.color),
                                                     &runBrush))) {
            layout->SetDrawingEffect(runBrush.Get(),
                                     DWRITE_TEXT_RANGE{run.start, run.length});
        }
    }

    brush_->SetColor(ToD2D(text.color));
    target_->DrawTextLayout(origin, layout, brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::Draw(const ccl::view::ViewState& view,
                    const ccl::doc::Stroke* active, const BrushCursor* cursor,
                    const D2D1_RECT_F* highlight, ID2D1Geometry* selection,
                    ID2D1Geometry* removing) noexcept {
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

    // Before the frame opens: this draws to a target of its own, which cannot
    // be done once BeginDraw below has been called.
    CaptureEffectSources();

    target_->BeginDraw();

    // Clearing to the outline colour and insetting the content by the border
    // width leaves exactly a one pixel frame around the capture.
    //
    // While a turn is being previewed the padding colour takes over, so that
    // what shows around the picture is what committing would fill in.
    const bool previewing = previewRotation_ != 0.0f;
    target_->Clear(previewing ? ToD2D(previewFill_)
                              : D2D1::ColorF(0.29f, 0.29f, 0.29f));

    // Zoom and scroll are applied as a transform, so annotations are stored in
    // image coordinates and stay locked to the picture at any zoom level.
    const float zoom = view.Zoom();
    const POINT scroll = view.Scroll();
    const auto inset = static_cast<float>(border_);

    D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(zoom, zoom) *
        D2D1::Matrix3x2F::Translation(inset - static_cast<float>(scroll.x),
                                      inset - static_cast<float>(scroll.y));

    // Turned about the middle of the picture, before the zoom and scroll, so
    // the preview turns about the same point that committing does.
    if (previewing && image_) {
        const D2D1_SIZE_F picture = image_->GetSize();
        transform = D2D1::Matrix3x2F::Rotation(
                        previewRotation_, D2D1::Point2F(picture.width * 0.5f,
                                                        picture.height * 0.5f)) *
                    transform;
    }

    target_->SetTransform(transform);

    const LONGLONG pictureStart = ccl::timing::Mark();
    if (image_) {
        const D2D1_SIZE_F size = image_->GetSize();
        const D2D1_BITMAP_INTERPOLATION_MODE interpolation =
            smoothScaling_ ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                           : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        target_->DrawBitmap(image_.Get(),
                            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                            1.0f, interpolation);
    }
    ccl::timing::AddSince(pictureStats_, pictureStart);

    const LONGLONG annotationStart = ccl::timing::Mark();
    if (document_ != nullptr) {
        for (const auto& annotation : document_->Annotations()) {
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke:
                    DrawStroke(annotation.stroke, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Text:
                    DrawText(annotation.text, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Effect:
                    DrawEffect(annotation.effect, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Area:
                    DrawArea(annotation.area, annotation.id);
                    break;
            }
        }
    }
    if (active != nullptr) {
        // No id: the stroke in progress is a different shape every frame.
        DrawStroke(*active, 0);
    }
    ccl::timing::AddSince(annotationStats_, annotationStart);

    if ((selection != nullptr || removing != nullptr) && brush_) {
        // Drawn on the boundary itself rather than outside it, so that the
        // outline says exactly what is selected. The width is divided by the
        // zoom because the transform would otherwise scale it too.
        const float lineWidth = 1.0f / zoom;
        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (selection != nullptr) {
            brush_->SetColor(D2D1::ColorF(0.35f, 0.65f, 1.0f, 0.9f));
            target_->DrawGeometry(selection, brush_.Get(), lineWidth);
        }
        if (removing != nullptr) {
            brush_->SetColor(D2D1::ColorF(1.0f, 0.3f, 0.3f, 0.9f));
            target_->DrawGeometry(removing, brush_.Get(), lineWidth);
        }
    }

    if (highlight != nullptr && brush_) {
        // Marks what a click would pick up. Drawn slightly outside the text so
        // it frames it rather than striking through it.
        const float lineWidth = 1.0f / zoom;
        const float margin = 2.0f * lineWidth;

        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        brush_->SetColor(D2D1::ColorF(0.35f, 0.65f, 1.0f, 0.9f));
        target_->DrawRectangle(
            D2D1::RectF(highlight->left - margin, highlight->top - margin,
                        highlight->right + margin, highlight->bottom + margin),
            brush_.Get(), lineWidth);
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

    // Direct2D queues the drawing above. Flush carries it out, so that what
    // EndDraw costs after it is the handing over rather than the drawing.
    // Only done while measuring: breaking the batch up is not free, and the
    // split is a question about the program rather than a part of it.
    const LONGLONG rasterStart = ccl::timing::Mark();
    if (ccl::timing::g_enabled) {
        target_->Flush();
    }
    ccl::timing::AddSince(rasterStats_, rasterStart);

    const LONGLONG presentStart = ccl::timing::Mark();
    const HRESULT presented = target_->EndDraw();
    ccl::timing::AddSince(presentStats_, presentStart);

    if (presented == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }

    PruneCaches();

    if (measure) {
        watch.Lap(L"  d2d present");
        measuredFirstDraw_ = true;
    }
}

ccl::capture::DibBuffer Renderer::Flatten() noexcept {
    if (document_ == nullptr || !document_->IsValid()) {
        return {};
    }
    // An effect with nothing to process draws nothing, and an area that was
    // meant to be hidden would go out unhidden. Saving and copying come
    // through here, so the sources are made sure of first.
    CaptureEffectSources();
    // No background and no smoothing: the picture covers the whole surface at
    // its own size, and its pixels are wanted exactly as they are.
    return RenderOffscreen(static_cast<UINT>(document_->Width()),
                           static_cast<UINT>(document_->Height()),
                           D2D1::Matrix3x2F::Identity(), nullptr, false);
}

ccl::capture::DibBuffer Renderer::CaptureView(
    UINT width, UINT height, const ccl::view::ViewState& view) noexcept {
    const float zoom = view.Zoom();
    const POINT scroll = view.Scroll();

    // As for Flatten: what an effect hides has to survive being captured.
    CaptureEffectSources();

    // The window's transform without its border inset, and sized to the area
    // inside the border. Taking the outline too would make the picture two
    // pixels wider every time this was used.
    // Whatever the picture does not cover shows the window's backing colour,
    // the same as it does on screen.
    const D2D1_COLOR_F backing = D2D1::ColorF(0.29f, 0.29f, 0.29f);
    return RenderOffscreen(
        width, height,
        D2D1::Matrix3x2F::Scale(zoom, zoom) *
            D2D1::Matrix3x2F::Translation(-static_cast<float>(scroll.x),
                                          -static_cast<float>(scroll.y)),
        &backing, true);
}

ccl::capture::DibBuffer Renderer::RenderRotated(float degrees,
                                                const ccl::doc::Color& fill,
                                                float scale) noexcept {
    ccl::capture::DibBuffer result;
    if (document_ == nullptr || !document_->IsValid() || scale <= 0.0f) {
        return result;
    }

    const float width = static_cast<float>(document_->Width());
    const float height = static_cast<float>(document_->Height());

    // The box a turned picture needs. Direct2D takes the angle in degrees while
    // the sines want radians, so the conversion is kept here, in the one place
    // that needs both -- apart, the two would drift.
    const float radians = degrees * std::numbers::pi_v<float> / 180.0f;
    const float across = std::abs(std::cos(radians));
    const float down = std::abs(std::sin(radians));
    const float turnedWidth = width * across + height * down;
    const float turnedHeight = width * down + height * across;

    const auto scaled = [scale](float value) {
        return static_cast<UINT>(std::max(1L, std::lround(value * scale)));
    };

    // Turn about the middle of the picture, slide that middle onto the middle
    // of the larger surface, then shrink the lot when a preview asks for it.
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Rotation(
            degrees, D2D1::Point2F(width * 0.5f, height * 0.5f)) *
        D2D1::Matrix3x2F::Translation((turnedWidth - width) * 0.5f,
                                      (turnedHeight - height) * 0.5f) *
        D2D1::Matrix3x2F::Scale(scale, scale);

    // As for saving: what an effect hides has to survive being turned, or the
    // area comes back showing what it was put there to cover.
    CaptureEffectSources();

    const D2D1_COLOR_F background = ToD2D(fill);
    return RenderOffscreen(scaled(turnedWidth), scaled(turnedHeight), transform,
                           &background, true);
}

ccl::capture::DibBuffer Renderer::RenderOffscreen(
    UINT width, UINT height, const D2D1_MATRIX_3X2_F& transform,
    const D2D1_COLOR_F* background, bool interpolate) noexcept {
    ccl::capture::DibBuffer result;
    if (context_ == nullptr || document_ == nullptr || !document_->IsValid() ||
        width == 0 || height == 0) {
        return result;
    }

    IWICImagingFactory* imaging = context_->Imaging();
    if (imaging == nullptr || context_->Factory() == nullptr) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> surface;
    if (FAILED(imaging->CreateBitmap(width, height,
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &surface))) {
        return result;
    }

    // Software rendering, because the result has to be read back on the CPU and
    // a window's target cannot be. Speed does not matter here: this runs when
    // the user saves or transforms, not while drawing.
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> offscreen;
    if (FAILED(context_->Factory()->CreateWicBitmapRenderTarget(
            surface.Get(), properties, &offscreen))) {
        return result;
    }

    // The drawing helpers all work through the members, so the off-screen
    // target takes their place for the duration. Bitmaps and brushes belong to
    // the target that made them and cannot be shared, so the window's are set
    // aside rather than reused.
    auto savedTarget = target_;
    auto savedBrush = brush_;
    auto savedImage = image_;
    auto savedCache = std::move(effectCache_);
    effectCache_.clear();

    target_ = offscreen;
    brush_.Reset();
    image_.Reset();

    bool ok = SUCCEEDED(target_->CreateSolidColorBrush(
                  D2D1::ColorF(D2D1::ColorF::White), &brush_)) &&
              EnsureImageBitmap();

    if (ok) {
        target_->BeginDraw();
        if (background != nullptr) {
            target_->Clear(*background);
        }
        target_->SetTransform(transform);

        const D2D1_SIZE_F size = image_->GetSize();
        // Nearest neighbour at 1:1 keeps the pixels exact; anywhere the view is
        // zoomed the window's own interpolation setting is what to match.
        const D2D1_BITMAP_INTERPOLATION_MODE interpolation =
            smoothScaling_ ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                           : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        target_->DrawBitmap(image_.Get(),
                            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                            1.0f,
                            interpolate
                                ? interpolation
                                : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

        for (const auto& annotation : document_->Annotations()) {
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke:
                    DrawStroke(annotation.stroke, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Text:
                    DrawText(annotation.text, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Effect:
                    DrawEffect(annotation.effect, annotation.id);
                    break;
                case ccl::doc::AnnotationKind::Area:
                    DrawArea(annotation.area, annotation.id);
                    break;
            }
        }

        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        ok = SUCCEEDED(target_->EndDraw());
    }

    target_ = savedTarget;
    brush_ = savedBrush;
    image_ = savedImage;
    effectCache_ = std::move(savedCache);

    if (!ok || !result.Create(static_cast<int>(width), static_cast<int>(height))) {
        result.Reset();
        return result;
    }

    // Both are 32-bit top-down BGRA, so the rows transfer straight across.
    if (FAILED(surface->CopyPixels(nullptr, result.Stride(),
                                   result.Stride() * height,
                                   static_cast<BYTE*>(result.Pixels())))) {
        result.Reset();
    }
    return result;
}

}  // namespace ccl::render

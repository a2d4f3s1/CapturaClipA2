// Turns the effect class ids into data rather than declarations. Has to come
// before anything that reaches d2d1effects.h -- which Renderer.h does -- or the
// header is already past by the time this is read. Done here and nowhere else:
// a second translation unit doing the same would give the linker two of each.
#include <initguid.h>

#include <d2d1effects.h>

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

// Nine ways for a shadow to fall: 0 to 7 clockwise from straight up, and 8
// straight underneath, where the throw is nothing and only the spread shows.
constexpr float kShadowAcross[] = {0.0f,   0.7071f,  1.0f,  0.7071f, 0.0f,
                                   -0.7071f, -1.0f, -0.7071f, 0.0f};
constexpr float kShadowDown[] = {-1.0f, -0.7071f, 0.0f,  0.7071f, 1.0f,
                                 0.7071f, 0.0f, -0.7071f, 0.0f};

// How far the shadow is thrown, in the text's own coordinates.
D2D1_POINT_2F ShadowThrow(const ccl::doc::TextAnnotation& text) noexcept {
    const int way =
        text.shadowDirection >= 0 &&
                text.shadowDirection < static_cast<int>(ARRAYSIZE(kShadowAcross))
            ? text.shadowDirection
            : 3;
    return D2D1::Point2F(kShadowAcross[way] * text.shadowLength,
                         kShadowDown[way] * text.shadowLength);
}

// How far it spreads: half of how far it is thrown. Deriving it means one
// number settles the whole shadow, and there is no way to ask for a hard edge
// -- which is deliberate, since a hard shadow is a copy of the letters sitting
// beside them rather than something cast by them.
float ShadowSpread(const ccl::doc::TextAnnotation& text) noexcept {
    return text.shadowLength * 0.5f;
}

// The scale a transform carries, however it is composed. Taken from the area
// it multiplies by rather than from one entry, so that the turn applied while
// a rotation is being previewed does not read as a change of size.
float ScaleOf(const D2D1_MATRIX_3X2_F& matrix) noexcept {
    return std::sqrt(
        std::fabs(matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21));
}

}  // namespace

D2D1_RECT_F TurnedBounds(const D2D1_RECT_F& box, float degrees,
                         D2D1_POINT_2F about) noexcept {
    const D2D1_MATRIX_3X2_F turn = D2D1::Matrix3x2F::Rotation(degrees, about);
    const D2D1_POINT_2F corners[4] = {
        D2D1::Point2F(box.left, box.top), D2D1::Point2F(box.right, box.top),
        D2D1::Point2F(box.right, box.bottom),
        D2D1::Point2F(box.left, box.bottom)};

    D2D1_RECT_F turned{};
    for (int i = 0; i < 4; ++i) {
        const D2D1_POINT_2F moved =
            D2D1::Matrix3x2F::ReinterpretBaseType(&turn)->TransformPoint(
                corners[i]);
        if (i == 0) {
            turned = D2D1::RectF(moved.x, moved.y, moved.x, moved.y);
            continue;
        }
        turned.left = (std::min)(turned.left, moved.x);
        turned.top = (std::min)(turned.top, moved.y);
        turned.right = (std::max)(turned.right, moved.x);
        turned.bottom = (std::max)(turned.bottom, moved.y);
    }
    return turned;
}

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

void Renderer::InvalidateShape(unsigned int id) noexcept {
    geometryCache_.erase(id);
}

void Renderer::InvalidateText(unsigned int id) noexcept {
    layoutCache_.erase(id);
    shapeCache_.erase(id);
    bakedCache_.erase(id);
}

void Renderer::InvalidateTextPixels(unsigned int id) noexcept {
    bakedCache_.erase(id);
}

void Renderer::InvalidateResults() noexcept {
    layoutCache_.clear();
    shapeCache_.clear();
    bakedCache_.clear();
    effectCache_.clear();
    // The paths traced by lines and the shapes of painted patches belong here
    // too. They did not while nothing could change one in place -- an
    // annotation was made or unmade, never altered -- but moving and turning
    // do exactly that, and a step back then put the old coordinates in the
    // document while the picture went on being drawn from the new ones.
    geometryCache_.clear();
}

void Renderer::ReportStats() const noexcept {
    ccl::timing::ReportFrames(L"  picture", pictureStats_);
    ccl::timing::ReportFrames(L"  annotations", annotationStats_);
    // TEMPORARY (2026-08-20)
    ccl::timing::ReportFrames(L"  scene rebuild", sceneStats_);
    ccl::timing::ReportFrames(L"  scene annotations", sceneAnnotationStats_);
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

bool Renderer::EnsureScene(const ccl::view::ViewState& view) noexcept {
    if (!target_ || document_ == nullptr || !document_->IsValid()) {
        return false;
    }

    const D2D1_SIZE_F size = target_->GetSize();
    if (size.width <= 0.0f || size.height <= 0.0f) {
        return false;
    }

    const float zoom = view.Zoom();
    const POINT scroll = view.Scroll();
    const unsigned int revision = document_->Revision();

    const bool sameShape = sceneTarget_ && sceneBitmap_ &&
                           sceneSize_.width == size.width &&
                           sceneSize_.height == size.height;
    if (sameShape && sceneRevision_ == revision && sceneZoom_ == zoom &&
        sceneScroll_.x == scroll.x && sceneScroll_.y == scroll.y &&
        sceneBorder_ == border_ && sceneSmooth_ == smoothScaling_) {
        return true;
    }

    if (!sameShape) {
        sceneBitmap_.Reset();
        sceneTarget_.Reset();
        // A surface larger than the device will hand out is refused here rather
        // than failing later; the frame then draws the long way round, which is
        // what it did before any of this.
        if (FAILED(target_->CreateCompatibleRenderTarget(size, &sceneTarget_)) ||
            !sceneTarget_) {
            sceneTarget_.Reset();
            return false;
        }
        sceneSize_ = size;
    }

    if (!EnsureImageBitmap()) {
        return false;
    }

    // TEMPORARY (2026-08-20): what a rebuild costs, and how much of it is the
    // annotations rather than the picture.
    const LONGLONG sceneStart = ccl::timing::Mark();

    // The same transform the frame is about to use, so that what is baked lines
    // up exactly with what gets drawn on top of it.
    const auto inset = static_cast<float>(border_);
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(zoom, zoom) *
        D2D1::Matrix3x2F::Translation(inset - static_cast<float>(scroll.x),
                                      inset - static_cast<float>(scroll.y));

    // The drawing helpers all work through target_, so the scene's takes its
    // place while it is filled. Unlike the off-screen render used for saving,
    // this one is on the same device, so nothing has to be set aside.
    auto saved = target_;
    target_ = sceneTarget_;

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(0.29f, 0.29f, 0.29f));
    target_->SetTransform(transform);

    if (image_) {
        const D2D1_SIZE_F picture = image_->GetSize();
        const D2D1_BITMAP_INTERPOLATION_MODE interpolation =
            smoothScaling_ ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                           : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        target_->DrawBitmap(
            image_.Get(), D2D1::RectF(0.0f, 0.0f, picture.width, picture.height),
            1.0f, interpolation);
    }

    const LONGLONG sceneAnnotationStart = ccl::timing::Mark();
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
    ccl::timing::AddSince(sceneAnnotationStats_, sceneAnnotationStart);

    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT drawn = target_->EndDraw();
    target_ = saved;
    ccl::timing::AddSince(sceneStats_, sceneStart);

    if (FAILED(drawn) ||
        FAILED(sceneTarget_->GetBitmap(&sceneBitmap_)) || !sceneBitmap_) {
        sceneBitmap_.Reset();
        return false;
    }

    sceneRevision_ = revision;
    sceneZoom_ = zoom;
    sceneScroll_ = scroll;
    sceneBorder_ = border_;
    sceneSmooth_ = smoothScaling_;
    return true;
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
    if (shapeCache_.size() > limit) {
        shapeCache_.clear();
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

// Which stretch of the text a piece of the shape came from. Hung on the layout
// while the shape is being built, and handed back on every piece the layout
// produces -- glyphs, underlines and strikethroughs alike.
//
// It carries the position rather than the colour on purpose: the shape is kept
// between frames, and a colour baked into it would have to be thrown away and
// worked out again every time someone recoloured a piece of text.
class RunTag : public IUnknown {
public:
    explicit RunTag(int index) noexcept : index_(index) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) override {
        if (iid == __uuidof(IUnknown)) {
            *obj = this;
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }

    int Index() const noexcept { return index_; }

private:
    int index_ = -1;
    ULONG refs_ = 1;
};

// Walks a laid-out piece of text and collects the shape its glyphs trace, so
// that the shadow, the outline and the text itself can all be drawn from one
// shape instead of laying the glyphs out again for each.
//
// Font fallback has already happened by the time this is called -- each run
// arrives with the face the layout resolved for it -- so a string that reaches
// several faces needs nothing special here.
class OutlineCollector : public IDWriteTextRenderer {
public:
    explicit OutlineCollector(ID2D1Factory* factory) noexcept
        : factory_(factory) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
            iid == __uuidof(IDWriteTextRenderer)) {
            *obj = this;
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,
                                                     BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,
                                                  DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
        *ppd = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT baselineX, FLOAT baselineY, DWRITE_MEASURING_MODE,
        DWRITE_GLYPH_RUN const* run, DWRITE_GLYPH_RUN_DESCRIPTION const*,
        IUnknown* tag) override {
        which_ = IndexOf(tag);
        if (run == nullptr || run->fontFace == nullptr || run->glyphCount == 0) {
            return S_OK;
        }
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory_->CreatePathGeometry(&path))) {
            return S_OK;
        }
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink))) {
            return S_OK;
        }
        const HRESULT hr = run->fontFace->GetGlyphRunOutline(
            run->fontEmSize, run->glyphIndices, run->glyphAdvances,
            run->glyphOffsets, run->glyphCount, run->isSideways,
            (run->bidiLevel & 1) != 0, sink.Get());
        sink->Close();
        if (FAILED(hr)) {
            return S_OK;
        }
        Add(path.Get(), baselineX, baselineY);
        return S_OK;
    }

    // Neither of these is a glyph, so both arrive on their own and would lose
    // their outline if they were left out.
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT baselineX,
                                            FLOAT baselineY,
                                            DWRITE_UNDERLINE const* u,
                                            IUnknown* tag) override {
        which_ = IndexOf(tag);
        AddRect(baselineX, baselineY + u->offset, u->width, u->thickness);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT baselineX,
                                                FLOAT baselineY,
                                                DWRITE_STRIKETHROUGH const* s,
                                                IUnknown* tag) override {
        which_ = IndexOf(tag);
        AddRect(baselineX, baselineY + s->offset, s->width, s->thickness);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT,
                                               IDWriteInlineObject*, BOOL, BOOL,
                                               IUnknown*) override {
        return E_NOTIMPL;
    }

    Microsoft::WRL::ComPtr<ID2D1GeometryGroup> Group() noexcept {
        std::vector<ID2D1Geometry*> raw;
        raw.reserve(parts_.size());
        for (auto& part : parts_) {
            raw.push_back(part.shape.Get());
        }
        Microsoft::WRL::ComPtr<ID2D1GeometryGroup> group;
        if (!raw.empty()) {
            factory_->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(),
                                          static_cast<UINT32>(raw.size()),
                                          &group);
        }
        return group;
    }

    std::vector<TextPart>& Parts() noexcept { return parts_; }

private:
    static int IndexOf(IUnknown* tag) noexcept {
        return tag != nullptr ? static_cast<RunTag*>(tag)->Index() : -1;
    }

    // The outline comes out at the origin, so each piece is moved to where the
    // run actually sits.
    void Add(ID2D1Geometry* shape, FLOAT x, FLOAT y) noexcept {
        Microsoft::WRL::ComPtr<ID2D1TransformedGeometry> moved;
        if (SUCCEEDED(factory_->CreateTransformedGeometry(
                shape, D2D1::Matrix3x2F::Translation(x, y), &moved))) {
            parts_.push_back(TextPart{moved, which_});
        }
    }
    void AddRect(FLOAT x, FLOAT y, FLOAT width, FLOAT thickness) noexcept {
        Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> rect;
        if (SUCCEEDED(factory_->CreateRectangleGeometry(
                D2D1::RectF(x, y, x + width, y + thickness), &rect))) {
            Add(rect.Get(), 0.0f, 0.0f);
        }
    }

    ID2D1Factory* factory_ = nullptr;
    std::vector<TextPart> parts_;
    // The run the piece being handed over belongs to. Set as each piece
    // arrives, since the callbacks take it separately from the shape.
    int which_ = -1;
    ULONG refs_ = 1;
};

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

    const DWRITE_FONT_WEIGHT weight =
        text.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const DWRITE_FONT_STYLE slant =
        text.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    HRESULT made = writer->CreateTextFormat(
        text.fontFamily.c_str(), nullptr, weight, slant,
        DWRITE_FONT_STRETCH_NORMAL, text.fontSize, L"", &format);

    if (FAILED(made)) {
        // Nothing drawn is the worst thing that can happen to a piece of text:
        // the window is the only place that picture exists, and a piece that
        // silently disappears takes what it said with it. So a second attempt
        // is made with values that cannot be refused -- a size DirectWrite will
        // accept, and the face used when none is asked for.
        //
        // Measured (2026-08-21): of the 365 names the menu offers, the 693 the
        // settings offer, and names that are not installed at all, none is
        // refused. A size of zero or less is refused. That is the way in.
        const float size =
            text.fontSize > 0.0f ? (std::min)(text.fontSize, 400.0f) : 30.0f;
        made = writer->CreateTextFormat(L"", nullptr, weight, slant,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                        &format);
        if (ccl::timing::g_enabled) {
            // Drawing something other than what was asked for is a repair, and
            // a repair that leaves no trace is indistinguishable from the bug
            // it covers. This is where to look when a piece of text comes out
            // in the wrong face.
            wchar_t line[192];
            ::swprintf_s(line,
                         L"[timing] text fallback            size %g -> %g  "
                         L"face '%s' -> default  %s\n",
                         text.fontSize, size, text.fontFamily.c_str(),
                         SUCCEEDED(made) ? L"recovered" : L"still refused");
            ccl::timing::Write(line);
        }
    }

    if (FAILED(made)) {
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
    elsewhere_ = true;

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
    elsewhere_ = false;

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

bool Renderer::MeasureLines(const ccl::doc::TextAnnotation& text,
                            std::vector<float>& heights) noexcept {
    heights.clear();
    if (context_ == nullptr) {
        return false;
    }

    ccl::doc::TextAnnotation probe = text;
    // An empty box still sits on a line, and that line still has a height.
    if (probe.text.empty()) {
        probe.text = L"A";
        probe.runs.clear();
    }

    const auto layout = BuildLayout(context_->Text(), probe);
    if (!layout) {
        return false;
    }

    UINT32 count = 0;
    // Asked for nothing first, which reports how many there are.
    layout->GetLineMetrics(nullptr, 0, &count);
    if (count == 0) {
        return false;
    }

    std::vector<DWRITE_LINE_METRICS> lines(count);
    if (FAILED(layout->GetLineMetrics(lines.data(), count, &count))) {
        return false;
    }

    heights.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        heights.push_back(lines[i].height);
    }
    return true;
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

const TextShape* Renderer::TextShapeFor(const ccl::doc::TextAnnotation& text,
                                        unsigned int id) noexcept {
    if (id != 0) {
        const auto cached = shapeCache_.find(id);
        if (cached != shapeCache_.end()) {
            return &cached->second;
        }
    }
    IDWriteTextLayout* layout = TextLayout(text, id);
    if (layout == nullptr || context_ == nullptr) {
        return nullptr;
    }

    // Each run is labelled before the walk, so that every piece the layout
    // produces comes back knowing which run it belongs to and can be given
    // that run's colour when it is drawn.
    std::vector<Microsoft::WRL::ComPtr<RunTag>> tags;
    tags.reserve(text.runs.size());
    for (size_t i = 0; i < text.runs.size(); ++i) {
        const ccl::doc::TextRun& run = text.runs[i];
        tags.emplace_back(new RunTag(static_cast<int>(i)));
        tags.back()->Release();  // the ComPtr took its own reference
        layout->SetDrawingEffect(tags.back().Get(),
                                 DWRITE_TEXT_RANGE{run.start, run.length});
    }

    const LONGLONG start = ccl::timing::Mark();
    OutlineCollector* collector = new OutlineCollector(context_->Factory());
    layout->Draw(nullptr, collector, 0.0f, 0.0f);

    TextShape shape;
    shape.group = collector->Group();
    shape.parts = std::move(collector->Parts());
    if (ccl::timing::g_enabled) {
        wchar_t line[128];
        ::swprintf_s(line, L"[timing] %-28s %8.2f ms  %zu chars, %zu parts\n",
                     L"  text shape",
                     ccl::timing::MillisecondsSince(start), text.text.size(),
                     shape.parts.size());
        ccl::timing::Write(line);
    }
    collector->Release();
    if (!shape.group) {
        return nullptr;
    }
    if (id == 0) {
        transientShape_ = std::move(shape);
        return &transientShape_;
    }
    return &(shapeCache_[id] = std::move(shape));
}

void Renderer::BakeTexts(float zoom) noexcept {
    if (document_ == nullptr || !target_ || zoom <= 0.0f) {
        return;
    }
    // While the zoom is still moving, what is already held is stretched to fit
    // instead of being drawn again: a piece of text would otherwise be redrawn
    // on every notch of the wheel. Anything with nothing held yet is drawn the
    // long way by the frame, as it is when a bitmap would be too large.
    const bool settled = zoom == lastZoom_;
    lastZoom_ = zoom;
    if (!settled) {
        return;
    }
    for (const auto& annotation : document_->Annotations()) {
        if (annotation.kind != ccl::doc::AnnotationKind::Text ||
            annotation.text.text.empty()) {
            continue;
        }
        const auto found = bakedCache_.find(annotation.id);
        if (found != bakedCache_.end() && found->second.scale == zoom &&
            found->second.angle == annotation.text.angle) {
            continue;
        }

        D2D1_RECT_F bounds{};
        if (!MeasureText(annotation.text, bounds)) {
            continue;
        }
        // Room for whatever stands out past the glyphs: the edge goes round
        // them, the shadow falls to one side of them and spreads past where it
        // lands, and a rounded join adds a little more at the corners.
        const float reach =
            annotation.text.shadow
                ? annotation.text.shadowLength +
                      ShadowSpread(annotation.text) * 3.0f
                : 0.0f;
        const float spread =
            (std::max)(annotation.text.outline ? annotation.text.outlineWidth
                                               : 0.0f,
                       reach) +
            2.0f;
        bounds.left -= spread;
        bounds.top -= spread;
        bounds.right += spread;
        bounds.bottom += spread;

        // A turned piece needs a sheet big enough for where its corners land.
        // Turned about its own corner, so that is the point the corners are
        // swung around.
        if (annotation.text.angle != 0.0f) {
            bounds = TurnedBounds(bounds, annotation.text.angle,
                                  D2D1::Point2F(annotation.text.x,
                                                annotation.text.y));
        }

        const float wide = (bounds.right - bounds.left) * zoom;
        const float tall = (bounds.bottom - bounds.top) * zoom;
        // Past what a surface can be, the frame draws it the long way instead.
        if (wide <= 0.0f || tall <= 0.0f || wide > 16000.0f || tall > 16000.0f) {
            bakedCache_.erase(annotation.id);
            continue;
        }

        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> sheet;
        if (FAILED(target_->CreateCompatibleRenderTarget(
                D2D1::SizeF(wide, tall), &sheet)) ||
            !sheet) {
            continue;
        }

        auto savedTarget = target_;
        auto savedBrush = brush_;
        target_ = sheet;
        brush_.Reset();
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White), &brush_))) {
            baking_ = true;
            target_->BeginDraw();
            target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            target_->SetTransform(
                D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top) *
                D2D1::Matrix3x2F::Scale(zoom, zoom));
            DrawText(annotation.text, annotation.id);
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
            target_->EndDraw();
            baking_ = false;
        }
        target_ = savedTarget;
        brush_ = savedBrush;

        BakedText baked;
        // Held against the text's own position, so that moving it moves the
        // blit with it rather than leaving it where it was drawn.
        baked.offset = D2D1::RectF(bounds.left - annotation.text.x,
                                   bounds.top - annotation.text.y,
                                   bounds.right - annotation.text.x,
                                   bounds.bottom - annotation.text.y);
        baked.scale = zoom;
        baked.angle = annotation.text.angle;
        if (SUCCEEDED(sheet->GetBitmap(&baked.bitmap)) && baked.bitmap) {
            bakedCache_[annotation.id] = std::move(baked);
        }
    }
}

void Renderer::DrawTextShadow(ID2D1Geometry* shape,
                              const ccl::doc::TextAnnotation& text,
                              const D2D1_MATRIX_3X2_F& placed) noexcept {
    if (shape == nullptr || !target_ || !brush_) {
        return;
    }

    const D2D1_POINT_2F throw_ = ShadowThrow(text);
    // The throw goes into the transform rather than being added to the result,
    // so that it turns and scales with everything else: a shadow cast down-
    // right stays down-right of the letters at any zoom, and follows them
    // round while a rotation is being previewed.
    const D2D1_MATRIX_3X2_F thrown =
        D2D1::Matrix3x2F::Translation(throw_.x, throw_.y) * placed;

    // What to fall back on. A shadow with a hard edge is not what was asked
    // for, but it beats a piece of text that suddenly has none.
    const auto plain = [&]() {
        brush_->SetColor(ToD2D(text.shadowColor));
        target_->SetTransform(thrown);
        target_->FillGeometry(shape, brush_.Get());
    };

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(target_.As(&context)) || !context) {
        plain();
        return;
    }

    // Both the throw and the spread are whole pixels at 100%, so the spread
    // has to be taken up to the pixels actually being drawn to.
    const float spread = ShadowSpread(text) * ScaleOf(placed);
    if (spread <= 0.0f) {
        plain();
        return;
    }

    D2D1_RECT_F bounds{};
    if (FAILED(shape->GetBounds(thrown, &bounds))) {
        plain();
        return;
    }
    // Three deviations out, a Gaussian has nothing left worth keeping.
    const float margin = spread * 3.0f + 2.0f;
    bounds.left -= margin;
    bounds.top -= margin;
    bounds.right += margin;
    bounds.bottom += margin;

    const float wide = bounds.right - bounds.left;
    const float tall = bounds.bottom - bounds.top;
    if (wide <= 0.0f || tall <= 0.0f) {
        plain();
        return;
    }

    // Past what a surface can be -- a long line seen at a high zoom -- the
    // shadow alone is drawn smaller and stretched back up. Only the shadow can
    // take that: what stretching costs it is sharpness, and sharpness is the
    // one thing it does not have. Measured at 15239x173, shrinking by half put
    // the result 1.4 of 255 away from the full-sized one on average.
    constexpr float kLargest = 16000.0f;
    float shrink = 1.0f;
    if (wide > kLargest || tall > kLargest) {
        shrink = (std::min)(kLargest / wide, kLargest / tall);
    }

    // A surface of its own, because the spread has to reach past the letters
    // without taking the letters with it.
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> sheet;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
    if (FAILED(target_->CreateCompatibleRenderTarget(
            D2D1::SizeF(wide * shrink, tall * shrink), &sheet)) ||
        !sheet ||
        FAILED(sheet->CreateSolidColorBrush(ToD2D(text.shadowColor), &ink))) {
        plain();
        return;
    }

    sheet->BeginDraw();
    sheet->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    sheet->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    sheet->SetTransform(
        thrown * D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top) *
        D2D1::Matrix3x2F::Scale(shrink, shrink));
    sheet->FillGeometry(shape, ink.Get());
    sheet->SetTransform(D2D1::Matrix3x2F::Identity());
    Microsoft::WRL::ComPtr<ID2D1Bitmap> filled;
    if (FAILED(sheet->EndDraw()) || FAILED(sheet->GetBitmap(&filled)) ||
        !filled) {
        plain();
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1Effect> blur;
    if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) || !blur) {
        plain();
        return;
    }
    blur->SetInput(0, filled.Get());
    // The spread is shrunk with the surface, so the result comes out the same
    // width once it is stretched back.
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, spread * shrink);

    ID2D1Effect* drawn = blur.Get();
    Microsoft::WRL::ComPtr<ID2D1Effect> stretch;
    if (shrink < 1.0f) {
        if (FAILED(context->CreateEffect(CLSID_D2D1Scale, &stretch)) ||
            !stretch) {
            plain();
            return;
        }
        stretch->SetInputEffect(0, blur.Get());
        stretch->SetValue(D2D1_SCALE_PROP_SCALE,
                          D2D1::Vector2F(1.0f / shrink, 1.0f / shrink));
        stretch->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                          D2D1_SCALE_INTERPOLATION_MODE_LINEAR);
        drawn = stretch.Get();
    }

    // The surface was filled in the pixels being drawn to, so it goes back
    // with no transform of its own, at the corner it was measured from.
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->DrawImage(drawn, D2D1::Point2F(bounds.left, bounds.top));
}

void Renderer::DrawText(const ccl::doc::TextAnnotation& text,
                        unsigned int id) noexcept {
    if (text.text.empty() || !brush_ || context_ == nullptr) {
        return;
    }

    // A sheet already prepared stands in for the whole of the work below: one
    // blit, at the size it was drawn for. Skipped while a sheet is being
    // filled, and on the off-screen targets, whose device is not the one that
    // holds it.
    if (!baking_ && !elsewhere_ && id != 0) {
        const auto baked = bakedCache_.find(id);
        if (baked != bakedCache_.end() && baked->second.bitmap) {
            const D2D1_RECT_F& from = baked->second.offset;
            target_->DrawBitmap(
                baked->second.bitmap.Get(),
                D2D1::RectF(text.x + from.left, text.y + from.top,
                            text.x + from.right, text.y + from.bottom));
            return;
        }
    }

    const TextShape* shape = TextShapeFor(text, id);
    if (shape == nullptr || !shape->group) {
        return;
    }

    // Letters are never drawn hard-edged. The setting that hardens the brush is
    // about lines and fills; applying it here would only make text worse.
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // The shape is in the text's own coordinates, so where the text sits is
    // composed onto whatever the frame already put on the target -- the zoom
    // and scroll, or a turn being previewed.
    D2D1_MATRIX_3X2_F scene{};
    target_->GetTransform(&scene);
    // Turned first, about the text's own corner, and then carried to where the
    // corner sits. Composed this way round the shadow's throw is turned with
    // the letters, which is what a decoration attached to them should do.
    const D2D1_MATRIX_3X2_F placed =
        (text.angle == 0.0f
             ? D2D1::Matrix3x2F::Identity()
             : D2D1::Matrix3x2F::Rotation(text.angle,
                                          D2D1::Point2F(0.0f, 0.0f))) *
        D2D1::Matrix3x2F::Translation(text.x, text.y) * scene;
    target_->SetTransform(placed);

    // Screenshots are busy backgrounds, so the shadow and the outline are there
    // to keep text readable rather than to decorate it.
    if (text.shadow && text.shadowColor.a > 0.0f) {
        DrawTextShadow(shape->group.Get(), text, placed);
        target_->SetTransform(placed);
    }

    if (text.outline && text.outlineWidth > 0.0f) {
        // A stroke straddles the line it follows, so twice the width asked for
        // goes down and the letters drawn next cover the half that fell inside.
        //
        // Rounded joins are not decoration: under a mitre a sharp corner -- the
        // arms of a reference mark, for one -- throws a spike far past the
        // letter it belongs to.
        if (!outlineStyle_) {
            context_->Factory()->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                    D2D1_DASH_STYLE_SOLID, 0.0f),
                nullptr, 0, &outlineStyle_);
        }
        brush_->SetColor(ToD2D(text.outlineColor));
        target_->DrawGeometry(shape->group.Get(), brush_.Get(),
                              text.outlineWidth * 2.0f, outlineStyle_.Get());
    }

    // The letters themselves, a part at a time so that a stretch given a colour
    // of its own comes out in it. The colour is looked up here rather than kept
    // in the shape: recolouring then costs nothing but the next frame, where
    // baking it in would mean tracing every glyph again.
    for (const TextPart& part : shape->parts) {
        const ccl::doc::Color& colour =
            part.run >= 0 && part.run < static_cast<int>(text.runs.size())
                ? text.runs[part.run].color
                : text.color;
        brush_->SetColor(ToD2D(colour));
        target_->FillGeometry(part.shape.Get(), brush_.Get());
    }

    target_->SetTransform(scene);
}

void Renderer::Draw(const ccl::view::ViewState& view,
                    const ccl::doc::Stroke* active, const BrushCursor* cursor,
                    const D2D1_RECT_F* highlight, ID2D1Geometry* selection,
                    ID2D1Geometry* removing, const D2D1_COLOR_F* selectionColor,
                    const unsigned int* pickedIds, size_t pickedCount,
                    float grabSlack) noexcept {
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

    // The same goes for the scene, which is filled through a target of its own.
    // Skipped while a turn is being previewed: the transform changes on every
    // frame then, so there would be nothing to reuse.
    // TEMPORARY (2026-08-20): the same goes for baking the text -- a target of
    // its own, so before the frame opens and before the scene that uses it.
    BakeTexts(view.Zoom());

    const bool previewing = previewRotation_ != 0.0f;
    const bool scened = !previewing && EnsureScene(view);

    target_->BeginDraw();

    // Clearing to the outline colour and insetting the content by the border
    // width leaves exactly a one pixel frame around the capture.
    //
    // While a turn is being previewed the padding colour takes over, so that
    // what shows around the picture is what committing would fill in.
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
    if (scened) {
        // One blit stands in for the picture and every settled annotation on
        // it. Drawn untransformed and at its own size, so nothing is resampled.
        const D2D1_SIZE_F surface = target_->GetSize();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        target_->DrawBitmap(
            sceneBitmap_.Get(),
            D2D1::RectF(0.0f, 0.0f, surface.width, surface.height));
        target_->SetTransform(transform);
    } else if (image_) {
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
    if (!scened && document_ != nullptr) {
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
            // The area tools' blue unless the caller names another, which is
            // what tells the two kinds of selecting apart.
            brush_->SetColor(selectionColor != nullptr
                                 ? *selectionColor
                                 : D2D1::ColorF(0.35f, 0.65f, 1.0f, 0.9f));
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

    if (pickedIds != nullptr && pickedCount > 0 && brush_ &&
        document_ != nullptr) {
        // Dark outside and light inside, so the mark reads whatever it happens
        // to be sitting on -- the same reasoning as the brush ring. The hover
        // outline is a single blue line, which keeps the two apart: one says
        // what a press would take hold of, this says what is already held.
        //
        // A band along the piece rather than a box round it. A box round a
        // diagonal line covers a square the size of the line's reach in both
        // directions, and says a press anywhere inside would take hold, which
        // has not been true since the reach became the ink plus a little.
        const float lineWidth = 1.0f / zoom;
        const float slack = grabSlack / zoom;

        const auto holds = [pickedIds, pickedCount](unsigned int id) {
            for (size_t i = 0; i < pickedCount; ++i) {
                if (pickedIds[i] == id) {
                    return true;
                }
            }
            return false;
        };
        // Both bands at once, then the piece itself back over the top. The
        // band is wider than the ink, so drawing it over would bury what is
        // being marked; drawing it under is not open to us either, since
        // settled annotations are baked into a sheet that is not remade when
        // the picked set changes.
        // The same round caps and joins the ink is drawn with. Without them the
        // band ends in a flat slab standing out past the tip of the line.
        const auto band = [&](ID2D1Geometry* path, float width) {
            if (path == nullptr) {
                return;
            }
            brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f));
            target_->DrawGeometry(path, brush_.Get(), width,
                                  strokeStyle_.Get());
            brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
            target_->DrawGeometry(path, brush_.Get(),
                                  (std::max)(width - 2.0f * lineWidth,
                                             lineWidth),
                                  strokeStyle_.Get());
        };
        // A stroke that tapers is drawn segment by segment, each at its own
        // width, so its band has to be too: one drawn at the widest the line
        // ever gets leaves a slab lying along the thin end. The width here is
        // the one the hit test uses -- the wider of the segment's two ends --
        // so the edge of the band is exactly as far as a press reaches.
        //
        // Every dark segment before any light one: taken in turn, each dark
        // segment would paint over the light one before it.
        const auto taperedBand = [&](const ccl::doc::Stroke& stroke) {
            for (int pass = 0; pass < 2; ++pass) {
                brush_->SetColor(pass == 0
                                     ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f)
                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
                for (size_t i = 1; i < stroke.points.size(); ++i) {
                    const auto& from = stroke.points[i - 1];
                    const auto& to = stroke.points[i];
                    const float widest = (std::max)(from.width, to.width);
                    const float width =
                        pass == 0
                            ? widest + 2.0f * slack
                            : (std::max)(widest + 2.0f * slack -
                                             2.0f * lineWidth,
                                         lineWidth);
                    target_->DrawLine(D2D1::Point2F(from.x, from.y),
                                      D2D1::Point2F(to.x, to.y), brush_.Get(),
                                      width, strokeStyle_.Get());
                }
            }
        };

        for (const auto& annotation : document_->Annotations()) {
            if (!holds(annotation.id)) {
                continue;
            }
            switch (annotation.kind) {
                case ccl::doc::AnnotationKind::Stroke: {
                    float widest = 0.0f;
                    float narrowest = FLT_MAX;
                    for (const auto& point : annotation.stroke.points) {
                        widest = (std::max)(widest, point.width);
                        narrowest = (std::min)(narrowest, point.width);
                    }
                    if (widest > narrowest) {
                        taperedBand(annotation.stroke);
                    } else {
                        // One width all along, so one pass over the path does
                        // it -- much the cheaper of the two.
                        band(StrokeGeometry(annotation.stroke, annotation.id),
                             widest + 2.0f * slack);
                    }
                    DrawStroke(annotation.stroke, annotation.id);
                    break;
                }
                case ccl::doc::AnnotationKind::Area: {
                    band(AreaGeometry(annotation.area, annotation.id),
                         annotation.area.width + 2.0f * slack);
                    DrawArea(annotation.area, annotation.id);
                    break;
                }
                case ccl::doc::AnnotationKind::Text: {
                    // Text keeps a box: the reach round a piece of text is its
                    // own box already, so a box tells the truth here. A turned
                    // one, though -- an upright box round a turned one stands
                    // well outside the reach at the corners.
                    D2D1_RECT_F box{};
                    if (!MeasureText(annotation.text, box)) {
                        break;
                    }
                    box.left -= slack;
                    box.top -= slack;
                    box.right += slack;
                    box.bottom += slack;
                    const D2D1_MATRIX_3X2_F was = transform;
                    if (annotation.text.angle != 0.0f) {
                        target_->SetTransform(
                            D2D1::Matrix3x2F::Rotation(
                                annotation.text.angle,
                                D2D1::Point2F(annotation.text.x,
                                              annotation.text.y)) *
                            was);
                    }
                    brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f));
                    target_->DrawRectangle(
                        D2D1::RectF(box.left - lineWidth, box.top - lineWidth,
                                    box.right + lineWidth,
                                    box.bottom + lineWidth),
                        brush_.Get(), lineWidth);
                    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
                    target_->DrawRectangle(box, brush_.Get(), lineWidth);
                    if (annotation.text.angle != 0.0f) {
                        target_->SetTransform(was);
                    }
                    break;
                }
                case ccl::doc::AnnotationKind::Effect:
                    // Never picked, so never marked.
                    break;
            }
        }
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
    elsewhere_ = true;

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
    elsewhere_ = false;

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

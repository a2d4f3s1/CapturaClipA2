#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <unordered_map>
#include <vector>

#include "capture/DibBuffer.h"
#include "doc/Annotation.h"
#include "util/Timing.h"
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
    void Attach(D2DContext& context, HWND hwnd) noexcept;
    void SetDocument(const ccl::doc::Document* document) noexcept;

    void Resize(UINT width, UINT height) noexcept;

    // Drops the cached pixels for an effect whose strength has changed.
    void InvalidateEffect(unsigned int id) noexcept;

    // Drops the laid-out glyphs of a piece of text whose size or font has
    // changed. Only needed for a change made in place: re-editing produces a
    // new annotation with an id of its own, which has no cache entry yet.
    void InvalidateText(unsigned int id) noexcept;

    // Where the time in a frame went, split so that rebuilding shapes on the
    // CPU can be told apart from the cost of putting pixels on the screen.
    // Written to the timing log when the window closes.
    void ReportStats() const noexcept;

    // `active` is the stroke currently being drawn, which is not yet part of
    // the document. `cursor` draws the brush size outline when set.
    // `highlight` outlines an annotation, marking what a click would act on.
    // `selection` outlines the selected area. `removing` outlines an area
    // being taken back out of it, drawn in a colour of its own: what is being
    // dragged out is about to disappear from the selection, so an outline the
    // same colour as the rest would say the opposite of what is happening.
    //
    // These are separate parameters rather than one "outline this", because
    // they answer different questions -- what a click would pick up, what an
    // area operation would act on, what is about to stop being selected -- and
    // any of them can be true at once.
    void Draw(const ccl::view::ViewState& view,
              const ccl::doc::Stroke* active = nullptr,
              const BrushCursor* cursor = nullptr,
              const D2D1_RECT_F* highlight = nullptr,
              ID2D1Geometry* selection = nullptr,
              ID2D1Geometry* removing = nullptr) noexcept;

    // Draws the picture and its annotations into a new buffer at full size,
    // producing the image as it is actually seen. Everything that leaves the
    // program -- saving, copying, printing -- goes through this, and so does
    // any operation that reshapes the picture, since a rotated or cropped
    // image and annotations placed against the old one cannot both be kept.
    ccl::capture::DibBuffer Flatten() noexcept;

    // The window's contents as they appear: zoom applied, scrolled to where it
    // is. The size given is the area inside the border, which is excluded --
    // otherwise the picture would grow by the border on every use. What the
    // tools draw on top -- the brush ring, the highlight around text -- is left
    // out as well, since none of it is part of the picture.
    ccl::capture::DibBuffer CaptureView(
        UINT width, UINT height, const ccl::view::ViewState& view) noexcept;

    // The picture and its annotations turned by `degrees`, on a surface grown
    // to fit the corners. What the turn leaves uncovered is filled with `fill`.
    //
    // `scale` shrinks the whole result, which is what makes a preview cheap:
    // the same call at a small scale while the angle is being chosen, and at
    // 1.0 once it is settled. Turning always resamples, so the picture is only
    // ever built from the original -- previewing costs it nothing.
    ccl::capture::DibBuffer RenderRotated(float degrees,
                                          const ccl::doc::Color& fill,
                                          float scale) noexcept;

    // Bounding box of a piece of text in image coordinates, used to work out
    // which one was clicked. Returns false if it could not be measured.
    bool MeasureText(const ccl::doc::TextAnnotation& text,
                     D2D1_RECT_F& bounds) noexcept;

    // Height of one line and the distance from its top to the baseline, in
    // image pixels. The editor is told to use these so that typed and drawn
    // text sit on the same lines.
    bool MeasureLine(const ccl::doc::TextAnnotation& text, float& lineHeight,
                     float& baseline) noexcept;

    // Turns what is on screen without touching the picture, so an angle can be
    // judged against the real thing at its real size rather than a thumbnail.
    // The zoom is left alone: what falls outside the window is simply not seen,
    // and committing grows the picture to fit its corners, so nothing is lost.
    //
    // `fill` is shown wherever the turn leaves the picture, so that the padding
    // being previewed is the padding that will be applied. Zero degrees puts
    // everything back.
    void SetPreviewRotation(float degrees, const ccl::doc::Color& fill) noexcept {
        previewRotation_ = degrees;
        previewFill_ = fill;
    }

    // Smooth interpolation looks better for photographs and text, nearest
    // neighbour is what you want when inspecting individual pixels.
    // The shape of the arrowhead a line can be given, all measured against the
    // head's own width so that the brush size carries them along.
    void SetArrowShape(float scale, float aspect, float rounding) noexcept {
        arrowScale_ = scale;
        arrowAspect_ = aspect;
        arrowRounding_ = rounding;
    }

    void SetSmoothScaling(bool smooth) noexcept { smoothScaling_ = smooth; }
    bool SmoothScaling() const noexcept { return smoothScaling_; }

    // Width of the outline drawn around the picture. Zero for the window styles
    // that have a frame of their own, or none at all.
    void SetBorderWidth(int width) noexcept { border_ = width; }


private:
    bool EnsureTarget() noexcept;
    bool EnsureImageBitmap() noexcept;
    // Draws the document into a buffer of the given size through `transform`,
    // on a target that can be read back on the CPU.
    // `background` fills the surface before anything is drawn; without one,
    // whatever the picture does not cover is left as it started.
    //
    // `interpolate` is separate on purpose. Clearing and smoothing sound like
    // they go together -- and did, while the only callers were saving at 1:1
    // and capturing a zoomed view -- but a turn needs smoothing over a filled
    // background, and saving needs neither. Tying them to one flag hides that.
    ccl::capture::DibBuffer RenderOffscreen(UINT width, UINT height,
                                            const D2D1_MATRIX_3X2_F& transform,
                                            const D2D1_COLOR_F* background,
                                            bool interpolate) noexcept;
    // Draws the annotations below `limit` onto a piece of the picture that was
    // cropped out at (left, top). Works on the piece alone, so what it costs
    // follows the size of the area rather than the size of the capture.
    bool OverlayAnnotations(ccl::capture::DibBuffer& region, int left, int top,
                            size_t limit) noexcept;
    void DiscardDeviceResources() noexcept;

    // `id` is the annotation the shape belongs to, which is what the worked-out
    // result is kept against. Zero for the stroke still being drawn: it grows
    // with every mouse message, so there is nothing about it worth keeping.
    void DrawStroke(const ccl::doc::Stroke& stroke, unsigned int id) noexcept;
    // The stroke itself, without the highlighter's layer around it.
    void DrawStrokeShape(const ccl::doc::Stroke& stroke,
                         unsigned int id) noexcept;
    // Just the line. Kept apart from the above because it has three ways of
    // being drawn -- a dot, a run of segments each at its own width, or one
    // path -- and each leaves as soon as it is done, so anything added after
    // the last of them would only ever run for that one case.
    void DrawStrokeLine(const ccl::doc::Stroke& stroke,
                        unsigned int id) noexcept;
    // The arrowheads placed along it, drawn after the line so they sit on top
    // of it, and inside the highlighter's layer so a head that overlaps the
    // line does not come out twice as dark.
    void DrawStrokeArrows(const ccl::doc::Stroke& stroke) noexcept;
    void DrawVariableStroke(const ccl::doc::Stroke& stroke) noexcept;
    // The path a stroke traces, built once and kept. Null for a stroke whose
    // width varies, which Direct2D cannot express as a single path.
    ID2D1Geometry* StrokeGeometry(const ccl::doc::Stroke& stroke,
                                  unsigned int id) noexcept;
    void DrawArea(const ccl::doc::AreaAnnotation& area,
                  unsigned int id) noexcept;
    // The shape an area covers, folded from its pieces once and kept. It is
    // never reshaped after it is placed, so the result stays good for as long
    // as its id does.
    ID2D1Geometry* AreaGeometry(const ccl::doc::AreaAnnotation& area,
                                unsigned int id) noexcept;
    void DrawText(const ccl::doc::TextAnnotation& text, unsigned int id) noexcept;
    // The laid-out glyphs of a piece of text. Text is never edited in place --
    // re-editing replaces the annotation -- so a layout stays good for as long
    // as its id does.
    IDWriteTextLayout* TextLayout(const ccl::doc::TextAnnotation& text,
                                  unsigned int id) noexcept;
    void DrawEffect(const ccl::doc::EffectAnnotation& effect,
                    unsigned int id) noexcept;
    // Takes a copy of what lies under each effect that has not got one yet:
    // the picture with everything drawn below the effect, so that a stroke it
    // covers is obscured along with the picture rather than wiped out. Runs
    // before the frame starts, because Direct2D cannot be asked to draw
    // somewhere else in the middle of drawing here.
    void CaptureEffectSources() noexcept;
    // Rasterises the shape an effect hides at the size of its box: one byte a
    // pixel, full inside and nothing outside. Empty for an effect that covers
    // the whole of its box.
    std::vector<unsigned char> BuildEffectMask(
        const ccl::doc::EffectAnnotation& effect) noexcept;
    // Processes those pixels once and keeps the result; what an effect
    // obscures is settled when it is placed and does not change after.
    ID2D1Bitmap* EffectBitmap(const ccl::doc::EffectAnnotation& effect,
                              unsigned int id) noexcept;
    // Throws away cached results for annotations that are no longer there.
    void PruneCaches() noexcept;

    D2DContext* context_ = nullptr;
    HWND hwnd_ = nullptr;
    const ccl::doc::Document* document_ = nullptr;

    // Drawing goes through target_, which is normally the window's target but
    // is swapped for an off-screen one while flattening.
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> windowTarget_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> image_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle_;
    std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<ID2D1Bitmap>>
        effectCache_;
    // What was under each effect when it was placed. Kept separately from the
    // processed result above, so that turning the strength up and down only
    // costs the processing and not another look at the picture.
    std::unordered_map<unsigned int, ccl::capture::DibBuffer> effectSource_;
    // The shape each effect hides within its box, a byte a pixel. Kept apart
    // from the processed pixels for the same reason as the sources: the shape
    // does not change when the strength does, and working it out again on
    // every press of the size keys would be paid for nothing.
    std::unordered_map<unsigned int, std::vector<unsigned char>> effectMask_;
    // Worked-out shapes, whatever produced them: the path a stroke traces, the
    // area a fill covers. Ids are handed out once and never reused, so the two
    // can share the one map.
    std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<ID2D1Geometry>>
        geometryCache_;
    std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<IDWriteTextLayout>>
        layoutCache_;
    // The stroke still being drawn changes shape with every mouse message, so
    // its path is rebuilt each frame. Held here only so it outlives the call
    // that draws with it.
    Microsoft::WRL::ComPtr<ID2D1Geometry> transientGeometry_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> transientLayout_;
    // One layer, reused by every highlighter stroke. Each PushLayer wants an
    // intermediate surface, and asking for a new one per stroke per frame is
    // the one cost here that grows with the size of the capture.
    Microsoft::WRL::ComPtr<ID2D1Layer> highlightLayer_;

    // Non-zero only while a rotation is being previewed.
    float previewRotation_ = 0.0f;
    ccl::doc::Color previewFill_{};

    int border_ = kWindowBorder;
    // Seeded from the settings file; the defaults here only stand until it is
    // read.
    float arrowScale_ = 3.0f;
    float arrowAspect_ = 1.2f;
    float arrowRounding_ = 0.15f;
    bool smoothScaling_ = true;
    bool measuredFirstDraw_ = false;

    // Direct2D queues drawing and does the work at EndDraw, so these do not
    // split the frame into "time spent on each thing". They split it into
    // "time this process spent building shapes" and "time everything else
    // took", which is the distinction that says whether caching would help.
    ccl::timing::FrameStats pictureStats_;
    ccl::timing::FrameStats annotationStats_;
    // Carrying out the queued drawing, and putting the finished frame on the
    // screen. Split because the two want opposite answers: less to draw, or a
    // different way of handing it over.
    ccl::timing::FrameStats rasterStats_;
    ccl::timing::FrameStats presentStats_;
};

}  // namespace ccl::render

#include "render/SelectionGeometry.h"

#include <algorithm>

namespace ccl::render {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID2D1Geometry> ShapeGeometry(ID2D1Factory* factory,
                                    const ccl::doc::SelectionShape& shape) noexcept {
    ComPtr<ID2D1Geometry> result;

    if (!shape.lasso) {
        // Taken in whichever order it was dragged out.
        const D2D1_RECT_F area = D2D1::RectF(
            (std::min)(shape.left, shape.right), (std::min)(shape.top, shape.bottom),
            (std::max)(shape.left, shape.right), (std::max)(shape.top, shape.bottom));
        if (area.right <= area.left || area.bottom <= area.top) {
            return result;
        }
        ComPtr<ID2D1RectangleGeometry> rectangle;
        if (SUCCEEDED(factory->CreateRectangleGeometry(area, &rectangle))) {
            result = rectangle;
        }
        return result;
    }

    // Fewer than three points enclose nothing at all.
    if (shape.points.size() < 3) {
        return result;
    }

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) {
        return result;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return result;
    }

    sink->BeginFigure(D2D1::Point2F(shape.points[0].x, shape.points[0].y),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < shape.points.size(); ++i) {
        sink->AddLine(D2D1::Point2F(shape.points[i].x, shape.points[i].y));
    }
    // Closed regardless of where the drag ended: an area has to be enclosed to
    // be an area, and asking for the last stretch to be drawn by hand would be
    // asking for something no one manages.
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) {
        return result;
    }

    result = path;
    return result;
}

ComPtr<ID2D1Geometry> Combine(ID2D1Factory* factory, ID2D1Geometry* left,
                              ID2D1Geometry* right,
                              D2D1_COMBINE_MODE mode) noexcept {
    ComPtr<ID2D1Geometry> result;

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) {
        return result;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return result;
    }
    const HRESULT hr =
        left->CombineWithGeometry(right, mode, nullptr, sink.Get());
    if (FAILED(sink->Close()) || FAILED(hr)) {
        return result;
    }

    result = path;
    return result;
}

}  // namespace

ComPtr<ID2D1Geometry> BuildSelectionGeometry(
    ID2D1Factory* factory, const ccl::doc::SelectionShapes& shapes) noexcept {
    ComPtr<ID2D1Geometry> result;
    if (factory == nullptr) {
        return result;
    }

    for (const ccl::doc::SelectionShape& shape : shapes) {
        const ComPtr<ID2D1Geometry> piece = ShapeGeometry(factory, shape);

        switch (shape.op) {
            case ccl::doc::SelectionOp::Replace:
                result = piece;
                break;

            case ccl::doc::SelectionOp::Add:
                if (!piece) {
                    break;
                }
                result = result ? Combine(factory, result.Get(), piece.Get(),
                                          D2D1_COMBINE_MODE_UNION)
                                : piece;
                break;

            case ccl::doc::SelectionOp::Subtract:
                // Nothing to take it out of yet.
                if (!piece || !result) {
                    break;
                }
                result = Combine(factory, result.Get(), piece.Get(),
                                 D2D1_COMBINE_MODE_EXCLUDE);
                break;
        }
    }

    return result;
}

void SelectionGeometry::Rebuild(
    ID2D1Factory* factory, const ccl::doc::SelectionShapes& shapes) noexcept {
    geometry_ = BuildSelectionGeometry(factory, shapes);
    area_ = 0.0f;
    bounds_ = D2D1::RectF();

    if (!geometry_) {
        return;
    }
    if (FAILED(geometry_->ComputeArea(D2D1::Matrix3x2F::Identity(), &area_)) ||
        area_ <= 0.0f) {
        area_ = 0.0f;
        geometry_.Reset();
        return;
    }
    if (FAILED(geometry_->GetBounds(nullptr, &bounds_))) {
        area_ = 0.0f;
        geometry_.Reset();
    }
}

void SelectionGeometry::Clear() noexcept {
    geometry_.Reset();
    area_ = 0.0f;
    bounds_ = D2D1::RectF();
}

}  // namespace ccl::render

#pragma once

#include <d2d1.h>
#include <wrl/client.h>

#include "doc/Selection.h"

namespace ccl::render {

// Folds the pieces of a selection into one shape, in the order they were laid
// down. Null when the pieces amount to nothing.
Microsoft::WRL::ComPtr<ID2D1Geometry> BuildSelectionGeometry(
    ID2D1Factory* factory, const ccl::doc::SelectionShapes& shapes) noexcept;

// The folded shape together with the answers taken from it.
//
// Rebuilt when told to rather than when asked: the shape only changes when the
// pointer moves, while the answers are wanted on every frame.
class SelectionGeometry {
public:
    void Rebuild(ID2D1Factory* factory,
                 const ccl::doc::SelectionShapes& shapes) noexcept;
    void Clear() noexcept;

    ID2D1Geometry* Get() const noexcept { return geometry_.Get(); }

    // Zero once nothing is left. This, rather than the bounding box, is what
    // says whether anything is selected: taking everything away again leaves a
    // shape whose box comes back as infinities, which would sail through a
    // "is it wider than nothing" test and then poison whatever used it.
    float Area() const noexcept { return area_; }

    // Only meaningful while Area() is above zero.
    D2D1_RECT_F Bounds() const noexcept { return bounds_; }

private:
    Microsoft::WRL::ComPtr<ID2D1Geometry> geometry_;
    float area_ = 0.0f;
    D2D1_RECT_F bounds_{};
};

}  // namespace ccl::render

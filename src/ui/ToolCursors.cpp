#include "ui/ToolCursors.h"

#include <cmath>
#include <iterator>
#include <vector>

namespace ccl::ui {
namespace {

// Everything below is laid out on a 32x32 grid and scaled to whatever the
// system's cursor size turns out to be. Keeping the design at one size means
// the numbers can be read against the drawing they came from.
constexpr int kDesign = 32;

// The pointer. Five points rather than three: the step from (5,16) to (8,15)
// is what makes it read as a pointer instead of a wedge. The long tail the
// ordinary arrow has is left off -- it reaches across the badge and hides it.
//
// The tip is the first point, and is also the hot spot: the pixel a press
// lands on is the one the pointer is aimed at.
constexpr POINT kArrow[] = {
    {1, 1}, {1, 20}, {5, 16}, {8, 15}, {16, 15},
};

// The badge sits in the corner away from the tip. Its outline is drawn twice,
// dark and then light, so it reads over whatever it is sitting on -- the same
// reasoning the brush ring and the selection band are drawn with.
constexpr int kBadgeLeft = 10;
constexpr int kBadgeTop = 10;
constexpr int kBadgeSize = 19;  // so the 3px dark edge stays inside the grid

// The middle, filled in for the tools that pick out pieces rather than areas.
constexpr int kCoreLeft = 15;
constexpr int kCoreTop = 15;
constexpr int kCoreSize = 10;

// The hand-drawn loop, as eight distances from its middle. Deliberately
// uneven: with one radius throughout this comes out a circle, and a circle
// says "ellipse" rather than "drawn round it by hand".
constexpr double kLoopRadii[] = {9.5, 7.2, 9.0, 6.9, 9.5, 8.0, 6.9, 9.0};
constexpr double kLoopCentre = 19.5;

constexpr COLORREF kBlack = RGB(0, 0, 0);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kArea = RGB(89, 166, 255);     // picking an area
constexpr COLORREF kObject = RGB(77, 217, 102);   // picking the pieces
constexpr COLORREF kRemove = RGB(255, 77, 77);    // Alt: taking them back out

// Nothing in the palette is this, so it can stand for "not drawn on" while GDI
// works, which leaves nowhere for an alpha channel.
constexpr COLORREF kNothing = RGB(255, 0, 255);

int Scaled(int value, int size) noexcept {
    return static_cast<int>(std::lround(value * (size / double(kDesign))));
}

// One span of a closed Catmull-Rom through the loop's points. Worked out here
// rather than reached for: GDI draws bezier curves and straight lines, and a
// spline through given points is neither.
POINT SplinePoint(const POINT* knots, size_t count, size_t i, double t) noexcept {
    const POINT& p0 = knots[(i + count - 1) % count];
    const POINT& p1 = knots[i % count];
    const POINT& p2 = knots[(i + 1) % count];
    const POINT& p3 = knots[(i + 2) % count];

    const double t2 = t * t;
    const double t3 = t2 * t;
    const auto axis = [&](LONG a, LONG b, LONG c, LONG d) {
        return 0.5 * ((2.0 * b) + (-a + c) * t + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 +
                      (-a + 3.0 * b - 3.0 * c + d) * t3);
    };
    return POINT{static_cast<LONG>(std::lround(axis(p0.x, p1.x, p2.x, p3.x))),
                 static_cast<LONG>(std::lround(axis(p0.y, p1.y, p2.y, p3.y)))};
}

std::vector<POINT> LoopOutline(int size) noexcept {
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kQuarterTurn = 1.5707963267948966;
    const double scale = size / double(kDesign);

    POINT knots[std::size(kLoopRadii)]{};
    for (size_t i = 0; i < std::size(kLoopRadii); ++i) {
        // Starting straight up and going round, so the radii read in the same
        // order they are written above.
        const double angle = kTwoPi * i / std::size(kLoopRadii) - kQuarterTurn;
        const double x = kLoopCentre + kLoopRadii[i] * std::cos(angle);
        const double y = kLoopCentre + kLoopRadii[i] * std::sin(angle);
        knots[i].x = static_cast<LONG>(std::lround(x * scale));
        knots[i].y = static_cast<LONG>(std::lround(y * scale));
    }

    // Enough steps that the curve is smooth at the sizes a cursor is ever
    // drawn at; more would only repeat pixels.
    constexpr int kSteps = 8;
    std::vector<POINT> outline;
    outline.reserve(std::size(knots) * kSteps + 1);
    for (size_t i = 0; i < std::size(knots); ++i) {
        for (int step = 0; step < kSteps; ++step) {
            outline.push_back(SplinePoint(knots, std::size(knots), i, step / double(kSteps)));
        }
    }
    outline.push_back(outline.front());
    return outline;
}

void StrokeBadge(HDC dc, bool lasso, int size, int width, COLORREF colour) noexcept {
    HPEN pen = ::CreatePen(PS_SOLID, Scaled(width, size), colour);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    if (lasso) {
        const std::vector<POINT> outline = LoopOutline(size);
        ::Polyline(dc, outline.data(), static_cast<int>(outline.size()));
    } else {
        const int left = Scaled(kBadgeLeft, size);
        const int top = Scaled(kBadgeTop, size);
        ::Rectangle(dc, left, top, left + Scaled(kBadgeSize, size) + 1,
                    top + Scaled(kBadgeSize, size) + 1);
    }

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void FillCore(HDC dc, bool lasso, int size, int inset, COLORREF colour) noexcept {
    HBRUSH brush = ::CreateSolidBrush(colour);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));

    const int left = Scaled(kCoreLeft + inset, size);
    const int top = Scaled(kCoreTop + inset, size);
    const int span = Scaled(kCoreSize - inset * 2, size);
    // GDI leaves off the far edge, and a filled shape wants it.
    if (lasso) {
        ::Ellipse(dc, left, top, left + span + 1, top + span + 1);
    } else {
        ::Rectangle(dc, left, top, left + span + 1, top + span + 1);
    }

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void DrawArrow(HDC dc, int size) noexcept {
    POINT points[std::size(kArrow)]{};
    for (size_t i = 0; i < std::size(kArrow); ++i) {
        points[i].x = Scaled(kArrow[i].x, size);
        points[i].y = Scaled(kArrow[i].y, size);
    }

    HPEN pen = ::CreatePen(PS_SOLID, 1, kBlack);
    HBRUSH brush = ::CreateSolidBrush(kWhite);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);

    ::Polygon(dc, points, static_cast<int>(std::size(points)));

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

// Builds one cursor. The drawing is done with GDI, which knows nothing about
// transparency, so the sheet starts filled with a colour the design never uses
// and every pixel still holding it afterwards becomes see-through.
HCURSOR BuildOne(bool lasso, bool filled, COLORREF colour, int cx, int cy) noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = cx;
    info.bmiHeader.biHeight = -cy;  // top-down, so y runs the way the design does
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP colourBits =
        ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (colourBits == nullptr || pixels == nullptr) {
        if (colourBits != nullptr) {
            ::DeleteObject(colourBits);
        }
        return nullptr;
    }

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        ::DeleteObject(colourBits);
        return nullptr;
    }
    HGDIOBJ oldBitmap = ::SelectObject(dc, colourBits);

    RECT all{0, 0, cx, cy};
    HBRUSH nothing = ::CreateSolidBrush(kNothing);
    ::FillRect(dc, &all, nothing);
    ::DeleteObject(nothing);

    // The badge first, so the pointer stays on top of it where they overlap:
    // the pointer is the part that has to stay readable.
    StrokeBadge(dc, lasso, cx, 3, kBlack);
    StrokeBadge(dc, lasso, cx, 1, colour);
    if (filled) {
        FillCore(dc, lasso, cx, 0, kBlack);
        FillCore(dc, lasso, cx, 1, colour);
    }
    DrawArrow(dc, cx);

    ::SelectObject(dc, oldBitmap);
    ::DeleteDC(dc);

    // GDI wrote nothing into the top byte, so it is set here: opaque wherever
    // something was drawn, clear where the sheet still shows through.
    const DWORD clear = 0x00FF00FF;  // kNothing, as the DIB stores it
    auto* argb = static_cast<DWORD*>(pixels);
    for (int i = 0; i < cx * cy; ++i) {
        argb[i] = (argb[i] & 0x00FFFFFF) == clear ? 0u : (argb[i] | 0xFF000000u);
    }

    // A mask is still required even for a 32-bit cursor. An all-zero one means
    // "take the colour sheet as it stands", which is what the alpha above says.
    //
    // Zeroed explicitly: created without bits, what it holds is undefined, and
    // whatever happened to be in that memory would show up around the edges.
    const size_t maskStride = ((static_cast<size_t>(cx) + 31) / 32) * 4;
    const std::vector<BYTE> maskBits(maskStride * static_cast<size_t>(cy), 0);
    HBITMAP mask = ::CreateBitmap(cx, cy, 1, 1, maskBits.data());
    if (mask == nullptr) {
        ::DeleteObject(colourBits);
        return nullptr;
    }

    ICONINFO icon{};
    icon.fIcon = FALSE;  // a cursor, so the hot spot below is used
    icon.xHotspot = static_cast<DWORD>(Scaled(kArrow[0].x, cx));
    icon.yHotspot = static_cast<DWORD>(Scaled(kArrow[0].y, cy));
    icon.hbmMask = mask;
    icon.hbmColor = colourBits;

    HCURSOR cursor = reinterpret_cast<HCURSOR>(::CreateIconIndirect(&icon));
    ::DeleteObject(mask);
    ::DeleteObject(colourBits);
    return cursor;
}

}  // namespace

void ToolCursors::Build() noexcept {
    if (built_) {
        return;
    }
    built_ = true;

    // The system's own size rather than the 32 the design is drawn on: a fixed
    // size comes out small wherever the pointer has been made larger.
    int cx = ::GetSystemMetrics(SM_CXCURSOR);
    int cy = ::GetSystemMetrics(SM_CYCURSOR);
    if (cx <= 0 || cy <= 0) {
        cx = kDesign;
        cy = kDesign;
    }

    struct Shape {
        bool lasso;
        bool filled;
        COLORREF colour;
    };
    const Shape shapes[] = {
        {false, false, kArea},    // Rect
        {true, false, kArea},     // Lasso
        {false, true, kObject},   // ObjectRect
        {true, true, kObject},    // ObjectLasso
    };

    for (size_t i = 0; i < std::size(shapes); ++i) {
        cursors_[i * 2] = BuildOne(shapes[i].lasso, shapes[i].filled, shapes[i].colour, cx, cy);
        cursors_[i * 2 + 1] = BuildOne(shapes[i].lasso, shapes[i].filled, kRemove, cx, cy);
    }
}

HCURSOR ToolCursors::Get(SelectCursor which, bool removing) const noexcept {
    const size_t index = static_cast<size_t>(which) * 2 + (removing ? 1 : 0);
    return index < kCount ? cursors_[index] : nullptr;
}

void ToolCursors::Destroy() noexcept {
    for (HCURSOR& cursor : cursors_) {
        if (cursor != nullptr) {
            ::DestroyCursor(cursor);
            cursor = nullptr;
        }
    }
    built_ = false;
}

}  // namespace ccl::ui

#include "render/Renderer.h"

#include "doc/Document.h"
#include "render/D2DContext.h"
#include "util/Timing.h"

namespace ccl::render {

void Renderer::Attach(const D2DContext& context, HWND hwnd) noexcept {
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
    return target_ != nullptr;
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

void Renderer::Draw() noexcept {
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

    // Clearing to the outline colour and insetting the image by the border
    // width leaves exactly a one pixel frame around the capture.
    target_->Clear(D2D1::ColorF(0.29f, 0.29f, 0.29f));

    if (image_) {
        const auto inset = static_cast<float>(kWindowBorder);
        const D2D1_SIZE_F size = image_->GetSize();
        target_->DrawBitmap(
            image_.Get(),
            D2D1::RectF(inset, inset, inset + size.width, inset + size.height),
            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }

    if (measure) {
        watch.Lap(L"  d2d present");
        measuredFirstDraw_ = true;
    }
}

}  // namespace ccl::render

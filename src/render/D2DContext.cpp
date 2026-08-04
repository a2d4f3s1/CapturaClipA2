#include "render/D2DContext.h"

#include <cstdio>

#include "util/Timing.h"

namespace ccl::render {
namespace {

D2D1_RENDER_TARGET_PROPERTIES TargetProperties(
    D2D1_RENDER_TARGET_TYPE type) noexcept {
    return D2D1::RenderTargetProperties(
        type,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        // Force 96 DPI: the process works in physical pixels, so letting D2D
        // apply its own DPI scaling would double-scale the capture.
        96.0f, 96.0f);
}

}  // namespace

D2DContext::~D2DContext() {
    imaging_.Reset();
    text_.Reset();
    factory_.Reset();

    if (comInitialized_) {
        ::CoUninitialize();
    }
}

bool D2DContext::Initialize() noexcept {
    if (FAILED(::CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return false;
    }
    comInitialized_ = true;

    D2D1_FACTORY_OPTIONS options{};
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;

    const HRESULT hr = ::D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(factory_.GetAddressOf()));

    return SUCCEEDED(hr);
}

IDWriteFactory* D2DContext::Text() noexcept {
    if (text_) {
        return text_.Get();
    }

    const HRESULT hr = ::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(text_.GetAddressOf()));

    return SUCCEEDED(hr) ? text_.Get() : nullptr;
}

Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> D2DContext::CreateHwndTarget(
    HWND hwnd, UINT width, UINT height) const noexcept {
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target;
    if (!factory_) {
        return target;
    }

    const D2D1_HWND_RENDER_TARGET_PROPERTIES windowProperties =
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height),
                                         D2D1_PRESENT_OPTIONS_IMMEDIATELY);

    // Asked for outright rather than left to DEFAULT, which settles for
    // software when the GPU is not to be had and says nothing about it. The
    // difference is between a frame costing a fraction of a millisecond and
    // costing tens of them, so it is worth knowing which one this is.
    const wchar_t* kind = L"hardware";
    if (FAILED(factory_->CreateHwndRenderTarget(
            TargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE),
            windowProperties, &target))) {
        kind = L"software";
        target.Reset();
        if (FAILED(factory_->CreateHwndRenderTarget(
                TargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT),
                windowProperties, &target))) {
            target.Reset();
            return target;
        }
    }

    if (ccl::timing::g_enabled) {
        // Where the window is goes in as well as how big it is. The cost of a
        // frame turned out not to follow the pixel count alone, and the other
        // thing it might follow is which output has to be handed the result --
        // the displays here are on three different adapters, at two different
        // refresh rates, and one of them is not a graphics output at all.
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR handle =
            ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        wchar_t where[96] = L"?";
        if (::GetMonitorInfoW(handle, &monitor)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            const DWORD refresh =
                ::EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS,
                                       &mode)
                    ? mode.dmDisplayFrequency
                    : 0;

            RECT bounds{};
            ::GetWindowRect(hwnd, &bounds);
            const bool spans = bounds.left < monitor.rcMonitor.left ||
                               bounds.top < monitor.rcMonitor.top ||
                               bounds.right > monitor.rcMonitor.right ||
                               bounds.bottom > monitor.rcMonitor.bottom;

            ::swprintf_s(where, L"%s %luHz %s", monitor.szDevice, refresh,
                         spans ? L"spanning" : L"single");
        }

        wchar_t line[192];
        ::swprintf_s(line, L"[timing] render target          %-8s %ux%u  %s\n",
                     kind, width, height, where);
        ccl::timing::Write(line);
    }
    return target;
}

IWICImagingFactory* D2DContext::Imaging() noexcept {
    if (imaging_) {
        return imaging_.Get();
    }

    const HRESULT hr =
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging_));

    return SUCCEEDED(hr) ? imaging_.Get() : nullptr;
}

}  // namespace ccl::render

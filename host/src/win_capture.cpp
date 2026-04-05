// Window capture using Windows.Graphics.Capture API.
// Requires Win10 1903+, C++17, windowsapp.lib.

#include "win_capture.h"

#include <winrt/base.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <fstream>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

static wgd3::IDirect3DDevice wrapDevice(ID3D11Device* d3d) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    d3d->QueryInterface(dxgi.put());
    winrt::com_ptr<::IInspectable> insp;
    CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    return insp.as<wgd3::IDirect3DDevice>();
}

bool captureWindowToBMP(HWND hwnd, const char* bmpPath) {
    if (!hwnd) return false;

    // D3D11 device for capture (separate from Host Vulkan device)
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
    if (!dev) return false;

    auto device = wrapDevice(dev.get());

    // GraphicsCaptureItem from HWND
    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = factory->CreateForWindow(hwnd,
        winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(hr) || !item) {
        fprintf(stderr, "[WinCapture] CreateForWindow failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    auto size = item.Size();

    // FreeThreaded frame pool (no DispatcherQueue needed)
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    auto session = pool.CreateCaptureSession(item);

    // Wait for one frame
    HANDLE evt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    wgc::Direct3D11CaptureFrame frame{ nullptr };
    pool.FrameArrived([&](auto& sender, auto&) {
        frame = sender.TryGetNextFrame();
        SetEvent(evt);
    });
    session.StartCapture();
    WaitForSingleObject(evt, 3000);
    CloseHandle(evt);
    session.Close();
    pool.Close();

    if (!frame) {
        fprintf(stderr, "[WinCapture] No frame captured\n");
        return false;
    }

    // Get texture from captured surface
    auto surface = frame.Surface();
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    access->GetInterface(IID_PPV_ARGS(tex.put()));

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    // Create staging texture for CPU read
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    dev->CreateTexture2D(&desc, nullptr, staging.put());
    ctx->CopyResource(staging.get(), tex.get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);

    uint32_t w = desc.Width, h = desc.Height;
    uint32_t imgSize = w * 4 * h;

    // Quick pixel analysis
    auto* px = (uint8_t*)mapped.pData;
    uint32_t nz = 0;
    for (uint32_t y = 0; y < h; y++) {
        auto* row = px + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; x++) {
            if (row[x*4] | row[x*4+1] | row[x*4+2]) nz++;
        }
    }
    uint32_t cy = h/2, cx = w/2;
    auto* cp = px + cy * mapped.RowPitch + cx * 4;
    fprintf(stderr, "[WinCapture] %ux%u nz=%u/%u (%u%%) center=BGRA(%u,%u,%u,%u)\n",
            w, h, nz, w*h, 100*nz/(w*h), cp[0], cp[1], cp[2], cp[3]);

    // Write BMP
    FILE* f = fopen(bmpPath, "wb");
    if (f) {
        uint8_t fh[14] = {}; fh[0]='B'; fh[1]='M';
        *(uint32_t*)(fh+2) = 14+40+imgSize;
        *(uint32_t*)(fh+10) = 54;
        fwrite(fh, 1, 14, f);
        uint8_t dh[40] = {};
        *(uint32_t*)(dh+0) = 40;
        *(int32_t*)(dh+4) = w;
        *(int32_t*)(dh+8) = -(int32_t)h;
        *(uint16_t*)(dh+12) = 1;
        *(uint16_t*)(dh+14) = 32;
        fwrite(dh, 1, 40, f);
        for (uint32_t y = 0; y < h; y++)
            fwrite(px + y * mapped.RowPitch, 1, w * 4, f);
        fclose(f);
    }

    ctx->Unmap(staging.get(), 0);
    fprintf(stderr, "[WinCapture] Saved %s\n", bmpPath);
    return true;
}

// capture_helper.exe — WGC single-shot window capture
// Usage: capture_helper.exe <window_title_substr> <output.png>
// Returns: 0 success, 1 failure (reason on stderr)
//
// Uses Windows Graphics Capture API (Windows 10 1803+) via C++/WinRT.
// Sync polling approach: TryGetNextFrame() loop, no callbacks/coroutines.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>

// windows.graphics.directx.direct3d11.interop.h is guarded by WINAPI_PARTITION_APP
// and may not emit its declarations for plain desktop CMake projects.
// Declare the only thing we need manually — the GUID is stable and will never change.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

// CreateDirect3D11DeviceFromDXGIDevice is provided by the C++/WinRT projection
// (winrt/Windows.Graphics.DirectX.Direct3D11.h) for non-CX code.
// Declare it here in case the header guard above hid it.
#if !defined(CREATE_DIRECT3D11_DEVICE_FROM_DXGI_DEVICE_DECLARED)
STDAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**);
#define CREATE_DIRECT3D11_DEVICE_FROM_DXGI_DEVICE_DECLARED
#endif

#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wgc    = winrt::Windows::Graphics::Capture;
namespace wdx    = winrt::Windows::Graphics::DirectX;
namespace wdxd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

// ---------------------------------------------------------------------------
// Window search
// ---------------------------------------------------------------------------

struct FindWndData { std::wstring substr; std::vector<HWND> results; };

static BOOL CALLBACK EnumWndProc(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindWndData*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512];
    GetWindowTextW(hwnd, title, 512);
    if (std::wstring(title).find(d->substr) != std::wstring::npos) {
        d->results.push_back(hwnd);
    }
    return TRUE;  // continue — collect all matches
}

// Returns all visible windows whose title contains substr, in Z-order (topmost first).
static std::vector<HWND> FindAllWindowsSubstr(const std::wstring& substr) {
    FindWndData d{ substr, {} };
    EnumWindows(EnumWndProc, reinterpret_cast<LPARAM>(&d));
    return d.results;
}

// ---------------------------------------------------------------------------
// PNG writer via WIC
// ---------------------------------------------------------------------------

static bool WritePng(const wchar_t* path,
                     const uint8_t* pixels, UINT width, UINT height, UINT srcRowPitch)
{
    // Copy to contiguous BGRA buffer (srcRowPitch may have padding)
    std::vector<uint8_t> buf(width * height * 4);
    for (UINT y = 0; y < height; y++) {
        memcpy(buf.data() + y * width * 4,
               pixels + y * srcRowPitch,
               width * 4);
    }

    winrt::com_ptr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(fac.put())))) return false;

    winrt::com_ptr<IWICStream> stream;
    if (FAILED(fac->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;

    winrt::com_ptr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()))) return false;
    if (FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(frame.put(), props.put()))) return false;
    if (FAILED(frame->Initialize(props.get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    UINT stride = width * 4;
    if (FAILED(frame->WritePixels(height, stride, stride * height, buf.data()))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(enc->Commit())) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: capture_helper.exe <window_title_substr> <output.png>\n");
        return 1;
    }
    const wchar_t* titleSubstr = argv[1];
    const wchar_t* outputPath  = argv[2];

    // WinRT requires MTA
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Find all windows matching the title substr, try each for WGC compatibility
    auto candidates = FindAllWindowsSubstr(titleSubstr);
    if (candidates.empty()) {
        fprintf(stderr, "ERROR: no visible window with title containing '%ls'\n", titleSubstr);
        return 1;
    }

    // Get WGC interop factory (needed to test CreateForWindow)
    auto interopFactory = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();

    HWND hwnd = nullptr;
    for (HWND candidate : candidates) {
        wchar_t title[512] = {};
        GetWindowTextW(candidate, title, 512);
        LONG style = GetWindowLongW(candidate, GWL_STYLE);
        HWND parent = GetParent(candidate);
        fprintf(stderr, "INFO: candidate HWND=%p title='%ls' style=0x%08lX parent=%p\n",
                (void*)candidate, title, (unsigned long)style, (void*)parent);
        // Test whether WGC can capture this window
        wgc::GraphicsCaptureItem testItem{ nullptr };
        HRESULT testHr = interopFactory->CreateForWindow(candidate,
            winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(testItem));
        if (SUCCEEDED(testHr)) {
            hwnd = candidate;
            fprintf(stderr, "INFO: selected HWND=%p for capture\n", (void*)hwnd);
            break;
        }
        fprintf(stderr, "INFO: HWND=%p CreateForWindow failed 0x%08X, skipping\n",
                (void*)candidate, (unsigned int)testHr);
    }
    if (!hwnd) {
        fprintf(stderr, "ERROR: no capturable window found with title containing '%ls'\n", titleSubstr);
        return 1;
    }

    // Create D3D11 device (BGRA support required for WGC)
    winrt::com_ptr<ID3D11Device>        d3dDev;
    winrt::com_ptr<ID3D11DeviceContext> d3dCtx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        d3dDev.put(), &fl, d3dCtx.put());
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: D3D11CreateDevice failed 0x%08X\n", hr);
        return 1;
    }

    // Wrap as WinRT IDirect3DDevice
    winrt::com_ptr<IDXGIDevice> dxgiDev;
    d3dDev->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgiDev.put_void());
    wdxd3d::IDirect3DDevice winrtDev{ nullptr };
    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDev.get(), reinterpret_cast<IInspectable**>(winrt::put_abi(winrtDev)));
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreateDirect3D11DeviceFromDXGIDevice failed 0x%08X\n", hr);
        return 1;
    }

    // Create capture item for the selected window (already tested above)
    wgc::GraphicsCaptureItem item{ nullptr };
    hr = interopFactory->CreateForWindow(hwnd,
        winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreateForWindow failed 0x%08X\n", hr);
        return 1;
    }

    // Create frame pool and session
    auto itemSize = item.Size();
    auto pool = wgc::Direct3D11CaptureFramePool::Create(
        winrtDev,
        wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        itemSize);
    auto session = pool.CreateCaptureSession(item);
    session.StartCapture();

    // Poll for a frame: 50ms x 60 = up to 3 seconds
    wgc::Direct3D11CaptureFrame capturedFrame{ nullptr };
    for (int i = 0; i < 60; i++) {
        Sleep(50);
        capturedFrame = pool.TryGetNextFrame();
        if (capturedFrame) break;
    }

    session.Close();
    pool.Close();

    if (!capturedFrame) {
        fprintf(stderr, "ERROR: timeout waiting for capture frame\n");
        return 1;
    }

    // Get D3D11 texture from frame surface via IDirect3DDxgiInterfaceAccess
    auto surface = capturedFrame.Surface();
    auto dxgiAccess = surface.as<IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> frameTex;
    hr = dxgiAccess->GetInterface(IID_PPV_ARGS(frameTex.put()));
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: GetInterface(ID3D11Texture2D) failed 0x%08X\n", hr);
        return 1;
    }

    D3D11_TEXTURE2D_DESC texDesc;
    frameTex->GetDesc(&texDesc);
    UINT capW = texDesc.Width;
    UINT capH = texDesc.Height;

    // Staging texture for CPU readback
    D3D11_TEXTURE2D_DESC stagDesc = texDesc;
    stagDesc.Usage          = D3D11_USAGE_STAGING;
    stagDesc.BindFlags      = 0;
    stagDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagDesc.MiscFlags      = 0;
    stagDesc.MipLevels      = 1;
    stagDesc.ArraySize      = 1;
    stagDesc.SampleDesc     = { 1, 0 };

    winrt::com_ptr<ID3D11Texture2D> stagTex;
    hr = d3dDev->CreateTexture2D(&stagDesc, nullptr, stagTex.put());
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreateTexture2D(staging) failed 0x%08X\n", hr);
        return 1;
    }

    d3dCtx->CopyResource(stagTex.get(), frameTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = d3dCtx->Map(stagTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: Map failed 0x%08X\n", hr);
        return 1;
    }

    bool ok = WritePng(outputPath,
                       static_cast<const uint8_t*>(mapped.pData),
                       capW, capH, mapped.RowPitch);
    d3dCtx->Unmap(stagTex.get(), 0);

    if (!ok) {
        fprintf(stderr, "ERROR: WritePng failed\n");
        return 1;
    }

    fprintf(stdout, "OK %ux%u -> %ls\n", capW, capH, outputPath);
    return 0;
}

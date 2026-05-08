// DX11 Resize Test — VBox GPU Bridge (Track A1.5a validation)
//
// Exercises the swapchain destroy/recreate path:
//   - Initial swapchain at 800x600
//   - Every RESIZE_INTERVAL_FRAMES, cycle to next size in g_sizes[]
//   - DXVK responds to ResizeBuffers with vkDestroySwapchainKHR +
//     vkCreateSwapchainKHR, exercising VN_CMD_BRIDGE_DestroySwapchain
//
// Validation:
//   - Host VRAM / map counts (visible in dashboard / DIAG) stay flat across cycles
//   - No crashes / glitches on resize
//   - Final cleanup releases all resources cleanly

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

struct Size { uint32_t w, h; const char* label; };
static const Size g_sizes[] = {
    {  800,  600, "800x600"   },
    { 1280,  720, "1280x720"  },
    { 1920, 1080, "1920x1080" },
    {  640,  480, "640x480"   },
};
static constexpr int NUM_SIZES = sizeof(g_sizes) / sizeof(g_sizes[0]);
static constexpr int RESIZE_INTERVAL_FRAMES = 180; // ~3s @ 60 FPS
static constexpr int MAX_CYCLES = 30; // 30 resize cycles, then exit

static bool g_running = true;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE: g_running = false; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

struct Vertex { float x, y, z; float r, g, b; };

static const char* g_vsSource = R"(
struct VS_IN  { float3 pos : POSITION; float3 col : COLOR; };
struct VS_OUT { float4 pos : SV_Position; float3 col : COLOR; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos = float4(i.pos, 1.0);
    o.col = i.col;
    return o;
}
)";

static const char* g_psSource = R"(
float4 main(float4 pos : SV_Position, float3 col : COLOR) : SV_Target {
    return float4(col, 1.0);
}
)";

static ID3DBlob* compileShader(const char* source, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            fprintf(stderr, "Shader compile error: %s\n", (char*)errors->GetBufferPointer());
            errors->Release();
        }
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

int main(int /*argc*/, char* /*argv*/[]) {
    fprintf(stderr, "[Resize Test] Starting. Will cycle through %d resolutions, %d cycles total.\n",
            NUM_SIZES, MAX_CYCLES);

    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DX11ResizeTest";
    RegisterClassExA(&wc);

    uint32_t curW = g_sizes[0].w, curH = g_sizes[0].h;
    RECT rect = { 0, 0, (LONG)curW, (LONG)curH };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "DX11 Resize Test",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = curW;
    scd.BufferDesc.Height = curH;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate = { 60, 1 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapchain, &device, &featureLevel, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[Resize Test] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 1;
    }

    // RTV / viewport (recreated after each resize)
    ID3D11RenderTargetView* rtv = nullptr;
    auto buildRtvAndViewport = [&]() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        ID3D11Texture2D* bb = nullptr;
        swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        device->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (FLOAT)curW, (FLOAT)curH, 0, 1 };
        ctx->RSSetViewports(1, &vp);
    };
    buildRtvAndViewport();

    // Shaders, IL, VB
    ID3DBlob* vsBlob = compileShader(g_vsSource, "vs_5_0");
    ID3DBlob* psBlob = compileShader(g_psSource, "ps_5_0");
    if (!vsBlob || !psBlob) {
        fprintf(stderr, "[Resize Test] Shader compile failed\n");
        return 1;
    }
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* inputLayout = nullptr;
    device->CreateInputLayout(inputDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
    vsBlob->Release();
    psBlob->Release();

    ctx->IASetInputLayout(inputLayout);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);

    Vertex vertices[] = {
        {  0.0f,  0.7f, 0.0f, 1, 0, 0 },
        { -0.7f, -0.5f, 0.0f, 0, 1, 0 },
        {  0.7f, -0.5f, 0.0f, 0, 0, 1 },
    };
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(vertices);
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{vertices};
    ID3D11Buffer* vb = nullptr;
    device->CreateBuffer(&vbd, &vsd, &vb);
    UINT stride = sizeof(Vertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The actual resize logic
    auto doResize = [&](uint32_t newW, uint32_t newH, const char* label) {
        // Detach the RTV first; ResizeBuffers fails if back buffer is still bound.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (rtv) { rtv->Release(); rtv = nullptr; }

        // Resize the host window so DXVK queries match.
        RECT r2 = { 0, 0, (LONG)newW, (LONG)newH };
        AdjustWindowRect(&r2, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        HRESULT rhr = swapchain->ResizeBuffers(2, newW, newH,
                                               DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(rhr)) {
            fprintf(stderr, "[Resize Test] ResizeBuffers(%s) FAILED: 0x%08X\n", label, rhr);
            return false;
        }
        curW = newW; curH = newH;
        buildRtvAndViewport();
        return true;
    };

    fprintf(stderr, "[Resize Test] Setup complete. Cycling resolutions every %d frames.\n",
            RESIZE_INTERVAL_FRAMES);

    MSG msg{};
    uint32_t frameCount = 0;
    int sizeIdx = 0;
    int cyclesDone = 0;

    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        // Trigger resize on cadence
        if (frameCount > 0 && (frameCount % RESIZE_INTERVAL_FRAMES) == 0) {
            sizeIdx = (sizeIdx + 1) % NUM_SIZES;
            const Size& sz = g_sizes[sizeIdx];
            fprintf(stderr, "[Resize Test] cycle=%d frame=%u → %s\n",
                    cyclesDone, frameCount, sz.label);
            fflush(stderr);
            if (!doResize(sz.w, sz.h, sz.label)) {
                fprintf(stderr, "[Resize Test] FATAL: resize failed, exiting\n");
                break;
            }
            cyclesDone++;
            if (cyclesDone >= MAX_CYCLES) {
                fprintf(stderr, "[Resize Test] Completed %d cycles, exiting cleanly.\n", MAX_CYCLES);
                g_running = false;
                break;
            }
        }

        float clearColor[4] = {
            ((sizeIdx & 1) ? 0.2f : 0.0f),
            ((sizeIdx & 2) ? 0.2f : 0.0f),
            0.1f, 1.0f
        };
        ctx->ClearRenderTargetView(rtv, clearColor);
        ctx->Draw(3, 0);
        swapchain->Present(1, 0);
        frameCount++;
    }

    // Cleanup
    if (rtv) rtv->Release();
    if (vb) vb->Release();
    if (inputLayout) inputLayout->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (swapchain) swapchain->Release();
    if (ctx) ctx->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);

    fprintf(stderr, "[Resize Test] Done. cycles=%d frames=%u\n", cyclesDone, frameCount);
    return 0;
}

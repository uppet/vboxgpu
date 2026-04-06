// Minimal DX11 triangle test program.
// When run alongside DXVK's d3d11.dll + our vulkan-1.dll ICD,
// it exercises the full chain: DX11 → DXVK → ICD → TCP → Host Vulkan.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static constexpr UINT WIDTH = 800;
static constexpr UINT HEIGHT = 600;
static bool g_running = true;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static const char* g_vsSource = R"(
float4 main(uint vid : SV_VertexID) : SV_Position {
    float2 pos[3] = {
        float2( 0.0, -0.5),
        float2(-0.5,  0.5),
        float2( 0.5,  0.5)
    };
    return float4(pos[vid], 0.0, 1.0);
}
)";

static const char* g_psSource = R"(
cbuffer TimeBuffer : register(b0) {
    float time;
    float3 pad;
};

float3 hsv2rgb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0 - abs(fmod(h / 60.0, 2.0) - 1.0));
    float m = v - c;
    float3 rgb;
    if (h < 60.0)       rgb = float3(c, x, 0);
    else if (h < 120.0) rgb = float3(x, c, 0);
    else if (h < 180.0) rgb = float3(0, c, x);
    else if (h < 240.0) rgb = float3(0, x, c);
    else if (h < 300.0) rgb = float3(x, 0, c);
    else                 rgb = float3(c, 0, x);
    return rgb + m;
}

float4 main() : SV_Target {
    float hue = fmod(time * 60.0, 360.0);
    float3 color = hsv2rgb(hue, 1.0, 1.0);
    return float4(color, 1.0);
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

int main(int argc, char* argv[]) {
    fprintf(stderr, "[DX11 Test] Starting...\n");

    // Window
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DX11TriangleTest";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "DX11 Triangle - VBox GPU Bridge Test",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // Create device and swap chain
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = WIDTH;
    scd.BufferDesc.Height = HEIGHT;
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
        0, // no debug flag — DXVK doesn't support D3D debug layer
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapchain, &device, &featureLevel, &ctx);

    if (FAILED(hr)) {
        fprintf(stderr, "[DX11 Test] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 1;
    }
    fprintf(stderr, "[DX11 Test] Device created. Feature level: 0x%X\n", featureLevel);

    // Render target view
    ID3D11Texture2D* backBuffer = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Viewport
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0, 1 };
    ctx->RSSetViewports(1, &vp);

    // Compile shaders
    ID3DBlob* vsBlob = compileShader(g_vsSource, "vs_5_0");
    ID3DBlob* psBlob = compileShader(g_psSource, "ps_5_0");
    if (!vsBlob || !psBlob) {
        fprintf(stderr, "[DX11 Test] Shader compilation failed.\n");
        return 1;
    }

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    vsBlob->Release();
    psBlob->Release();

    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);

    // Create constant buffer for time
    struct alignas(16) TimeData { float time; float pad[3]; };
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(TimeData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* timeCB = nullptr;
    device->CreateBuffer(&cbd, nullptr, &timeCB);
    ctx->PSSetConstantBuffers(0, 1, &timeCB);

    LARGE_INTEGER freq, startTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);

    // Set topology
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    fprintf(stderr, "[DX11 Test] Setup complete. Entering render loop.\n");

    // Main loop
    MSG msg{};
    uint32_t frameCount = 0;
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        // Update time constant buffer
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float elapsed = (float)(now.QuadPart - startTime.QuadPart) / (float)freq.QuadPart;
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(timeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        TimeData* td = (TimeData*)mapped.pData;
        td->time = elapsed;
        ctx->Unmap(timeCB, 0);

        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clearColor);
        ctx->Draw(3, 0);
        swapchain->Present(1, 0);

        frameCount++;
        if (frameCount % 300 == 0)
            fprintf(stderr, "[DX11 Test] %u frames rendered.\n", frameCount);
    }

    // Cleanup
    timeCB->Release();
    vs->Release();
    ps->Release();
    rtv->Release();
    swapchain->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    fprintf(stderr, "[DX11 Test] Done. Total frames: %u\n", frameCount);
    return 0;
}

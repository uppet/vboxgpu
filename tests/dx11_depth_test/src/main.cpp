// DX11 Depth Test — VBox GPU Bridge
// Two overlapping triangles at different Z depths.
// Red triangle (z=0.7, farther) partially behind blue triangle (z=0.3, closer).
// Validates depth buffer creation, depth testing, and depth state forwarding.

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

static const uint32_t WIDTH = 800, HEIGHT = 600;
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
    o.pos = float4(i.pos.xy, i.pos.z, 1.0);
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

int main(int argc, char* argv[]) {
    fprintf(stderr, "[Depth Test] Starting...\n");

    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DX11DepthTest";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "DX11 Depth Test - VBox GPU Bridge",
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
        0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapchain, &device, &featureLevel, &ctx);

    if (FAILED(hr)) {
        fprintf(stderr, "[Depth Test] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 1;
    }
    fprintf(stderr, "[Depth Test] Device created. Feature level: 0x%X\n", featureLevel);

    // Render target view
    ID3D11Texture2D* backBuffer = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();

    // Depth buffer
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = WIDTH;
    depthDesc.Height = HEIGHT;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depthTex = nullptr;
    hr = device->CreateTexture2D(&depthDesc, nullptr, &depthTex);
    fprintf(stderr, "[Depth Test] CreateDepthTexture: hr=0x%08X\n", hr);

    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = device->CreateDepthStencilView(depthTex, &dsvDesc, &dsv);
    fprintf(stderr, "[Depth Test] CreateDSV: hr=0x%08X\n", hr);

    // Bind render target + depth buffer
    ctx->OMSetRenderTargets(1, &rtv, dsv);

    // Depth stencil state: enable depth test + write
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState* dsState = nullptr;
    device->CreateDepthStencilState(&dsDesc, &dsState);
    ctx->OMSetDepthStencilState(dsState, 0);

    // Viewport
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0, 1 };
    ctx->RSSetViewports(1, &vp);

    // Compile shaders
    ID3DBlob* vsBlob = compileShader(g_vsSource, "vs_5_0");
    ID3DBlob* psBlob = compileShader(g_psSource, "ps_5_0");
    if (!vsBlob || !psBlob) {
        fprintf(stderr, "[Depth Test] Shader compilation failed.\n");
        return 1;
    }

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    // Input layout: POSITION (float3) + COLOR (float3)
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

    // Two overlapping triangles:
    // Red triangle: z=0.7 (farther), centered slightly left
    // Blue triangle: z=0.3 (closer), centered slightly right, overlapping
    Vertex vertices[] = {
        // Red triangle (farther, z=0.7)
        { -0.3f, -0.6f, 0.7f,  1.0f, 0.2f, 0.2f },
        { -0.8f,  0.6f, 0.7f,  1.0f, 0.2f, 0.2f },
        {  0.4f,  0.4f, 0.7f,  1.0f, 0.2f, 0.2f },
        // Blue triangle (closer, z=0.3)
        {  0.3f, -0.6f, 0.3f,  0.2f, 0.3f, 1.0f },
        { -0.4f,  0.4f, 0.3f,  0.2f, 0.3f, 1.0f },
        {  0.8f,  0.6f, 0.3f,  0.2f, 0.3f, 1.0f },
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

    fprintf(stderr, "[Depth Test] Setup complete. Entering render loop.\n");
    fprintf(stderr, "[Depth Test] Expected: blue triangle (closer) occludes red triangle where they overlap.\n");

    MSG msg{};
    uint32_t frameCount = 0;
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clearColor);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

        // Draw all 6 vertices (2 triangles)
        ctx->Draw(6, 0);

        swapchain->Present(1, 0);

        frameCount++;
        if (frameCount % 300 == 0)
            fprintf(stderr, "[Depth Test] %u frames rendered.\n", frameCount);
    }

    // Cleanup
    dsState->Release();
    dsv->Release();
    depthTex->Release();
    vb->Release();
    inputLayout->Release();
    vs->Release();
    ps->Release();
    rtv->Release();
    swapchain->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    fprintf(stderr, "[Depth Test] Done. Total frames: %u\n", frameCount);
    return 0;
}

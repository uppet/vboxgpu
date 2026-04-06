// DX11 Triangle Test — VBox GPU Bridge
// Tests vertex buffer, texture, and constant buffer rendering through DXVK → ICD → Host Vulkan.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

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

struct Vertex { float x, y, z; float u, v; };

static const char* g_vsSource = R"(
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos = float4(i.pos, 1.0);
    o.uv = i.uv;
    return o;
}
)";

static const char* g_psSource = R"(
cbuffer TimeBuffer : register(b0) {
    float time;
    float3 pad;
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

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

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float hue = fmod(time * 60.0, 360.0);
    float3 tint = hsv2rgb(hue, 1.0, 1.0);
    float3 texColor = tex.Sample(smp, uv).rgb;
    float3 uvColor = float3(uv.x, uv.y, 1.0 - uv.x);
    return float4((texColor + uvColor * 0.3) * tint, 1.0);
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
        0, nullptr, 0, D3D11_SDK_VERSION,
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

    // Input layout
    D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* inputLayout = nullptr;
    device->CreateInputLayout(inputDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
    vsBlob->Release();
    psBlob->Release();

    ctx->IASetInputLayout(inputLayout);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);

    // Vertex buffer
    Vertex vertices[] = {
        { 0.0f, -0.5f, 0.0f,  0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f,  0.0f, 1.0f},
        { 0.5f,  0.5f, 0.0f,  1.0f, 1.0f},
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

    // Checkerboard texture (64x64)
    const uint32_t TEX_W = 64, TEX_H = 64;
    std::vector<uint32_t> texData(TEX_W * TEX_H);
    for (uint32_t y = 0; y < TEX_H; y++)
        for (uint32_t x = 0; x < TEX_W; x++)
            texData[y * TEX_W + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFFFFF : 0xFF808080;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = TEX_W; td.Height = TEX_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tsd{texData.data(), TEX_W * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&td, &tsd, &tex);
    fprintf(stderr, "[DX11 Test] CreateTexture2D: hr=0x%08X tex=%p\n", hr, (void*)tex);

    // SRV (may fail if ICD doesn't support format properly)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
    fprintf(stderr, "[DX11 Test] CreateSRV: hr=0x%08X srv=%p\n", hr, (void*)srv);
    if (srv) ctx->PSSetShaderResources(0, 1, &srv);

    // Sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    device->CreateSamplerState(&sd, &sampler);
    ctx->PSSetSamplers(0, 1, &sampler);

    // Constant buffer for time
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

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    fprintf(stderr, "[DX11 Test] Setup complete. Entering render loop.\n");

    MSG msg{};
    uint32_t frameCount = 0;
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float elapsed = (float)(now.QuadPart - startTime.QuadPart) / (float)freq.QuadPart;
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(timeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        TimeData* td2 = (TimeData*)mapped.pData;
        td2->time = elapsed;
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
    sampler->Release();
    if (srv) srv->Release();
    if (tex) tex->Release();
    vb->Release();
    inputLayout->Release();
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

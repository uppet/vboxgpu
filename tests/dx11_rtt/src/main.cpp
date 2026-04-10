// DX11 Render-to-Texture Test — VBox GPU Bridge
// Tests the pattern DXVK/Unity uses: render to offscreen RT, then blit to swapchain.
// This exercises: render target creation, SRV binding, fullscreen blit, depth buffer.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static const UINT WIDTH = 800, HEIGHT = 600;
static bool g_running = true;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { g_running = false; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

// --- Scene shaders: render colored triangle to offscreen RT ---
static const char* g_sceneVS = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    // Fullscreen-ish triangle
    float2 pos[3] = { float2(0.0, 0.5), float2(-0.5, -0.5), float2(0.5, -0.5) };
    return float4(pos[id], 0.0, 1.0);
}
)";

static const char* g_scenePS = R"(
cbuffer CB : register(b0) { float time; float3 pad; };
float4 main(float4 pos : SV_Position) : SV_Target {
    float r = sin(time * 2.0) * 0.5 + 0.5;
    float g = sin(time * 3.0 + 1.0) * 0.5 + 0.5;
    float b = sin(time * 5.0 + 2.0) * 0.5 + 0.5;
    return float4(r, g, b, 1.0);
}
)";

// --- Blit shaders: sample from RT and draw to swapchain ---
static const char* g_blitVS = R"(
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    // Fullscreen triangle (covers entire screen with one triangle)
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char* g_blitPS = R"(
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return tex.Sample(smp, uv);
}
)";

static ID3DBlob* compile(const char* src, const char* target) {
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "Shader error: %s\n", (char*)err->GetBufferPointer());
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Create window
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = "DX11RTT"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("DX11RTT", "DX11 RTT Test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, WIDTH, HEIGHT, nullptr, nullptr, hInst, nullptr);

    // Create device + swapchain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2; scd.BufferDesc.Width = WIDTH; scd.BufferDesc.Height = HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr; D3D_FEATURE_LEVEL fl;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    fprintf(stderr, "[RTT] Device created: FL=%x\n", (unsigned)fl);

    // Swapchain backbuffer RTV
    ID3D11Texture2D* backBuf = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    ID3D11RenderTargetView* backRTV = nullptr;
    dev->CreateRenderTargetView(backBuf, nullptr, &backRTV);
    backBuf->Release();

    // --- Offscreen render target (the key feature being tested) ---
    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = WIDTH; rtDesc.Height = HEIGHT; rtDesc.MipLevels = 1; rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* rtTex = nullptr;
    dev->CreateTexture2D(&rtDesc, nullptr, &rtTex);
    ID3D11RenderTargetView* rtRTV = nullptr;
    dev->CreateRenderTargetView(rtTex, nullptr, &rtRTV);
    ID3D11ShaderResourceView* rtSRV = nullptr;
    dev->CreateShaderResourceView(rtTex, nullptr, &rtSRV);
    fprintf(stderr, "[RTT] Offscreen RT created: %ux%u\n", WIDTH, HEIGHT);

    // Depth buffer
    D3D11_TEXTURE2D_DESC dsDesc = rtDesc;
    dsDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* dsTex = nullptr;
    dev->CreateTexture2D(&dsDesc, nullptr, &dsTex);
    ID3D11DepthStencilView* dsv = nullptr;
    dev->CreateDepthStencilView(dsTex, nullptr, &dsv);

    // Compile shaders
    ID3DBlob* sceneVSBlob = compile(g_sceneVS, "vs_5_0");
    ID3DBlob* scenePSBlob = compile(g_scenePS, "ps_5_0");
    ID3DBlob* blitVSBlob  = compile(g_blitVS,  "vs_5_0");
    ID3DBlob* blitPSBlob  = compile(g_blitPS,  "ps_5_0");

    ID3D11VertexShader* sceneVS = nullptr; ID3D11PixelShader* scenePS = nullptr;
    ID3D11VertexShader* blitVS  = nullptr; ID3D11PixelShader* blitPS  = nullptr;
    dev->CreateVertexShader(sceneVSBlob->GetBufferPointer(), sceneVSBlob->GetBufferSize(), nullptr, &sceneVS);
    dev->CreatePixelShader(scenePSBlob->GetBufferPointer(), scenePSBlob->GetBufferSize(), nullptr, &scenePS);
    dev->CreateVertexShader(blitVSBlob->GetBufferPointer(), blitVSBlob->GetBufferSize(), nullptr, &blitVS);
    dev->CreatePixelShader(blitPSBlob->GetBufferPointer(), blitPSBlob->GetBufferSize(), nullptr, &blitPS);

    // Constant buffer
    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cbuf = nullptr;
    dev->CreateBuffer(&cbDesc, nullptr, &cbuf);

    // Sampler for blit
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sampDesc, &sampler);

    // Viewport
    D3D11_VIEWPORT vp = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };

    fprintf(stderr, "[RTT] Starting render loop\n");
    LARGE_INTEGER freq, start; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    int frame = 0;

    while (g_running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if (!g_running) break;

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float t = (float)(now.QuadPart - start.QuadPart) / freq.QuadPart;

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        *(float*)mapped.pData = t;
        ctx->Unmap(cbuf, 0);

        // === Pass 1: Render scene to offscreen RT ===
        float clearColor[4] = { 0.1f, 0.1f, 0.2f, 1.0f };
        ctx->ClearRenderTargetView(rtRTV, clearColor);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtRTV, dsv);
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(sceneVS, nullptr, 0);
        ctx->PSSetShader(scenePS, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0); // scene triangle

        // === Pass 2: Blit offscreen RT to swapchain ===
        ctx->OMSetRenderTargets(1, &backRTV, nullptr);
        ctx->VSSetShader(blitVS, nullptr, 0);
        ctx->PSSetShader(blitPS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &rtSRV);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(3, 0); // fullscreen blit

        // Unbind SRV to avoid D3D warning
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        sc->Present(1, 0);
        frame++;
        if (frame % 60 == 0)
            fprintf(stderr, "[RTT] Frame %d, time=%.1f\n", frame, t);
    }

    // Cleanup
    sampler->Release(); cbuf->Release();
    blitPS->Release(); blitVS->Release(); scenePS->Release(); sceneVS->Release();
    blitPSBlob->Release(); blitVSBlob->Release(); scenePSBlob->Release(); sceneVSBlob->Release();
    dsv->Release(); dsTex->Release();
    rtSRV->Release(); rtRTV->Release(); rtTex->Release();
    backRTV->Release(); sc->Release(); ctx->Release(); dev->Release();
    return 0;
}

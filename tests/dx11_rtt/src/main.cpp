// DX11 Render-to-Texture Test — VBox GPU Bridge
// Tests: render to offscreen RT, then blit to swapchain (same pattern as DXVK/Unity).

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

static const UINT WIDTH = 800, HEIGHT = 600;
static bool g_running = true;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE))
        { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

// Scene: render textured quads to offscreen RT using vertex buffer + index buffer
static const char* g_sceneVS = R"(
struct VS_IN  { float3 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };
cbuffer CB : register(b0) { float time; float xoff; float yoff; float scale; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    float angle = time * 0.5;
    float c = cos(angle), s = sin(angle);
    float2 p = float2(i.pos.x * c - i.pos.y * s, i.pos.x * s + i.pos.y * c);
    o.pos = float4(p * scale + float2(xoff, yoff), 0.5, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}
)";

static const char* g_scenePS = R"(
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float4 col : COLOR, float2 uv : TEXCOORD) : SV_Target {
    float4 t = tex.Sample(smp, uv);
    return col;  // TEMP: ignore texture, just vertex color — debug black screen
    //return col * t;
}
)";

// Blit: fullscreen triangle sampling from RT
static const char* g_blitVS = R"(
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
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
                            "main", target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "Shader error: %s\n", (char*)err->GetBufferPointer()); err->Release(); }
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = "DX11RTT";
    RegisterClassA(&wc);
    RECT r = {0, 0, (LONG)WIDTH, (LONG)HEIGHT};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("DX11RTT", "DX11 RTT Test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, r.right-r.left, r.bottom-r.top, nullptr, nullptr, hInst, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1; scd.BufferDesc.Width = WIDTH; scd.BufferDesc.Height = HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    fprintf(stderr, "[RTT] Device: FL=%x hr=%lx\n", (unsigned)fl, (unsigned long)hr);

    // Backbuffer RTV
    ID3D11Texture2D* backBuf = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    ID3D11RenderTargetView* backRTV = nullptr;
    dev->CreateRenderTargetView(backBuf, nullptr, &backRTV); backBuf->Release();

    // Offscreen RT
    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = WIDTH; rtDesc.Height = HEIGHT; rtDesc.MipLevels = 1; rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* rtTex = nullptr; dev->CreateTexture2D(&rtDesc, nullptr, &rtTex);
    ID3D11RenderTargetView* rtRTV = nullptr; dev->CreateRenderTargetView(rtTex, nullptr, &rtRTV);
    ID3D11ShaderResourceView* rtSRV = nullptr; dev->CreateShaderResourceView(rtTex, nullptr, &rtSRV);

    // Depth buffer for scene pass
    D3D11_TEXTURE2D_DESC dsDesc = rtDesc;
    dsDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* dsTex = nullptr; dev->CreateTexture2D(&dsDesc, nullptr, &dsTex);
    ID3D11DepthStencilView* dsv = nullptr; dev->CreateDepthStencilView(dsTex, nullptr, &dsv);
    fprintf(stderr, "[RTT] Depth buffer created: %ux%u D32_FLOAT\n", WIDTH, HEIGHT);

    // Compile shaders
    ID3DBlob* svs = compile(g_sceneVS, "vs_5_0");
    ID3DBlob* sps = compile(g_scenePS, "ps_5_0");
    ID3DBlob* bvs = compile(g_blitVS, "vs_5_0");
    ID3DBlob* bps = compile(g_blitPS, "ps_5_0");
    if (!svs || !sps || !bvs || !bps) { fprintf(stderr, "Shader compile failed\n"); return 1; }

    ID3D11VertexShader *sceneVS, *blitVS; ID3D11PixelShader *scenePS, *blitPS;
    dev->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, &sceneVS);
    dev->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &scenePS);
    dev->CreateVertexShader(bvs->GetBufferPointer(), bvs->GetBufferSize(), nullptr, &blitVS);
    dev->CreatePixelShader(bps->GetBufferPointer(), bps->GetBufferSize(), nullptr, &blitPS);

    // Input layout for scene (POSITION + COLOR + TEXCOORD)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* inputLayout = nullptr;
    dev->CreateInputLayout(layout, 3, svs->GetBufferPointer(), svs->GetBufferSize(), &inputLayout);

    // Vertex buffer: quad (2 triangles) with color + UV
    struct Vtx { float x,y,z; float r,g,b,a; float u,v; };
    Vtx verts[] = {
        {-0.6f,  0.6f, 0.5f,  1,1,1,1,  0,0},  // top-left
        { 0.6f,  0.6f, 0.5f,  1,1,1,1,  1,0},  // top-right
        { 0.6f, -0.6f, 0.5f,  1,1,1,1,  1,1},  // bottom-right
        {-0.6f, -0.6f, 0.5f,  1,1,1,1,  0,1},  // bottom-left
    };
    D3D11_BUFFER_DESC vbDesc = {}; vbDesc.ByteWidth = sizeof(verts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT; vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = {verts};
    ID3D11Buffer* vb = nullptr; dev->CreateBuffer(&vbDesc, &vbData, &vb);

    // Index buffer: two triangles forming a quad
    uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    D3D11_BUFFER_DESC ibDesc = {}; ibDesc.ByteWidth = sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT; ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = {indices};
    ID3D11Buffer* ib = nullptr; dev->CreateBuffer(&ibDesc, &ibData, &ib);

    // Checkerboard texture (8x8)
    uint32_t texData[8*8];
    for (int ty = 0; ty < 8; ty++)
        for (int tx = 0; tx < 8; tx++)
            texData[ty*8+tx] = ((tx^ty)&1) ? 0xFFFFFFFF : 0xFF808080;
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 8; texDesc.Height = 8; texDesc.MipLevels = 1; texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT; texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA texInit = {texData, 8*4, 0};
    ID3D11Texture2D* checkerTex = nullptr; dev->CreateTexture2D(&texDesc, &texInit, &checkerTex);
    ID3D11ShaderResourceView* checkerSRV = nullptr;
    dev->CreateShaderResourceView(checkerTex, nullptr, &checkerSRV);

    // Constant buffer (time + xoff + yoff + scale)
    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cbuf = nullptr; dev->CreateBuffer(&cbDesc, nullptr, &cbuf);

    // Rasterizer: no culling
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID; rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rsNoCull = nullptr; dev->CreateRasterizerState(&rsDesc, &rsNoCull);

    // Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* sampler = nullptr; dev->CreateSamplerState(&sampDesc, &sampler);

    D3D11_VIEWPORT vp = {0, 0, (float)WIDTH, (float)HEIGHT, 0, 1};
    LARGE_INTEGER freq, start; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    int frame = 0;

    fprintf(stderr, "[RTT] Entering render loop\n");
    while (g_running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if (!g_running) break;

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float t = (float)(now.QuadPart - start.QuadPart) / freq.QuadPart;

        // === Pass 1: render scene to offscreen RT (with depth + texture + DrawIndexed) ===
        float clearColor[4] = {0.1f, 0.1f, 0.3f, 1.0f};
        ctx->ClearRenderTargetView(rtRTV, clearColor);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtRTV, dsv);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rsNoCull);
        ctx->IASetInputLayout(inputLayout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = sizeof(Vtx), ofs = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &ofs);
        ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        ctx->VSSetShader(sceneVS, nullptr, 0);
        ctx->PSSetShader(scenePS, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetShaderResources(0, 1, &checkerSRV);
        ctx->PSSetSamplers(0, 1, &sampler);

        // Draw 3 quads at different positions (multi-draw + per-object cbuffer update)
        struct { float time, xoff, yoff, scale; } cbData;
        float positions[][2] = {{0, 0.3f}, {-0.4f, -0.3f}, {0.4f, -0.3f}};
        for (int d = 0; d < 3; d++) {
            cbData.time = t + d * 1.0f;
            cbData.xoff = positions[d][0];
            cbData.yoff = positions[d][1];
            cbData.scale = 0.4f;
            D3D11_MAPPED_SUBRESOURCE m2;
            ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m2);
            memcpy(m2.pData, &cbData, sizeof(cbData));
            ctx->Unmap(cbuf, 0);
            ctx->DrawIndexed(6, 0, 0);
        }

        // Unbind scene SRV before using RT as blit source
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        // === Pass 2: blit RT to swapchain ===
        ctx->OMSetRenderTargets(1, &backRTV, nullptr);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(blitVS, nullptr, 0);
        ctx->PSSetShader(blitPS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &rtSRV);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(3, 0);

        ctx->PSSetShaderResources(0, 1, &nullSRV);

        sc->Present(1, 0);
        frame++;
        if (frame % 60 == 0) fprintf(stderr, "[RTT] Frame %d t=%.1f\n", frame, t);
    }

    // Cleanup
    sampler->Release(); rsNoCull->Release(); cbuf->Release();
    checkerSRV->Release(); checkerTex->Release(); ib->Release(); vb->Release(); inputLayout->Release();
    dsv->Release(); dsTex->Release();
    blitPS->Release(); blitVS->Release(); scenePS->Release(); sceneVS->Release();
    bps->Release(); bvs->Release(); sps->Release(); svs->Release();
    rtSRV->Release(); rtRTV->Release(); rtTex->Release();
    backRTV->Release(); sc->Release(); ctx->Release(); dev->Release();
    return 0;
}

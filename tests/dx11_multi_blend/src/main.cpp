// DX11 Multi-Object + Alpha Blend Test — VBox GPU Bridge
// Multiple objects drawn with per-object constant buffer + one semi-transparent object.
// Validates: multiple Draw calls, constant buffer updates between draws, alpha blending.

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

struct Vertex { float x, y, z; };

static const char* g_vsSource = R"(
cbuffer ObjData : register(b0) {
    float2 offset;
    float  scale;
    float  pad0;
    float4 color;
};
struct VS_IN  { float3 pos : POSITION; };
struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos = float4(i.pos.xy * scale + offset, i.pos.z, 1.0);
    o.col = color;
    return o;
}
)";

static const char* g_psSource = R"(
float4 main(float4 pos : SV_Position, float4 col : COLOR) : SV_Target {
    return col;
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

struct alignas(16) ObjConstants {
    float offsetX, offsetY, scale, pad0;
    float r, g, b, a;
};

int main(int argc, char* argv[]) {
    fprintf(stderr, "[Multi+Blend] Starting...\n");

    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DX11MultiBlend";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "DX11 Multi+Blend - VBox GPU Bridge",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // Device + swap chain
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
        fprintf(stderr, "[Multi+Blend] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 1;
    }

    // Render target
    ID3D11Texture2D* backBuffer = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0, 1 };
    ctx->RSSetViewports(1, &vp);

    // Shaders
    ID3DBlob* vsBlob = compileShader(g_vsSource, "vs_5_0");
    ID3DBlob* psBlob = compileShader(g_psSource, "ps_5_0");
    if (!vsBlob || !psBlob) return 1;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* inputLayout = nullptr;
    device->CreateInputLayout(inputDesc, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
    vsBlob->Release();
    psBlob->Release();

    ctx->IASetInputLayout(inputLayout);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);

    // Unit triangle (centered at origin, will be transformed by constant buffer)
    Vertex vertices[] = {
        {  0.0f, -0.4f, 0.5f },
        { -0.35f, 0.3f, 0.5f },
        {  0.35f, 0.3f, 0.5f },
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

    // Constant buffer
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(ObjConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* objCB = nullptr;
    device->CreateBuffer(&cbd, nullptr, &objCB);
    ctx->VSSetConstantBuffers(0, 1, &objCB);

    // Blend state for alpha blending
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* blendOn = nullptr;
    device->CreateBlendState(&blendDesc, &blendOn);

    // No-blend state (default)
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    ID3D11BlendState* blendOff = nullptr;
    device->CreateBlendState(&blendDesc, &blendOff);

    // Per-object data: offset, scale, color (RGBA)
    ObjConstants objects[] = {
        // 3 opaque triangles at different positions
        { -0.5f, -0.3f, 0.7f, 0,  1.0f, 0.2f, 0.2f, 1.0f },  // Red, top-left
        {  0.5f, -0.3f, 0.7f, 0,  0.2f, 0.8f, 0.2f, 1.0f },  // Green, top-right
        {  0.0f,  0.4f, 0.7f, 0,  0.2f, 0.3f, 1.0f, 1.0f },  // Blue, bottom-center
        // 1 semi-transparent triangle overlapping all three
        {  0.0f,  0.0f, 1.0f, 0,  1.0f, 1.0f, 0.0f, 0.5f },  // Yellow, 50% alpha, centered
    };
    const int NUM_OBJECTS = 4;

    fprintf(stderr, "[Multi+Blend] Setup complete. %d objects per frame.\n", NUM_OBJECTS);
    fprintf(stderr, "[Multi+Blend] Expected: 3 opaque triangles + 1 semi-transparent yellow overlay.\n");

    MSG msg{};
    uint32_t frameCount = 0;
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        float clearColor[4] = { 0.15f, 0.15f, 0.15f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clearColor);

        for (int obj = 0; obj < NUM_OBJECTS; obj++) {
            // Switch blend state between opaque and translucent objects
            if (objects[obj].a < 1.0f)
                ctx->OMSetBlendState(blendOn, nullptr, 0xFFFFFFFF);
            else
                ctx->OMSetBlendState(blendOff, nullptr, 0xFFFFFFFF);

            D3D11_MAPPED_SUBRESOURCE mapped;
            ctx->Map(objCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            memcpy(mapped.pData, &objects[obj], sizeof(ObjConstants));
            ctx->Unmap(objCB, 0);

            ctx->Draw(3, 0);
        }

        swapchain->Present(1, 0);

        frameCount++;
        if (frameCount % 300 == 0)
            fprintf(stderr, "[Multi+Blend] %u frames rendered.\n", frameCount);
    }

    blendOn->Release();
    blendOff->Release();
    objCB->Release();
    vb->Release();
    inputLayout->Release();
    vs->Release();
    ps->Release();
    rtv->Release();
    swapchain->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    fprintf(stderr, "[Multi+Blend] Done. Total frames: %u\n", frameCount);
    return 0;
}

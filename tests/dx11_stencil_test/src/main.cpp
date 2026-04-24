// DX11 Stencil Test — VBox GPU Bridge
// Validates: stencil buffer + non-standard resolution (1280x720).
//
// Pass 1: Draw a green triangle with stencil REPLACE (ref=1).
//         Color write disabled — only writes stencil.
// Pass 2: Draw a full-screen red quad with stencil EQUAL (ref=1).
//         Only pixels inside the triangle's stencil mask are visible.
//
// Expected result: red triangle shape on dark background at 1280x720.
// If stencil is broken: either full red quad or nothing.
// If resolution is broken: image appears stretched/corrupted/clipped.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static const uint32_t WIDTH = 1280, HEIGHT = 720;
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
    fprintf(stderr, "[Stencil Test] Starting at %ux%u...\n", WIDTH, HEIGHT);

    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "DX11StencilTest";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "DX11 Stencil Test - 1280x720 - VBox GPU Bridge",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // --- Device + SwapChain ---
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
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapchain, &device, &featureLevel, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[Stencil Test] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 1;
    }
    fprintf(stderr, "[Stencil Test] Device created. Feature level: 0x%X\n", featureLevel);

    // Setup debug info queue
    ID3D11InfoQueue* infoQueue = nullptr;
    device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&infoQueue);
    if (infoQueue) {
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    }

    // --- Render target ---
    ID3D11Texture2D* backBuffer = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();

    // --- Depth-Stencil buffer (D24_UNORM_S8_UINT for stencil support) ---
    D3D11_TEXTURE2D_DESC dsTexDesc{};
    dsTexDesc.Width = WIDTH;
    dsTexDesc.Height = HEIGHT;
    dsTexDesc.MipLevels = 1;
    dsTexDesc.ArraySize = 1;
    dsTexDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsTexDesc.SampleDesc.Count = 1;
    dsTexDesc.Usage = D3D11_USAGE_DEFAULT;
    dsTexDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* dsTex = nullptr;
    hr = device->CreateTexture2D(&dsTexDesc, nullptr, &dsTex);
    fprintf(stderr, "[Stencil Test] CreateDepthStencilTexture (D24S8): hr=0x%08X\n", hr);

    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = device->CreateDepthStencilView(dsTex, &dsvDesc, &dsv);
    fprintf(stderr, "[Stencil Test] CreateDSV: hr=0x%08X\n", hr);

    ctx->OMSetRenderTargets(1, &rtv, dsv);

    // --- Stencil state: Pass 1 — write stencil, no color ---
    D3D11_DEPTH_STENCIL_DESC dsDescWrite{};
    dsDescWrite.DepthEnable = FALSE;
    dsDescWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDescWrite.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsDescWrite.StencilEnable = TRUE;
    dsDescWrite.StencilReadMask = 0xFF;
    dsDescWrite.StencilWriteMask = 0xFF;
    dsDescWrite.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dsDescWrite.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dsDescWrite.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dsDescWrite.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dsDescWrite.BackFace = dsDescWrite.FrontFace;
    ID3D11DepthStencilState* stencilWrite = nullptr;
    device->CreateDepthStencilState(&dsDescWrite, &stencilWrite);

    // --- Stencil state: Pass 2 — test stencil, draw color ---
    D3D11_DEPTH_STENCIL_DESC dsDescTest{};
    dsDescTest.DepthEnable = FALSE;
    dsDescTest.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDescTest.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsDescTest.StencilEnable = TRUE;
    dsDescTest.StencilReadMask = 0xFF;
    dsDescTest.StencilWriteMask = 0x00;
    dsDescTest.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    dsDescTest.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dsDescTest.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dsDescTest.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dsDescTest.BackFace = dsDescTest.FrontFace;
    ID3D11DepthStencilState* stencilTest = nullptr;
    device->CreateDepthStencilState(&dsDescTest, &stencilTest);

    // --- Blend state: no color write (for stencil-only pass) ---
    D3D11_BLEND_DESC blendNoColor{};
    blendNoColor.RenderTarget[0].RenderTargetWriteMask = 0; // disable color write
    ID3D11BlendState* bsNoColor = nullptr;
    device->CreateBlendState(&blendNoColor, &bsNoColor);

    // --- Viewport ---
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0, 1 };
    ctx->RSSetViewports(1, &vp);

    // --- Shaders ---
    ID3DBlob* vsBlob = compileShader(g_vsSource, "vs_5_0");
    ID3DBlob* psBlob = compileShader(g_psSource, "ps_5_0");
    if (!vsBlob || !psBlob) {
        fprintf(stderr, "[Stencil Test] Shader compilation failed.\n");
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
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- Geometry ---
    // Triangle (stencil mask shape) — CW winding for D3D11 front face
    Vertex triVerts[] = {
        {  0.0f,  0.6f, 0.5f,  0.0f, 1.0f, 0.0f },
        {  0.6f, -0.4f, 0.5f,  0.0f, 1.0f, 0.0f },
        { -0.6f, -0.4f, 0.5f,  0.0f, 1.0f, 0.0f },
    };
    // Full-screen quad (two triangles)
    Vertex quadVerts[] = {
        { -1.0f,  1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
        {  1.0f,  1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
        { -1.0f, -1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
        {  1.0f,  1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
        {  1.0f, -1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
        { -1.0f, -1.0f, 0.5f,  1.0f, 0.2f, 0.2f },
    };

    D3D11_BUFFER_DESC vbd{};
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    vbd.ByteWidth = sizeof(triVerts);
    D3D11_SUBRESOURCE_DATA triData{triVerts};
    ID3D11Buffer* triVB = nullptr;
    device->CreateBuffer(&vbd, &triData, &triVB);

    vbd.ByteWidth = sizeof(quadVerts);
    D3D11_SUBRESOURCE_DATA quadData{quadVerts};
    ID3D11Buffer* quadVB = nullptr;
    device->CreateBuffer(&vbd, &quadData, &quadVB);

    UINT stride = sizeof(Vertex), offset = 0;

    fprintf(stderr, "[Stencil Test] Setup complete.\n");
    fprintf(stderr, "[Stencil Test] Expected: red triangle shape on dark bg at 1280x720.\n");
    fprintf(stderr, "[Stencil Test]   If stencil broken: full red quad or black screen.\n");
    fprintf(stderr, "[Stencil Test]   If resolution broken: stretched/clipped/corrupt.\n");

    MSG msg{};
    uint32_t frameCount = 0;
    float defaultBlend[4] = {0, 0, 0, 0};

    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        float clearColor[4] = { 0.05f, 0.05f, 0.1f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clearColor);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtv, dsv);

        // --- Pass 1: Write stencil mask (triangle shape) ---
        ctx->OMSetDepthStencilState(stencilWrite, 1);  // ref=1
        ctx->OMSetBlendState(bsNoColor, defaultBlend, 0xFFFFFFFF);  // no color write
        ctx->IASetVertexBuffers(0, 1, &triVB, &stride, &offset);
        ctx->Draw(3, 0);  // draw triangle — writes stencil=1

        // --- Pass 2: Draw red quad, masked by stencil ---
        ctx->OMSetDepthStencilState(stencilTest, 1);   // ref=1, test EQUAL
        ctx->OMSetBlendState(nullptr, defaultBlend, 0xFFFFFFFF);  // normal color write
        ctx->IASetVertexBuffers(0, 1, &quadVB, &stride, &offset);
        ctx->Draw(6, 0);  // red quad — only visible where stencil==1

        swapchain->Present(1, 0);

        // Print D3D11 debug messages (first frame only)
        frameCount++;
        if (frameCount == 1 && infoQueue) {
            UINT64 msgCount = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < msgCount && i < 10; i++) {
                SIZE_T len = 0;
                infoQueue->GetMessage(i, nullptr, &len);
                auto* msg = (D3D11_MESSAGE*)malloc(len);
                infoQueue->GetMessage(i, msg, &len);
                fprintf(stderr, "[D3D11] %s\n", msg->pDescription);
                free(msg);
            }
            if (msgCount > 10) fprintf(stderr, "[D3D11] ... %llu more messages\n", msgCount - 10);
            infoQueue->ClearStoredMessages();
        }
        if (frameCount % 300 == 0)
            fprintf(stderr, "[Stencil Test] %u frames rendered.\n", frameCount);
    }

    // Cleanup
    stencilWrite->Release();
    stencilTest->Release();
    bsNoColor->Release();
    dsv->Release();
    dsTex->Release();
    triVB->Release();
    quadVB->Release();
    inputLayout->Release();
    vs->Release();
    ps->Release();
    rtv->Release();
    swapchain->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    fprintf(stderr, "[Stencil Test] Done. Total frames: %u\n", frameCount);
    return 0;
}

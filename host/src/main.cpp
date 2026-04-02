#include "vk_renderer.h"

static constexpr uint32_t WINDOW_WIDTH = 800;
static constexpr uint32_t WINDOW_HEIGHT = 600;
static bool g_running = true;

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Register window class
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUBridgeHost";
    RegisterClassExA(&wc);

    // Create window
    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "VBox GPU Bridge - Host Renderer",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, nCmdShow);

    // Init Vulkan
    VulkanContext vk{};
    PipelineContext pip{};
    RendererContext ren{};

    try {
        createInstance(vk);
        createSurface(vk, hwnd, hInstance);
        pickPhysicalDevice(vk);
        createLogicalDevice(vk);
        createSwapchain(vk, WINDOW_WIDTH, WINDOW_HEIGHT);
        createImageViews(vk);
        createRenderPass(vk, pip);
        createGraphicsPipeline(vk, pip);
        createFramebuffers(vk, pip);
        createCommandPool(vk, ren);
        createCommandBuffers(vk, pip, ren);
        createSyncObjects(vk, ren);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Vulkan Init Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Main loop
    MSG msg{};
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        try {
            drawFrame(vk, pip, ren);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "Render Error", MB_OK | MB_ICONERROR);
            break;
        }
    }

    vkDeviceWaitIdle(vk.device);

    // Cleanup
    cleanupRenderer(vk.device, ren);
    cleanupPipeline(vk.device, pip);
    cleanupVulkan(vk);

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}

// Host TCP server: receives Venus command streams from Guest, decodes and renders.

#include "vn_decoder.h"
#include "../../common/transport/tcp_transport.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>

static constexpr uint32_t WINDOW_WIDTH = 800;
static constexpr uint32_t WINDOW_HEIGHT = 600;
static std::atomic<bool> g_running{true};

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

int main(int argc, char* argv[]) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) port = static_cast<uint16_t>(atoi(argv[1]));

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // --- Create window ---
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUBridgeServer";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Host Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // --- Init Vulkan ---
    VulkanContext vk{};
    try {
        createInstance(vk);
        createSurface(vk, hwnd, hInstance);
        pickPhysicalDevice(vk);
        createLogicalDevice(vk);
    } catch (const std::exception& e) {
        fprintf(stderr, "Vulkan Init Error: %s\n", e.what());
        return 1;
    }

    VnDecoder decoder;
    decoder.init(vk.physicalDevice, vk.device, vk.graphicsQueue, vk.graphicsFamily, vk.surface);

    // --- Start TCP server ---
    TcpReceiver server;
    if (!server.listen(port)) {
        fprintf(stderr, "Failed to start TCP server on port %u\n", port);
        return 1;
    }
    if (!server.accept()) {
        fprintf(stderr, "Failed to accept connection\n");
        return 1;
    }

    fprintf(stderr, "[Host] Connection established. Processing command streams.\n");

    // --- Receive and process command streams ---
    constexpr size_t BUF_SIZE = 4 * 1024 * 1024; // 4 MB max per message
    std::vector<uint8_t> recvBuf(BUF_SIZE);

    while (g_running) {
        // Pump window messages
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        // Receive a command stream message
        size_t bytesRead = 0;
        if (!server.recv(recvBuf.data(), BUF_SIZE, bytesRead)) {
            fprintf(stderr, "[Host] Client disconnected.\n");
            break;
        }

        // Execute the command stream
        if (!decoder.execute(recvBuf.data(), bytesRead)) {
            fprintf(stderr, "[Host] Command stream execution failed.\n");
            break;
        }

        // Send back current swapchain image index (for next frame's framebuffer selection)
        auto* sc = decoder.getSwapchain(3); // H_SWAPCHAIN = 3
        uint32_t imageIndex = sc ? sc->currentImageIndex : 0;
        server.send(reinterpret_cast<const uint8_t*>(&imageIndex), sizeof(imageIndex));
    }

    // --- Cleanup ---
    decoder.cleanup();
    cleanupVulkan(vk);
    server.close();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    fprintf(stderr, "[Host] Shutdown complete.\n");
    return 0;
}

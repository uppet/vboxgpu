// Host renderer driven by Venus command stream.
// Builds a command stream encoding a triangle render, then decodes and executes it.

#include "vn_decoder.h"
#include "../../common/venus/vn_encoder.h"
#include <fstream>
#include <string>
#include <cstdio>

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
        if (wParam == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static std::vector<char> readShaderFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Cannot open shader: " + path);
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

// Handle IDs used in the command stream
enum HandleIds : uint64_t {
    H_DEVICE       = 1,
    H_QUEUE        = 2,
    H_SWAPCHAIN    = 3,
    H_RENDER_PASS  = 10,
    H_VERT_SHADER  = 11,
    H_FRAG_SHADER  = 12,
    H_PIPE_LAYOUT  = 13,
    H_PIPELINE     = 14,
    H_CMD_POOL     = 20,
    H_CMD_BUF      = 21,
    H_SEM_IMAGE    = 30,
    H_SEM_RENDER   = 31,
    H_FENCE        = 32,
    // Framebuffers: 100+ (one per swapchain image)
    H_FB_BASE      = 100,
};

// Build the "setup" command stream: creates all GPU resources once.
static std::vector<uint8_t> buildSetupStream(const char* shaderDir) {
    VnEncoder enc;

    // Load SPIR-V shaders
    auto vertSpv = readShaderFile(std::string(shaderDir) + "/triangle.vert.spv");
    auto fragSpv = readShaderFile(std::string(shaderDir) + "/triangle.frag.spv");

    // Create swapchain (bridge-specific, host manages it)
    enc.cmdBridgeCreateSwapchain(H_DEVICE, H_SWAPCHAIN, WINDOW_WIDTH, WINDOW_HEIGHT, 3);

    // Create render pass
    uint32_t format = VK_FORMAT_B8G8R8A8_SRGB;
    uint32_t loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    uint32_t storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    enc.cmdCreateRenderPass(H_DEVICE, H_RENDER_PASS, 1,
                            &format, &loadOp, &storeOp, &initialLayout, &finalLayout);

    // Create shaders
    enc.cmdCreateShaderModule(H_DEVICE, H_VERT_SHADER,
                              reinterpret_cast<const uint32_t*>(vertSpv.data()), vertSpv.size());
    enc.cmdCreateShaderModule(H_DEVICE, H_FRAG_SHADER,
                              reinterpret_cast<const uint32_t*>(fragSpv.data()), fragSpv.size());

    // Create pipeline
    enc.cmdCreatePipelineLayout(H_DEVICE, H_PIPE_LAYOUT);
    enc.cmdCreateGraphicsPipeline(H_DEVICE, H_PIPELINE, H_RENDER_PASS, H_PIPE_LAYOUT,
                                  H_VERT_SHADER, H_FRAG_SHADER, WINDOW_WIDTH, WINDOW_HEIGHT,
                                  0); // 0 = use renderPass

    // Create framebuffers for each swapchain image
    // Image view IDs are assigned by decoder as: swapchainId*100 + i + 1
    for (uint32_t i = 0; i < 3; i++) {
        uint64_t imageViewId = H_SWAPCHAIN * 100 + i + 1; // = 301, 302, 303
        enc.cmdCreateFramebuffer(H_DEVICE, H_FB_BASE + i, H_RENDER_PASS,
                                 imageViewId, WINDOW_WIDTH, WINDOW_HEIGHT);
    }

    // Create command pool + buffer
    enc.cmdCreateCommandPool(H_DEVICE, H_CMD_POOL, 0);
    enc.cmdAllocateCommandBuffers(H_DEVICE, H_CMD_POOL, H_CMD_BUF);

    // Create sync objects
    enc.cmdCreateSemaphore(H_DEVICE, H_SEM_IMAGE);
    enc.cmdCreateSemaphore(H_DEVICE, H_SEM_RENDER);
    enc.cmdCreateFence(H_DEVICE, H_FENCE, VK_FENCE_CREATE_SIGNALED_BIT);

    enc.cmdEndOfStream();

    auto* d = enc.data();
    return std::vector<uint8_t>(d, d + enc.size());
}

// Build a per-frame command stream.
// fbId must correspond to the acquired swapchain image.
static std::vector<uint8_t> buildFrameStream(uint64_t fbId) {
    VnEncoder enc;

    enc.cmdWaitForFences(H_DEVICE, H_FENCE);
    enc.cmdResetFences(H_DEVICE, H_FENCE);

    enc.cmdBeginCommandBuffer(H_CMD_BUF);
    enc.cmdBeginRenderPass(H_CMD_BUF, H_RENDER_PASS, fbId,
                           WINDOW_WIDTH, WINDOW_HEIGHT,
                           0.0f, 0.0f, 0.0f, 1.0f);
    enc.cmdBindPipeline(H_CMD_BUF, H_PIPELINE);
    enc.cmdDraw(H_CMD_BUF, 3, 1, 0, 0);
    enc.cmdEndRenderPass(H_CMD_BUF);
    enc.cmdEndCommandBuffer(H_CMD_BUF);

    enc.cmdQueueSubmit(H_QUEUE, H_CMD_BUF, H_SEM_IMAGE, H_SEM_RENDER, H_FENCE);
    enc.cmdBridgeQueuePresent(H_QUEUE, H_SWAPCHAIN, H_SEM_RENDER);

    enc.cmdEndOfStream();

    auto* d = enc.data();
    return std::vector<uint8_t>(d, d + enc.size());
}

int main(int argc, char* argv[]) {
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // --- Create window ---
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUBridgeCmdStream";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Command Stream Mode",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // --- Minimal Vulkan bootstrap (instance + device only) ---
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

    // --- Init decoder ---
    VnDecoder decoder;
    decoder.init(vk.physicalDevice, vk.device, vk.graphicsQueue, vk.graphicsFamily, vk.surface);

    // --- Execute setup stream ---
    try {
        auto setupStream = buildSetupStream(SHADER_DIR);
        if (!decoder.execute(setupStream.data(), setupStream.size())) {
            fprintf(stderr, "Setup stream execution failed\n");
            return 1;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Setup Error: %s\n", e.what());
        return 1;
    }

    fprintf(stderr, "[VBoxGPU] Setup complete. Entering render loop.\n");

    // --- Main loop: build & execute per-frame command streams ---
    MSG msg{};
    while (g_running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        auto* sc = decoder.getSwapchain(H_SWAPCHAIN);
        if (!sc) break;

        // Acquire next image directly (outside command stream for correct ordering)
        VkSemaphore imgSem = decoder.lookupSemaphore(H_SEM_IMAGE);
        vkAcquireNextImageKHR(vk.device, sc->swapchain, UINT64_MAX,
                              imgSem, VK_NULL_HANDLE, &sc->currentImageIndex);

        uint64_t fbId = H_FB_BASE + sc->currentImageIndex;
        auto frameStream = buildFrameStream(fbId);
        if (!decoder.execute(frameStream.data(), frameStream.size()))
            break;
    }

    // --- Cleanup ---
    decoder.cleanup();
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}

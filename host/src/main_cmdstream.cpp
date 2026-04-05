// Host renderer driven by Venus command stream.
// Builds a command stream encoding a triangle render, then decodes and executes it.

#include "vn_decoder.h"
#include "../../common/venus/vn_encoder.h"
#include <fstream>
#include <string>
#include <cstdio>
#include <cstring>

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

// --- Replay mode: read recorded command stream from dump file ---
struct DumpBatch {
    std::vector<uint8_t> data;
};

static std::vector<DumpBatch> loadDumpFile(const char* path) {
    std::vector<DumpBatch> batches;
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Replay] Cannot open dump file: %s\n", path);
        return batches;
    }
    while (true) {
        uint32_t sz;
        if (fread(&sz, sizeof(sz), 1, f) != 1) break;
        DumpBatch batch;
        batch.data.resize(sz);
        if (fread(batch.data.data(), 1, sz, f) != sz) break;
        batches.push_back(std::move(batch));
    }
    fclose(f);
    fprintf(stderr, "[Replay] Loaded %zu batches from %s\n", batches.size(), path);
    return batches;
}

static int replayMode(const char* dumpPath) {
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    auto batches = loadDumpFile(dumpPath);
    if (batches.empty()) {
        fprintf(stderr, "[Replay] No batches to replay.\n");
        return 1;
    }

    // --- Create window ---
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_HREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUReplay";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Replay Mode",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // --- Vulkan bootstrap ---
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

    // Execute setup batches (all batches before first Present)
    // Then loop the rendering batches
    size_t setupEnd = batches.size(); // default: replay all once
    // Find where the first Present happens — everything before is setup
    for (size_t i = 0; i < batches.size(); i++) {
        // Look for Present command (0x10002) in the batch
        if (batches[i].data.size() >= 8) {
            const uint8_t* p = batches[i].data.data();
            const uint8_t* end = p + batches[i].data.size();
            while (p + 8 <= end) {
                uint32_t cmd = *reinterpret_cast<const uint32_t*>(p);
                uint32_t sz = *reinterpret_cast<const uint32_t*>(p + 4);
                if (cmd == 0x10002) { // VN_CMD_vkBridgeQueuePresent
                    setupEnd = i + 1;
                    goto found_present;
                }
                if (p + 8 + sz > end) break;
                p += 8 + sz;
            }
        }
    }
found_present:

    fprintf(stderr, "[Replay] Setup batches: 0..%zu, Frame batches: %zu..%zu\n",
            setupEnd - 1, setupEnd, batches.size() - 1);

    // Execute setup batches once
    for (size_t i = 0; i < setupEnd; i++) {
        fprintf(stderr, "[Replay] Setup batch %zu (%zu bytes)\n", i, batches[i].data.size());
        if (!decoder.execute(batches[i].data.data(), batches[i].data.size())) {
            fprintf(stderr, "[Replay] Setup batch %zu failed\n", i);
            return 1;
        }
    }

    fprintf(stderr, "[Replay] Setup complete. Executing frame batches once.\n");

    // Execute frame batches once (DXVK command streams have complex CB lifecycle,
    // safe single-pass replay first)
    for (size_t i = setupEnd; i < batches.size() && g_running; i++) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        fprintf(stderr, "[Replay] Frame batch %zu (%zu bytes)\n", i, batches[i].data.size());
        if (!decoder.execute(batches[i].data.data(), batches[i].data.size())) {
            fprintf(stderr, "[Replay] Frame batch %zu failed\n", i);
            break;
        }
    }

    fprintf(stderr, "[Replay] All batches executed. Capturing screenshot.\n");
    // Use dump filename to derive screenshot path
    std::string ssPath = dumpPath;
    auto dotPos = ssPath.rfind('.');
    if (dotPos != std::string::npos) ssPath = ssPath.substr(0, dotPos);
    ssPath += "_screenshot.bmp";
    decoder.captureScreenshot(ssPath.c_str());
    fprintf(stderr, "[Replay] Keeping window open (press ESC to exit).\n");

    // Keep window open so user can see the result
    while (g_running) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(50);
    }

    decoder.cleanup();
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}

int main(int argc, char* argv[]) {
    // Check for --replay mode
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            return replayMode(argv[++i]);
        }
    }

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

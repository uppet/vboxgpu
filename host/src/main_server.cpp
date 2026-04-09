// Host TCP server: receives Venus command streams from Guest, decodes and renders.
// Also supports --replay <dump.bin> to replay a recorded command stream with screenshot.

#include "vn_decoder.h"
#include "win_capture.h"
#include "../../common/transport/tcp_transport.h"
#include <lz4.h>
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI hostCrashHandler(EXCEPTION_POINTERS* ep) {
    CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\host_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hFile);
        fprintf(stderr, "[Host] Crash dump: S:\\bld\\vboxgpu\\dumps\\host_crash.dmp\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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

// --- Replay mode ---
struct DumpBatch { std::vector<uint8_t> data; };

static std::vector<DumpBatch> loadDumpFile(const char* path) {
    std::vector<DumpBatch> batches;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[Replay] Cannot open: %s\n", path); return batches; }
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
    auto batches = loadDumpFile(dumpPath);
    if (batches.empty()) { fprintf(stderr, "[Replay] No batches.\n"); return 1; }

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUReplay";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Replay",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

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

    // Execute all batches once
    for (size_t i = 0; i < batches.size() && g_running; i++) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;
        if (!decoder.execute(batches[i].data.data(), batches[i].data.size())) {
            fprintf(stderr, "[Replay] Batch %zu failed\n", i);
            break;
        }
    }

    // Screenshot
    std::string ssPath = dumpPath;
    auto dotPos = ssPath.rfind('.');
    if (dotPos != std::string::npos) ssPath = ssPath.substr(0, dotPos);
    ssPath += "_screenshot.bmp";
    fprintf(stderr, "[Replay] All batches done. Capturing screenshot.\n");
    decoder.captureScreenshot(ssPath.c_str());

    fprintf(stderr, "[Replay] Keeping window open (ESC to exit).\n");
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
    SetUnhandledExceptionFilter(hostCrashHandler);
    setvbuf(stderr, NULL, _IONBF, 0);  // Force unbuffered stderr

    // Check --replay mode first
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            return replayMode(argv[++i]);
        }
    }

    uint16_t port = DEFAULT_PORT;
    const char* dumpPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else {
            port = static_cast<uint16_t>(atoi(argv[i]));
        }
    }
    FILE* dumpFile = nullptr;
    if (dumpPath) {
        dumpFile = fopen(dumpPath, "wb");
        if (!dumpFile) {
            fprintf(stderr, "[Host] Failed to open dump file: %s\n", dumpPath);
            return 1;
        }
        fprintf(stderr, "[Host] Dumping command stream to: %s\n", dumpPath);
    }

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

    // --- Quick test (DISABLED to avoid surface reuse issues): ---
#if 0 can the swapchain display ANYTHING? ---
    {
        // Create a minimal swapchain on the Host surface
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physicalDevice, vk.surface, &caps);
        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &fmtCount, fmts.data());

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = vk.surface;
        sci.minImageCount = 2;
        sci.imageFormat = fmts[0].format;
        sci.imageColorSpace = fmts[0].colorSpace;
        sci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{800,600};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;

        VkSwapchainKHR testSc;
        VkResult scr = vkCreateSwapchainKHR(vk.device, &sci, nullptr, &testSc);
        fprintf(stderr, "[SelfTest] Swapchain create: result=%d fmt=%u extent=%ux%u\n",
                (int)scr, fmts[0].format, sci.imageExtent.width, sci.imageExtent.height);

        if (scr == VK_SUCCESS) {
            uint32_t imgCount = 0;
            vkGetSwapchainImagesKHR(vk.device, testSc, &imgCount, nullptr);
            std::vector<VkImage> imgs(imgCount);
            vkGetSwapchainImagesKHR(vk.device, testSc, &imgCount, imgs.data());

            // Present RED on each image a few times
            VkCommandPool pool;
            VkCommandPoolCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = vk.graphicsFamily;
            vkCreateCommandPool(vk.device, &pci, nullptr, &pool);

            VkFence fence;
            VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vkCreateFence(vk.device, &fci, nullptr, &fence);

            for (int frame = 0; frame < 10; frame++) {
                uint32_t idx;
                vkAcquireNextImageKHR(vk.device, testSc, UINT64_MAX, VK_NULL_HANDLE, fence, &idx);
                vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(vk.device, 1, &fence);

                VkCommandBuffer cb;
                VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                vkAllocateCommandBuffers(vk.device, &ai, &cb);
                VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &bi);

                VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.image = imgs[idx]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,1,&b);

                VkClearColorValue red = {{1,0,0,1}};
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);

                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,nullptr,0,nullptr,1,&b);

                vkEndCommandBuffer(cb);
                VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                vkQueueSubmit(vk.graphicsQueue, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(vk.graphicsQueue);

                VkPresentInfoKHR pi{}; pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                pi.swapchainCount = 1; pi.pSwapchains = &testSc; pi.pImageIndices = &idx;
                VkResult pr = vkQueuePresentKHR(vk.graphicsQueue, &pi);
                if (frame == 0)
                    fprintf(stderr, "[SelfTest] Present frame %d imgIdx=%u result=%d\n", frame, idx, (int)pr);

                vkFreeCommandBuffers(vk.device, pool, 1, &cb);
                MSG msg{}; while (PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
                Sleep(30);
            }

            // WGC capture after self-test
            captureWindowToBMP(hwnd, "S:/bld/vboxgpu/selftest_capture.bmp");

            vkDestroyFence(vk.device, fence, nullptr);
            vkDestroyCommandPool(vk.device, pool, nullptr);
            vkDestroySwapchainKHR(vk.device, testSc, nullptr);
        }
    }
#endif

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

    // --- Worker thread: recv → decode → execute → send response ---
    // Runs on a separate thread so the main thread can pump window messages
    // without being blocked by vkWaitForFences or other long Vulkan operations.
    std::thread workerThread([&]() {
        constexpr size_t BUF_SIZE = 64 * 1024 * 1024; // 64 MB max per message
        std::vector<uint8_t> recvBuf(BUF_SIZE);
        // Persistent send buffer — avoids per-frame allocation
        std::vector<uint8_t> sendBuf;
        while (g_running) {
            // Receive a command stream message (blocking)
            size_t bytesRead = 0;
            if (!server.recv(recvBuf.data(), BUF_SIZE, bytesRead)) {
                fprintf(stderr, "[Host] Client disconnected.\n");
                g_running = false;
                PostMessageA(hwnd, WM_CLOSE, 0, 0); // wake main thread
                break;
            }

            fprintf(stderr, "[Host] Received %zu bytes\n", bytesRead);

            // Dump to file if recording
            if (dumpFile) {
                uint32_t sz = static_cast<uint32_t>(bytesRead);
                fwrite(&sz, sizeof(sz), 1, dumpFile);
                fwrite(recvBuf.data(), 1, bytesRead, dumpFile);
                fflush(dumpFile);
            }

            // Execute the command stream
            if (!decoder.execute(recvBuf.data(), bytesRead)) {
                fprintf(stderr, "[Host] Command stream execution failed.\n");
                g_running = false;
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
                break;
            }

            // Send response: [imageIndex(4)][width(4)][height(4)][compressedSize(4)][LZ4 data]
            auto* sc = decoder.getFirstSwapchain();
            uint32_t imageIndex = sc ? sc->currentImageIndex : 0;

            if (decoder.hasReadback()) {
                uint32_t w = decoder.getReadbackWidth();
                uint32_t h = decoder.getReadbackHeight();
                uint32_t rawSize = decoder.getReadbackSize();
                int maxCompressed = LZ4_compressBound(rawSize);

                // Ensure send buffer is large enough: 16-byte header + compressed data
                if (sendBuf.size() < 16 + (size_t)maxCompressed)
                    sendBuf.resize(16 + maxCompressed);

                int compressedSize = LZ4_compress_default(
                    static_cast<const char*>(decoder.getReadbackData()),
                    reinterpret_cast<char*>(sendBuf.data() + 16),
                    rawSize, maxCompressed);

                if (compressedSize > 0) {
                    uint32_t csz = static_cast<uint32_t>(compressedSize);
                    memcpy(sendBuf.data(),      &imageIndex, 4);
                    memcpy(sendBuf.data() + 4,  &w, 4);
                    memcpy(sendBuf.data() + 8,  &h, 4);
                    memcpy(sendBuf.data() + 12, &csz, 4);
                    server.send(sendBuf.data(), 16 + csz);

                    static int logCount = 0;
                    if (++logCount <= 3)
                        fprintf(stderr, "[Host] Frame %ux%u: %u -> %u bytes (%.1fx)\n",
                                w, h, rawSize, csz, (float)rawSize / csz);
                } else {
                    // Compression failed — send uncompressed as fallback
                    fprintf(stderr, "[Host] LZ4 compress failed, sending raw\n");
                    sendBuf.resize(16 + rawSize);
                    memcpy(sendBuf.data(),      &imageIndex, 4);
                    memcpy(sendBuf.data() + 4,  &w, 4);
                    memcpy(sendBuf.data() + 8,  &h, 4);
                    memcpy(sendBuf.data() + 12, &rawSize, 4);
                    memcpy(sendBuf.data() + 16, decoder.getReadbackData(), rawSize);
                    server.send(sendBuf.data(), 16 + rawSize);
                }
            } else {
                // No frame: send header with compressedSize=0
                uint8_t resp[16] = {};
                memcpy(resp, &imageIndex, 4);
                server.send(resp, sizeof(resp));
            }
        }
    });

    // --- Main thread: window message loop ---
    while (g_running) {
        MSG msg{};
        BOOL ret = GetMessageA(&msg, nullptr, 0, 0);
        if (ret == 0 || ret == -1) break; // WM_QUIT or error
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // --- Cleanup ---
    g_running = false;
    server.close(); // unblock recv() in worker thread
    if (workerThread.joinable()) workerThread.join();
    if (dumpFile) fclose(dumpFile);
    decoder.cleanup();
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    fprintf(stderr, "[Host] Shutdown complete.\n");
    return 0;
}

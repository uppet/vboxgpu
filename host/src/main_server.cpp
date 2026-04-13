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
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <csignal>
#include <crtdbg.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static void writeDump(EXCEPTION_POINTERS* ep) {
    CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\host_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithHandleData),
            ep ? &mei : NULL, NULL, NULL);
        CloseHandle(hFile);
        fprintf(stderr, "[Host] Crash dump: S:\\bld\\vboxgpu\\dumps\\host_crash.dmp\n");
        fflush(stderr);
    }
}

static LONG WINAPI hostCrashHandler(EXCEPTION_POINTERS* ep) {
    writeDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Catch abort() from assert failures — write dump without exception context
static void hostAbortHandler(int) {
    fprintf(stderr, "[Host] ABORT signal — writing crash dump\n"); fflush(stderr);
    writeDump(nullptr);
    _exit(99);
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

    // Execute all batches once, screenshot every 5 batches for debugging
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
        // Save screenshots at key batches
        if (i < 5 || i == 10 || i == 50 || i == 100 || i == 150) {
            char path[256];
            snprintf(path, sizeof(path), "%s_batch%zu.bmp", dumpPath, i);
            decoder.captureScreenshot(path);
            fprintf(stderr, "[Replay] Batch %zu done, screenshot: %s\n", i, path);
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
    signal(SIGABRT, hostAbortHandler);
    // Suppress assert dialog in Debug builds — go straight to abort handler
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
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

    // Check if frame readback is disabled (saves bandwidth for sortcourt etc.)
    bool disableReadback = (getenv("VBOXGPU_NO_READBACK") != nullptr);
    decoder.noReadback_ = disableReadback;
    if (disableReadback)
        fprintf(stderr, "[Host] Frame readback DISABLED (VBOXGPU_NO_READBACK)\n");

    // --- Async LZ4 compress thread ---
    // Decode thread posts raw frame pixels here; compress thread LZ4s them
    // in the background while the next frame's GPU work runs.
    // Adds ~1 frame display latency but removes ~3ms LZ4 from the critical path.

    struct CompressJob {
        std::vector<uint8_t> rawData;
        uint32_t w = 0, h = 0;
        bool valid = false;
        // Frame-level timing passthrough
        uint32_t frameId = 0;
        uint64_t presentUs = 0;
        uint64_t readbackUs = 0;
    };
    struct CompressResult {
        std::vector<uint8_t> compData;
        uint32_t w = 0, h = 0, rawSize = 0, compSize = 0;
        bool valid = false;
        // Frame-level timing passthrough
        uint32_t frameId = 0;
        uint64_t presentUs = 0;
        uint64_t readbackUs = 0;
        uint64_t compressDoneUs = 0;
    };

    std::mutex          cjMutex;   // guards compJob
    std::condition_variable cjCV;
    CompressJob         compJob;

    std::mutex          crMutex;   // guards compResult
    CompressResult      compResult;

    std::atomic<bool>   compRunning{true};

    std::thread compressThread([&]() {
        while (compRunning) {
            std::unique_lock<std::mutex> lock(cjMutex);
            cjCV.wait(lock, [&] { return compJob.valid || !compRunning; });
            if (!compRunning) break;

            uint32_t w = compJob.w, h = compJob.h;
            uint32_t jobFrameId = compJob.frameId;
            uint64_t jobPresentUs = compJob.presentUs;
            uint64_t jobReadbackUs = compJob.readbackUs;
            std::vector<uint8_t> localRaw = std::move(compJob.rawData);
            compJob.valid = false;
            lock.unlock(); // release lock before heavy LZ4 work

            uint32_t rawSize = static_cast<uint32_t>(localRaw.size());
            int bound = LZ4_compressBound(rawSize);
            std::vector<uint8_t> compressed(bound);
            int csz = LZ4_compress_default(
                reinterpret_cast<const char*>(localRaw.data()),
                reinterpret_cast<char*>(compressed.data()),
                rawSize, bound);

            if (csz > 0) {
                compressed.resize(csz);
                std::lock_guard<std::mutex> rlock(crMutex);
                compResult.compData      = std::move(compressed);
                compResult.w             = w;
                compResult.h             = h;
                compResult.rawSize       = rawSize;
                compResult.compSize      = static_cast<uint32_t>(csz);
                compResult.valid         = true;
                compResult.frameId       = jobFrameId;
                compResult.presentUs     = jobPresentUs;
                compResult.readbackUs    = jobReadbackUs;
                compResult.compressDoneUs = rtNowUs();
            } else {
                fprintf(stderr, "[Compress] LZ4 failed for %ux%u\n", w, h);
            }
        }
    });

    // --- Worker thread: recv → decode → execute → send response ---
    // Runs on a separate thread so the main thread can pump window messages
    // without being blocked by vkWaitForFences or other long Vulkan operations.

    // FPS counter
    int fpsFrameCount = 0;
    auto fpsLastTime = std::chrono::steady_clock::now();

    std::thread workerThread([&]() {
        constexpr size_t BUF_SIZE = 256 * 1024 * 1024; // 256 MB max per message
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

#if VBOXGPU_TIMING
            decoder.batchRecvUs_ = rtNowUs();
            RT_LOG(0, "T2", "recv %zu bytes", bytesRead);
#endif
#ifdef VBOXGPU_VERBOSE
            fprintf(stderr, "[Host] Received %zu bytes\n", bytesRead);
#endif

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

            // Post raw frame pixels to compress thread (fast memcpy, non-blocking).
            // Compress thread will LZ4 in background while next frame renders on GPU.
            if (!disableReadback && decoder.hasReadback()) {
                auto* rawPtr = static_cast<const uint8_t*>(decoder.getReadbackData());
                uint32_t rawSize = decoder.getReadbackSize();
                {
                    std::lock_guard<std::mutex> lock(cjMutex);
                    compJob.rawData.assign(rawPtr, rawPtr + rawSize);
                    compJob.w = decoder.getReadbackWidth();
                    compJob.h = decoder.getReadbackHeight();
                    compJob.valid = true;
#if VBOXGPU_TIMING
                    compJob.frameId   = decoder.readyFrameTiming_.frameId;
                    compJob.presentUs = decoder.readyFrameTiming_.presentUs;
                    compJob.readbackUs = decoder.readyFrameTiming_.readbackUs;
#endif
                }
                cjCV.notify_one();
            }

            // Take whatever compressed result the compress thread has ready (non-blocking).
            // Will be the frame from one iteration ago — acceptable 1-frame display lag.
            CompressResult result;
            if (!disableReadback) {
                std::lock_guard<std::mutex> lock(crMutex);
                if (compResult.valid) {
                    result = std::move(compResult);
                    compResult.valid = false;
                }
            }

            // Build response: [header(16)][LZ4 frame][bdaCount(4)][{bufId(8),addr(8)}*N]
            auto* sc = decoder.getFirstSwapchain();
            uint32_t imageIndex = sc ? sc->currentImageIndex : 0;
            size_t payloadSize = 16; // minimum: 16-byte header

            // FPS counter — print once per second
            fpsFrameCount++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fpsLastTime).count();
            if (elapsed >= 1000) {
                fprintf(stderr, "[Host] FPS: %.1f\n", fpsFrameCount * 1000.0 / elapsed);
                fpsFrameCount = 0;
                fpsLastTime = now;
            }

            size_t bdaBytes = 4 + decoder.pendingBdaResults_.size() * 16;
            // Timing suffix: batch(20) + frame(28) = 48 bytes
            // Batch: [seqId(4)][recvUs(8)][sendUs(8)]
            // Frame: [frameId(4)][presentUs(8)][readbackUs(8)][compressDoneUs(8)]
            constexpr size_t TIMING_BYTES = 48;
            if (result.valid) {
                uint32_t csz = result.compSize;
                if (sendBuf.size() < 16 + csz + bdaBytes + TIMING_BYTES)
                    sendBuf.resize(16 + csz + bdaBytes + TIMING_BYTES);
                memcpy(sendBuf.data(),      &imageIndex,    4);
                memcpy(sendBuf.data() + 4,  &result.w,      4);
                memcpy(sendBuf.data() + 8,  &result.h,      4);
                memcpy(sendBuf.data() + 12, &csz,           4);
                memcpy(sendBuf.data() + 16, result.compData.data(), csz);
                payloadSize = 16 + csz;

                static int logCount = 0;
                if (++logCount <= 3)
                    fprintf(stderr, "[Host] Frame %ux%u: %u -> %u bytes (%.1fx)\n",
                            result.w, result.h, result.rawSize, csz,
                            (float)result.rawSize / csz);
            } else {
                if (sendBuf.size() < 16 + bdaBytes + TIMING_BYTES)
                    sendBuf.resize(16 + bdaBytes + TIMING_BYTES);
                memset(sendBuf.data(), 0, 16);
                memcpy(sendBuf.data(), &imageIndex, 4);
                payloadSize = 16;
            }

            // Append BDA query results
            uint32_t bdaCount = static_cast<uint32_t>(decoder.pendingBdaResults_.size());
            memcpy(sendBuf.data() + payloadSize, &bdaCount, 4);
            for (uint32_t i = 0; i < bdaCount; i++) {
                memcpy(sendBuf.data() + payloadSize + 4 + i * 16,
                       &decoder.pendingBdaResults_[i].bufferId, 8);
                memcpy(sendBuf.data() + payloadSize + 4 + i * 16 + 8,
                       &decoder.pendingBdaResults_[i].address, 8);
            }
            payloadSize += 4 + bdaCount * 16;

#if VBOXGPU_TIMING
            // Append timing suffix: batch(20) + frame(28) = 48 bytes
            uint64_t hostSendUs = rtNowUs();
            if (sendBuf.size() < payloadSize + TIMING_BYTES)
                sendBuf.resize(payloadSize + TIMING_BYTES);
            // Batch timing: [seqId(4)][recvUs(8)][sendUs(8)]
            size_t tp = payloadSize;
            memcpy(sendBuf.data() + tp,      &decoder.currentSeqId_, 4);
            memcpy(sendBuf.data() + tp + 4,  &decoder.batchRecvUs_,  8);
            memcpy(sendBuf.data() + tp + 12, &hostSendUs,            8);
            // Frame timing: [frameId(4)][presentUs(8)][readbackUs(8)][compressDoneUs(8)]
            uint32_t ftFrameId = result.valid ? result.frameId : 0;
            uint64_t ftPresentUs = result.valid ? result.presentUs : 0;
            uint64_t ftReadbackUs = result.valid ? result.readbackUs : 0;
            uint64_t ftCompressUs = result.valid ? result.compressDoneUs : 0;
            memcpy(sendBuf.data() + tp + 20, &ftFrameId,     4);
            memcpy(sendBuf.data() + tp + 24, &ftPresentUs,   8);
            memcpy(sendBuf.data() + tp + 32, &ftReadbackUs,  8);
            memcpy(sendBuf.data() + tp + 40, &ftCompressUs,  8);
            payloadSize += TIMING_BYTES;
            RT_LOG(decoder.currentSeqId_, "T6", "send %zu bytes, host=%.2fms frame=%u",
                   payloadSize, (hostSendUs - decoder.batchRecvUs_) / 1000.0, ftFrameId);
#endif

            server.send(sendBuf.data(), payloadSize);
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
    // Stop compress thread
    compRunning = false;
    cjCV.notify_all();
    if (compressThread.joinable()) compressThread.join();
    if (dumpFile) fclose(dumpFile);
    decoder.cleanup();
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    fprintf(stderr, "[Host] Shutdown complete.\n");
    return 0;
}

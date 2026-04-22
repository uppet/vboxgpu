#include "client_session.h"
#include <lz4.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <cstdio>
#include <cstring>
#include <chrono>


#include "../../common/timing.h"

// TCP helpers — reuse the static inline functions from tcp_transport.h
// (already included via the header chain)

ClientSession::ClientSession(int id, VkPhysicalDevice physDevice, VkInstance instance,
                             HINSTANCE hInstance)
    : id_(id), physDevice_(physDevice), instance_(instance), hInstance_(hInstance) {}

ClientSession::~ClientSession() {
    join();
    destroySessionWindow();
}

void ClientSession::createSessionWindow() {
    char className[64];
    snprintf(className, sizeof(className), "VBoxGPUSession%d", id_);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassExA(&wc);

    char title[128];
    snprintf(title, sizeof(title), "VBox GPU Bridge - Client %d", id_);

    RECT rect = { 0, 0, 800, 600 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExA(WS_EX_TOOLWINDOW, className, title,
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top,
                            nullptr, nullptr, hInstance_, nullptr);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    // Ensure not cloaked by default
    BOOL uncloak = FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
}

void ClientSession::destroySessionWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void ClientSession::start(SOCKET clientSock, const char* dumpDir) {
    clientSock_ = clientSock;
    if (dumpDir) {
        char path[512];
        snprintf(path, sizeof(path), "%s\\session_%d.bin", dumpDir, id_);
        dumpFile_ = fopen(path, "wb");
        if (dumpFile_)
            fprintf(stderr, "[Session %d] Recording to %s\n", id_, path);
    }
    replay_ = false;
    running_ = true;
    compRunning_ = true;
    compressThread_ = std::thread(&ClientSession::compressLoop, this);
    workerThread_ = std::thread(&ClientSession::workerLoop, this);
    fprintf(stderr, "[Session %d] Started (live%s)\n", id_, dumpFile_ ? ", recording" : "");
}

void ClientSession::startReplay(std::vector<ReplayBatch> batches, const char* saveFramesDir) {
    replay_ = true;
    replayBatches_ = std::move(batches);
    saveFramesDir_ = saveFramesDir;
    running_ = true;
    compRunning_ = true;
    compressThread_ = std::thread(&ClientSession::compressLoop, this);
    workerThread_ = std::thread(&ClientSession::replayLoop, this);
    fprintf(stderr, "[Session %d] Started (replay, %zu batches)\n", id_, replayBatches_.size());
}

void ClientSession::join() {
    if (workerThread_.joinable()) workerThread_.join();
    compRunning_ = false;
    cjCV_.notify_all();
    if (compressThread_.joinable()) compressThread_.join();
}

void ClientSession::compressLoop() {
    while (compRunning_) {
        std::unique_lock<std::mutex> lock(cjMutex_);
        cjCV_.wait(lock, [&] { return compJob_.valid || !compRunning_; });
        if (!compRunning_) break;

        uint32_t w = compJob_.w, h = compJob_.h;
        uint32_t jobFrameId = compJob_.frameId;
        uint64_t jobPresentUs = compJob_.presentUs;
        uint64_t jobReadbackUs = compJob_.readbackUs;
        std::vector<uint8_t> localRaw = std::move(compJob_.rawData);
        compJob_.valid = false;
        lock.unlock();

        uint32_t rawSize = static_cast<uint32_t>(localRaw.size());
        int bound = LZ4_compressBound(rawSize);
        std::vector<uint8_t> compressed(bound);
        int csz = LZ4_compress_default(
            reinterpret_cast<const char*>(localRaw.data()),
            reinterpret_cast<char*>(compressed.data()),
            rawSize, bound);

        if (csz > 0) {
            compressed.resize(csz);
            std::lock_guard<std::mutex> rlock(crMutex_);
            compResult_.compData      = std::move(compressed);
            compResult_.w             = w;
            compResult_.h             = h;
            compResult_.rawSize       = rawSize;
            compResult_.compSize      = static_cast<uint32_t>(csz);
            compResult_.valid         = true;
            compResult_.frameId       = jobFrameId;
            compResult_.presentUs     = jobPresentUs;
            compResult_.readbackUs    = jobReadbackUs;
            compResult_.compressDoneUs = rtNowUs();
        }
    }
}

void ClientSession::workerLoop() {
    // Create window + Vulkan on worker thread (so DestroyWindow works on exit)
    createSessionWindow();
    vk_.instance = instance_;
    vk_.physicalDevice = physDevice_;
    createSurface(vk_, hwnd_, hInstance_);
    createLogicalDevice(vk_);
    decoder_.init(vk_.physicalDevice, vk_.device, vk_.graphicsQueue,
                  vk_.graphicsFamily, vk_.surface);
    bool disableReadback = (getenv("VBOXGPU_NO_READBACK") != nullptr);
    decoder_.noReadback_ = disableReadback;

    constexpr size_t BUF_SIZE = 256 * 1024 * 1024;
    std::vector<uint8_t> recvBuf(BUF_SIZE);
    std::vector<uint8_t> sendBuf;

    int fpsFrameCount = 0;
    auto fpsLastTime = std::chrono::steady_clock::now();

    while (running_) {
        // Receive
        size_t bytesRead = 0;
        uint32_t frameLen = 0;
        // Read framed message: [4-byte len][payload]
        if (!tcp_recv_all(clientSock_, reinterpret_cast<uint8_t*>(&frameLen), 4) ||
            frameLen > BUF_SIZE ||
            (frameLen > 0 && !tcp_recv_all(clientSock_, recvBuf.data(), frameLen))) {
            fprintf(stderr, "[Session %d] Client disconnected.\n", id_);
            break;
        }
        bytesRead = frameLen;

        // Record to dump file if enabled
        if (dumpFile_) {
            uint32_t sz = static_cast<uint32_t>(bytesRead);
            fwrite(&sz, sizeof(sz), 1, dumpFile_);
            fwrite(recvBuf.data(), 1, bytesRead, dumpFile_);
            fflush(dumpFile_);
        }

#if VBOXGPU_TIMING
        decoder_.batchRecvUs_ = rtNowUs();
        RT_LOG(0, "T2", "recv %zu bytes", bytesRead);
#endif

        // Execute
        if (!decoder_.execute(recvBuf.data(), bytesRead)) {
            fprintf(stderr, "[Session %d] Command stream execution failed.\n", id_);
            break;
        }

        // Post readback to compress thread
        if (!disableReadback && decoder_.hasReadback()) {
            auto* rawPtr = static_cast<const uint8_t*>(decoder_.getReadbackData());
            uint32_t rawSize = decoder_.getReadbackSize();
            {
                std::lock_guard<std::mutex> lock(cjMutex_);
                compJob_.rawData.assign(rawPtr, rawPtr + rawSize);
                compJob_.w = decoder_.getReadbackWidth();
                compJob_.h = decoder_.getReadbackHeight();
                compJob_.valid = true;
#if VBOXGPU_TIMING
                compJob_.frameId   = decoder_.readyFrameTiming_.frameId;
                compJob_.presentUs = decoder_.readyFrameTiming_.presentUs;
                compJob_.readbackUs = decoder_.readyFrameTiming_.readbackUs;
#endif
            }
            cjCV_.notify_one();
        }

        // Get compressed result (1-frame lag)
        CompressResult result;
        if (!disableReadback) {
            std::lock_guard<std::mutex> lock(crMutex_);
            if (compResult_.valid) {
                result = std::move(compResult_);
                compResult_.valid = false;
            }
        }

        // Build response
        auto* sc = decoder_.getFirstSwapchain();
        uint32_t imageIndex = sc ? sc->currentImageIndex : 0;
        size_t payloadSize = 16;

        // FPS counter
        fpsFrameCount++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fpsLastTime).count();
        if (elapsed >= 1000) {
            fprintf(stderr, "[Session %d] FPS: %.1f\n", id_, fpsFrameCount * 1000.0 / elapsed);
            fpsFrameCount = 0;
            fpsLastTime = now;
        }

        size_t bdaBytes = 4 + decoder_.pendingBdaResults_.size() * 16;
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
        } else {
            if (sendBuf.size() < 16 + bdaBytes + TIMING_BYTES)
                sendBuf.resize(16 + bdaBytes + TIMING_BYTES);
            memset(sendBuf.data(), 0, 16);
            memcpy(sendBuf.data(), &imageIndex, 4);
            payloadSize = 16;
        }

        // Append BDA results
        uint32_t bdaCount = static_cast<uint32_t>(decoder_.pendingBdaResults_.size());
        memcpy(sendBuf.data() + payloadSize, &bdaCount, 4);
        for (uint32_t i = 0; i < bdaCount; i++) {
            memcpy(sendBuf.data() + payloadSize + 4 + i * 16,
                   &decoder_.pendingBdaResults_[i].bufferId, 8);
            memcpy(sendBuf.data() + payloadSize + 4 + i * 16 + 8,
                   &decoder_.pendingBdaResults_[i].address, 8);
        }
        payloadSize += 4 + bdaCount * 16;

#if VBOXGPU_TIMING
        uint64_t hostSendUs = rtNowUs();
        if (sendBuf.size() < payloadSize + TIMING_BYTES)
            sendBuf.resize(payloadSize + TIMING_BYTES);
        size_t tp = payloadSize;
        memcpy(sendBuf.data() + tp,      &decoder_.currentSeqId_, 4);
        memcpy(sendBuf.data() + tp + 4,  &decoder_.batchRecvUs_,  8);
        memcpy(sendBuf.data() + tp + 12, &hostSendUs,            8);
        uint32_t ftFrameId = result.valid ? result.frameId : 0;
        uint64_t ftPresentUs = result.valid ? result.presentUs : 0;
        uint64_t ftReadbackUs = result.valid ? result.readbackUs : 0;
        uint64_t ftCompressUs = result.valid ? result.compressDoneUs : 0;
        memcpy(sendBuf.data() + tp + 20, &ftFrameId,     4);
        memcpy(sendBuf.data() + tp + 24, &ftPresentUs,   8);
        memcpy(sendBuf.data() + tp + 32, &ftReadbackUs,  8);
        memcpy(sendBuf.data() + tp + 40, &ftCompressUs,  8);
        payloadSize += TIMING_BYTES;
        RT_LOG(decoder_.currentSeqId_, "T6", "send %zu bytes, host=%.2fms frame=%u",
               payloadSize, (hostSendUs - decoder_.batchRecvUs_) / 1000.0, ftFrameId);
#endif

        // Send framed response
        if (!tcp_send_framed(clientSock_, sendBuf.data(), payloadSize)) {
            fprintf(stderr, "[Session %d] Send failed.\n", id_);
            break;
        }
    }

    // Cleanup
    running_ = false;
    if (dumpFile_) { fclose(dumpFile_); dumpFile_ = nullptr; }
    decoder_.cleanup();
    if (clientSock_ != INVALID_SOCKET) {
        closesocket(clientSock_);
        clientSock_ = INVALID_SOCKET;
    }
    // Destroy per-session Vulkan device (keep instance + physDevice shared)
    if (vk_.device) {
        vkDeviceWaitIdle(vk_.device);
        if (vk_.surface) { vkDestroySurfaceKHR(vk_.instance, vk_.surface, nullptr); vk_.surface = VK_NULL_HANDLE; }
        vkDestroyDevice(vk_.device, nullptr);
        vk_.device = VK_NULL_HANDLE;
    }
    // Destroy window immediately so it doesn't linger as "not responding"
    destroySessionWindow();
    fprintf(stderr, "[Session %d] Ended, resources released.\n", id_);
}

void ClientSession::replayLoop() {
    // Create window + Vulkan (same as live)
    createSessionWindow();
    vk_.instance = instance_;
    vk_.physicalDevice = physDevice_;
    createSurface(vk_, hwnd_, hInstance_);
    createLogicalDevice(vk_);
    decoder_.init(vk_.physicalDevice, vk_.device, vk_.graphicsQueue,
                  vk_.graphicsFamily, vk_.surface);

    fprintf(stderr, "[Session %d] Replaying %zu batches...\n", id_, replayBatches_.size());

    // Find first batch with a present (setup vs render split)
    size_t setupEnd = 0;
    for (size_t i = 0; i < replayBatches_.size(); i++) {
        // Heuristic: batches after the first large one are render batches
        if (replayBatches_[i].data.size() > 1024 * 1024) { setupEnd = i + 1; break; }
    }
    if (setupEnd == 0) setupEnd = replayBatches_.size();

    // Execute all batches
    for (size_t i = 0; i < replayBatches_.size() && running_; i++) {
        // Pump window messages between batches
        MSG msg;
        while (PeekMessageA(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        fprintf(stderr, "[Replay %d] Batch %zu (%zu bytes)%s\n",
                id_, i, replayBatches_[i].data.size(),
                i < setupEnd ? "" : " [render]");

        if (!decoder_.execute(replayBatches_[i].data.data(), replayBatches_[i].data.size())) {
            fprintf(stderr, "[Replay %d] Batch %zu failed\n", id_, i);
            break;
        }

        // Save screenshot if requested
        if (saveFramesDir_ && i >= setupEnd) {
            char path[512];
            snprintf(path, sizeof(path), "%s\\replay_%d_batch%zu.bmp", saveFramesDir_, id_, i);
            decoder_.captureScreenshot(path);
        }
    }

    fprintf(stderr, "[Replay %d] Done.\n", id_);

    // Cleanup (same as live)
    running_ = false;
    decoder_.cleanup();
    if (vk_.device) {
        vkDeviceWaitIdle(vk_.device);
        if (vk_.surface) { vkDestroySurfaceKHR(vk_.instance, vk_.surface, nullptr); vk_.surface = VK_NULL_HANDLE; }
        vkDestroyDevice(vk_.device, nullptr);
        vk_.device = VK_NULL_HANDLE;
    }
    destroySessionWindow();
    fprintf(stderr, "[Session %d] Replay ended, resources released.\n", id_);
}

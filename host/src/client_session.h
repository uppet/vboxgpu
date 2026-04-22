#pragma once

// ClientSession: encapsulates one guest client's rendering pipeline.
// Each session has its own window, VkDevice, VnDecoder, worker + compress threads.
// Lifecycle: construct → start(socket) → runs until client disconnects → cleanup → destroy.

#include "vn_decoder.h"
#include "vk_bootstrap.h"
#include "../../common/transport/tcp_transport.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

class ClientSession {
public:
    // id: session index (0..N-1), used for window title
    // physDevice: shared physical device (all sessions share the same GPU)
    // instance: shared VkInstance
    // hInstance: Win32 HINSTANCE for window creation
    explicit ClientSession(int id, VkPhysicalDevice physDevice, VkInstance instance,
                           HINSTANCE hInstance);
    ~ClientSession();

    // Start live TCP session. Takes ownership of the socket.
    // dumpDir: if non-null, record command stream to <dumpDir>/session_<id>.bin
    void start(SOCKET clientSock, const char* dumpDir = nullptr);

    // Start replay session from recorded batches.
    struct ReplayBatch { std::vector<uint8_t> data; };
    void startReplay(std::vector<ReplayBatch> batches, const char* saveFramesDir = nullptr);

    bool isRunning() const { return running_; }
    bool isReplay() const { return replay_; }
    void join();
    int id() const { return id_; }
    HWND hwnd() const { return hwnd_; }

private:
    void workerLoop();
    void replayLoop();
    void compressLoop();
    void createSessionWindow();
    void destroySessionWindow();

    int id_;
    bool replay_ = false;
    std::vector<ReplayBatch> replayBatches_;
    const char* saveFramesDir_ = nullptr;
    FILE* dumpFile_ = nullptr;  // record command stream (owned, session closes)
    VkPhysicalDevice physDevice_;
    VkInstance instance_;
    HINSTANCE hInstance_;

    // Per-session Vulkan
    VulkanContext vk_{};
    VnDecoder decoder_;
    HWND hwnd_ = nullptr;

    // TCP — raw socket, wrapped for send/recv
    SOCKET clientSock_ = INVALID_SOCKET;

    // Threading
    std::atomic<bool> running_{false};
    std::thread workerThread_;
    std::thread compressThread_;
    std::atomic<bool> compRunning_{false};

    // Compress pipeline (same structure as old main_server.cpp)
    struct CompressJob {
        std::vector<uint8_t> rawData;
        uint32_t w = 0, h = 0;
        bool valid = false;
        uint32_t frameId = 0;
        uint64_t presentUs = 0, readbackUs = 0;
    };
    struct CompressResult {
        std::vector<uint8_t> compData;
        uint32_t w = 0, h = 0, rawSize = 0, compSize = 0;
        bool valid = false;
        uint32_t frameId = 0;
        uint64_t presentUs = 0, readbackUs = 0, compressDoneUs = 0;
    };
    std::mutex cjMutex_;
    std::condition_variable cjCV_;
    CompressJob compJob_;
    std::mutex crMutex_;
    CompressResult compResult_;
};

// Default max concurrent sessions
constexpr int DEFAULT_MAX_SESSIONS = 3;

#pragma once

// VBox GPU Bridge — Vulkan ICD (Installable Client Driver)
// A proxy Vulkan driver that encodes all calls via Venus and sends to Host.
// DXVK loads this as vulkan-1.dll; no DXVK modification needed.

// Must include winsock2 before windows.h to avoid conflicts
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>
#include <chrono>

#include "../../common/venus/vn_encoder.h"
#include "../../common/transport/tcp_transport.h"
#define VBOXGPU_TIMING_WIN32_LOG
#include "../../common/timing.h"

// Handle ID generator
class HandleAllocator {
public:
    uint64_t alloc() { return next_.fetch_add(1); }
private:
    std::atomic<uint64_t> next_{1};
};

// Global ICD state
struct IcdState {
    HandleAllocator handles;
    TcpSender transport;
    bool connected = false;

    // Object tracking: our handle ID → opaque VkHandle for DXVK
    // We cast uint64_t IDs to Vulkan dispatchable/non-dispatchable handles
    std::mutex mutex;

    // Physical device properties (hardcoded for now, later queried from Host)
    VkPhysicalDeviceProperties physDeviceProps{};
    VkPhysicalDeviceFeatures physDeviceFeatures{};
    VkPhysicalDeviceMemoryProperties memProps{};
    std::vector<VkQueueFamilyProperties> queueFamilies;
    std::vector<VkExtensionProperties> deviceExtensions;
    std::vector<VkSurfaceFormatKHR> surfaceFormats;

    // Swapchain state
    uint32_t swapchainImageCount = 3;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapchainExtent = {800, 600};

    // Surface HWND for frame display (GDI blit)
    HWND presentHwnd = nullptr;

    // Per-frame sync: image index returned by Host (written by recv thread)
    uint32_t currentImageIndex = 0;

    // Frame data: pixel data received from Host (written only by recv thread)
    std::vector<uint8_t> framePixels;
    uint32_t frameWidth = 0, frameHeight = 0;
    bool frameValid = false;

    // Descriptor update templates: save entries so we can interpret pData later
    struct DescriptorTemplateInfo {
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
    };
    std::unordered_map<uint64_t, DescriptorTemplateInfo> descriptorTemplates;

    // Mapped memory tracking: guest shadow memory for host-visible regions
    struct MappedRegion {
        uint64_t memoryId;
        VkDeviceSize offset;
        VkDeviceSize size;
        void* ptr;
        bool dirty;  // set true after unmap or explicit flush
    };
    std::vector<MappedRegion> mappedRegions;
    std::mutex mappedMutex;

    // Shadow memory per VkDeviceMemory: memory_id → {ptr, size}
    // Allocated with MEM_WRITE_WATCH for kernel-level dirty page tracking.
    struct MemoryShadow {
        void* ptr;
        VkDeviceSize size;
        bool freed = false;    // set by FreeMemory, cleaned up by flushMappedMemory
    };
    std::unordered_map<uint64_t, MemoryShadow> memoryShadows;

    // ImageView → Image mapping (to detect swapchain targets in BeginRendering)
    std::unordered_map<uint64_t, uint64_t> imageViewToImage;

    // Image format tracking: image handle → VkFormat (needed for correct bpp in CopyBufferToImage)
    std::unordered_map<uint64_t, VkFormat> imageFormats;

    // Buffer size tracking: buffer handle → actual size
    std::unordered_map<uint64_t, VkDeviceSize> bufferSizes;

    // Buffer → memory binding: buffer_id → (memory_id, memory_offset)
    struct BufferBinding { uint64_t memoryId; VkDeviceSize memoryOffset; };
    std::unordered_map<uint64_t, BufferBinding> bufferBindings;

    // BDA forwarding: cache of host-side buffer device addresses (protected by bdaMutex_)
    std::unordered_map<uint64_t, uint64_t> bdaCache;

    // BDA optimization: flush at BindBufferMemory + Host auto-BDA
    std::unordered_set<uint64_t> bdaNeedBuffers_;

    // BDA recording: buffer IDs already emitted via RecordBDA (deduplication)
    std::unordered_set<uint64_t> bdaRecorded_;

    // GDI blit rate limiter: blit is driven by recv thread (actual rendered frames).
    // Cap at 60 FPS to avoid GDI saturation when game runs very fast.
    static constexpr int BLIT_INTERVAL_MS = 16;
    std::chrono::steady_clock::time_point lastBlitTime{};

    // Flush all small mapped memory data to encoder (call before QueueSubmit)
    void flushMappedMemory();

    // Flush specific buffer data based on its memory binding
    void flushBufferRange(uint64_t bufferId, VkDeviceSize offset, VkDeviceSize range);

    // Command encoder for current batch (MUST lock mutex before use — DXVK is multithreaded)
    VnEncoder encoder;
    std::mutex encoderMutex;

    // --- Async recv thread ---
    // sendBatch() sends the current encoder buffer to host (non-blocking).
    // recvLoop() runs in background and receives host responses:
    //   - Updates frame pixels and blits to window
    //   - Signals acquireCV_ for vkAcquireNextImageKHR
    //   - Signals bdaCV_ for syncGetBufferDeviceAddress
    std::thread recvThread_;
    std::atomic<bool> recvRunning_{false};

    // Ordered queue of send types — recv thread pops to know how to dispatch response.
    // true = present batch (signal acquireCV_), false = BDA query (signal bdaCV_).
    // Lock ordering: encoder.mutex_ → pendingQueueMutex_ (never reverse)
    struct PendingResponse {
        bool isPresent;
        uint32_t seqId;
        uint64_t sendTimestampUs;
    };
    std::queue<PendingResponse> pendingResponseQueue_;
    std::mutex pendingQueueMutex_;

    // Roundtrip timing: monotonic sequence ID for each batch
    uint32_t nextSeqId_ = 0;

    // AcquireNextImageKHR: recv thread delivers imageIndex here.
    // With pipelining, acquire uses round-robin prediction; this is consumed when available.
    std::mutex acquireMutex_;
    std::condition_variable acquireCV_;
    bool imageIndexReady_ = false;
    std::atomic<bool> firstPresented_{false};

    // Pipelining: limit in-flight present batches to MAX_IN_FLIGHT.
    // QueuePresent waits here if too many batches are outstanding.
    // Recv thread decrements on each present ack.
    static constexpr int MAX_IN_FLIGHT = 2;
    int inFlightBatches_ = 0;   // guarded by inFlightMutex_
    std::mutex inFlightMutex_;
    std::condition_variable inFlightCV_;

    // Predictive image index: last value returned by AcquireNextImageKHR.
    // Round-robin mod swapchainImageCount when ack has not yet arrived.
    uint32_t lastPredictedImageIndex_ = 0;

    // syncGetBufferDeviceAddress waits here for BDA results (also guards bdaCache).
    std::mutex bdaMutex_;
    std::condition_variable bdaCV_;

    ~IcdState() { stopRecvThread(); }
    void startRecvThread();
    void stopRecvThread();
    void recvLoop();
    bool sendBatch(bool isPresent); // send encoder buffer; isPresent=true for QueuePresent
    bool sendBatchLocked(bool isPresent); // caller must hold encoder.mutex_

    uint64_t syncGetBufferDeviceAddress(uint64_t bufferId);
    void blitFrameToWindow();

    void initDefaults();
    bool connectToHost(const char* host, uint16_t port);
};

extern IcdState g_icd;

// ICD entry point (exported from DLL)
extern "C" {
    __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
    vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

    // Also export as vkGetInstanceProcAddr for loader compatibility
    __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
    vkGetInstanceProcAddr(VkInstance instance, const char* pName);
}

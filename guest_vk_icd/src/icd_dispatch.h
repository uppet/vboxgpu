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
#include <vector>
#include <mutex>
#include <atomic>

#include "../../common/venus/vn_encoder.h"
#include "../../common/transport/tcp_transport.h"

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

    // Per-frame sync: image index returned by Host
    uint32_t currentImageIndex = 0;

    // Frame return: pixel data received from Host
    std::vector<uint8_t> frameRecvBuf;    // reusable TCP receive buffer
    std::vector<uint8_t> framePixels;     // last received frame (BGRA)
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
    struct MemoryShadow { void* ptr; VkDeviceSize size; };
    std::unordered_map<uint64_t, MemoryShadow> memoryShadows;

    // ImageView → Image mapping (to detect swapchain targets in BeginRendering)
    std::unordered_map<uint64_t, uint64_t> imageViewToImage;

    // Buffer size tracking: buffer handle → actual size
    std::unordered_map<uint64_t, VkDeviceSize> bufferSizes;

    // Buffer → memory binding: buffer_id → (memory_id, memory_offset)
    struct BufferBinding { uint64_t memoryId; VkDeviceSize memoryOffset; };
    std::unordered_map<uint64_t, BufferBinding> bufferBindings;

    // Flush all small mapped memory data to encoder (call before QueueSubmit)
    void flushMappedMemory();

    // Flush specific buffer data based on its memory binding
    void flushBufferRange(uint64_t bufferId, VkDeviceSize offset, VkDeviceSize range);

    // Command encoder for current batch (MUST lock mutex before use — DXVK is multithreaded)
    VnEncoder encoder;
    std::mutex encoderMutex;

    void initDefaults();
    bool connectToHost(const char* host, uint16_t port);
    bool sendAndRecv(uint32_t* imageIndexOut = nullptr);
    void blitFrameToWindow();
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

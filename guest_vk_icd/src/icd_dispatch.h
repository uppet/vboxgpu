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

    // Per-frame sync: image index returned by Host
    uint32_t currentImageIndex = 0;

    // Command encoder for current batch (MUST lock mutex before use — DXVK is multithreaded)
    VnEncoder encoder;
    std::mutex encoderMutex;

    void initDefaults();
    bool connectToHost(const char* host, uint16_t port);
    bool sendAndRecv(uint32_t* imageIndexOut = nullptr);
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

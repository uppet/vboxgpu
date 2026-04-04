#pragma once

// Host-side Venus command stream decoder.
// Reads binary commands and executes them on real Vulkan.

#include "vk_bootstrap.h"
#include "../../common/venus/vn_command.h"
#include "../../common/venus/vn_stream.h"

#include <unordered_map>
#include <vector>
#include <functional>
#include <string>

// The decoder maintains a mapping from stream handle IDs to real Vulkan objects.
// Swapchain is managed by the host (not a real Venus concept).
struct HostSwapchain {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    uint32_t currentImageIndex = 0;
};

class VnDecoder {
public:
    // Initialize with an already-set-up Vulkan context (instance, device, etc.)
    // The decoder uses this context to execute decoded commands.
    void init(VkPhysicalDevice physDevice, VkDevice device,
              VkQueue graphicsQueue, uint32_t graphicsFamily,
              VkSurfaceKHR surface);

    // Execute all commands in the stream. Returns false on error.
    bool execute(const uint8_t* data, size_t size);

    // Execute a single frame's worth of commands (up to QueuePresent).
    // Returns false if end-of-stream or error.
    bool executeOneFrame(VnStreamReader& reader);

    // Cleanup all resources created by decoded commands.
    void cleanup();

    // Get the swapchain (for main loop integration)
    HostSwapchain* getSwapchain(uint64_t id);

    // Lookup a semaphore by stream ID (for host-side acquire)
    VkSemaphore lookupSemaphore(uint64_t id) { return lookup(semaphores_, id); }

    // Get first available swapchain (for server mode)
    HostSwapchain* getFirstSwapchain() {
        if (swapchains_.empty()) return nullptr;
        return &swapchains_.begin()->second;
    }

private:
    void dispatch(uint32_t cmdType, VnStreamReader& reader, uint32_t cmdSize);

    // Command handlers
    void handleCreateRenderPass(VnStreamReader& r);
    void handleCreateShaderModule(VnStreamReader& r);
    void handleCreatePipelineLayout(VnStreamReader& r);
    void handleCreateGraphicsPipeline(VnStreamReader& r);
    void handleCreateFramebuffer(VnStreamReader& r);
    void handleCreateCommandPool(VnStreamReader& r);
    void handleAllocateCommandBuffers(VnStreamReader& r);
    void handleBeginCommandBuffer(VnStreamReader& r);
    void handleEndCommandBuffer(VnStreamReader& r);
    void handleCmdBeginRenderPass(VnStreamReader& r);
    void handleCmdEndRenderPass(VnStreamReader& r);
    void handleCmdBeginRendering(VnStreamReader& r);
    void handleCmdEndRendering(VnStreamReader& r);
    void handleCmdBindPipeline(VnStreamReader& r);
    void handleCmdSetViewport(VnStreamReader& r);
    void handleCmdSetScissor(VnStreamReader& r);
    void handleCmdDraw(VnStreamReader& r);
    void handleCreateSemaphore(VnStreamReader& r);
    void handleCreateFence(VnStreamReader& r);
    void handleQueueSubmit(VnStreamReader& r);
    void handleWaitForFences(VnStreamReader& r);
    void handleResetFences(VnStreamReader& r);
    void handleBridgeCreateSwapchain(VnStreamReader& r);
    void handleBridgeAcquireNextImage(VnStreamReader& r);
    void handleBridgeQueuePresent(VnStreamReader& r);

    // Handle maps: stream ID → real Vulkan object
    template<typename T>
    void store(std::unordered_map<uint64_t, T>& map, uint64_t id, T obj) { map[id] = obj; }

    template<typename T>
    T lookup(const std::unordered_map<uint64_t, T>& map, uint64_t id) {
        auto it = map.find(id);
        if (it == map.end()) return VK_NULL_HANDLE;
        return it->second;
    }

    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, VkRenderPass> renderPasses_;
    std::unordered_map<uint64_t, VkShaderModule> shaderModules_;
    std::unordered_map<uint64_t, VkPipelineLayout> pipelineLayouts_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
    std::unordered_map<uint64_t, VkCommandPool> commandPools_;
    std::unordered_map<uint64_t, VkCommandBuffer> commandBuffers_;
    std::unordered_map<uint64_t, VkSemaphore> semaphores_;
    std::unordered_map<uint64_t, VkFence> fences_;
    std::unordered_map<uint64_t, VkImageView> imageViews_;
    std::unordered_map<uint64_t, HostSwapchain> swapchains_;

    bool error_ = false;
};

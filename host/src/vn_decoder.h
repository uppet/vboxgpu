#pragma once

// Host-side Venus command stream decoder.
// Reads binary commands and executes them on real Vulkan.

// Performance optimization switches (define to 0 to disable)
#define VBOXGPU_PERF_FENCE_SYNC      1  // Remove vkDeviceWaitIdle from QueueSubmit
#define VBOXGPU_PERF_DIRTY_TRACK     1  // ICD mapped memory dirty tracking
#define VBOXGPU_PERF_READBACK_FENCE  1  // Readback uses fence instead of QueueWaitIdle

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

    // Set to true to skip GPU readback (saves ~5ms/frame, no frame return to guest)
    bool noReadback_ = false;

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

    // Capture current swapchain image to a BMP file.
    // Returns true on success.
    bool captureScreenshot(const char* path);

    // Frame readback: last presented frame available for TCP return
    bool hasReadback() const { return readbackValid_; }
    const void* getReadbackData() const { return readbackValid_ ? readback_.mappedPtr : nullptr; }
    uint32_t getReadbackWidth() const { return readback_.width; }
    uint32_t getReadbackHeight() const { return readback_.height; }
    uint32_t getReadbackSize() const { return static_cast<uint32_t>(readback_.bufferSize); }

private:
    void dispatch(uint32_t cmdType, VnStreamReader& reader, uint32_t cmdSize);

    // Command handlers
    void handleCreateRenderPass(VnStreamReader& r);
    void handleCreateShaderModule(VnStreamReader& r);
    void handleCreateDescriptorSetLayout(VnStreamReader& r, uint32_t cmdSize);
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
    void handleCmdPushConstants(VnStreamReader& r);
    void handleCreateSemaphore(VnStreamReader& r);
    void handleCreateFence(VnStreamReader& r);
    void handleQueueSubmit(VnStreamReader& r);
    void handleWaitForFences(VnStreamReader& r);
    void handleResetFences(VnStreamReader& r);
    void handleCreateImage(VnStreamReader& r);
    void handleAllocateMemory(VnStreamReader& r);
    void handleBindImageMemory(VnStreamReader& r);
    void handleCreateImageView(VnStreamReader& r);
    void handleCreateSampler(VnStreamReader& r);
    void handleCreateDescriptorPool(VnStreamReader& r);
    void handleAllocateDescriptorSets(VnStreamReader& r);
    void handleUpdateDescriptorSets(VnStreamReader& r);
    void handleCmdBindDescriptorSets(VnStreamReader& r);
    void handleCmdPushDescriptorSet(VnStreamReader& r);
    void handleCmdSetCullMode(VnStreamReader& r);
    void handleCmdSetFrontFace(VnStreamReader& r);
    void handleCmdSetPrimitiveTopology(VnStreamReader& r);
    void handleCmdSetDepthTestEnable(VnStreamReader& r);
    void handleCmdSetDepthWriteEnable(VnStreamReader& r);
    void handleCmdSetDepthCompareOp(VnStreamReader& r);
    void handleCmdSetDepthBoundsTestEnable(VnStreamReader& r);
    void handleCmdSetDepthBiasEnable(VnStreamReader& r);
    void handleCmdBindVertexBuffers(VnStreamReader& r);
    void handleCmdBindIndexBuffer(VnStreamReader& r);
    void handleCmdDrawIndexed(VnStreamReader& r);
    void handleCmdCopyBuffer(VnStreamReader& r);
    void handleCmdCopyImage(VnStreamReader& r);
    void handleCmdBlitImage(VnStreamReader& r);
    void handleCmdCopyBufferToImage(VnStreamReader& r);
    void handleCmdUpdateBuffer(VnStreamReader& r);
    void handleCmdPipelineBarrier(VnStreamReader& r);
    void handleCreateBuffer(VnStreamReader& r);
    void handleBindBufferMemory(VnStreamReader& r);
    void handleCmdClearAttachments(VnStreamReader& r);
    void handleCmdClearColorImage(VnStreamReader& r);
    void handleWriteMemory(VnStreamReader& r);
    // Destroy / Free handlers
    void handleDestroyBuffer(VnStreamReader& r);
    void handleDestroyImage(VnStreamReader& r);
    void handleDestroyImageView(VnStreamReader& r);
    void handleDestroyShaderModule(VnStreamReader& r);
    void handleDestroyPipeline(VnStreamReader& r);
    void handleDestroyPipelineLayout(VnStreamReader& r);
    void handleDestroyRenderPass(VnStreamReader& r);
    void handleDestroyFramebuffer(VnStreamReader& r);
    void handleDestroyCommandPool(VnStreamReader& r);
    void handleDestroySampler(VnStreamReader& r);
    void handleDestroyDescriptorPool(VnStreamReader& r);
    void handleDestroyDescriptorSetLayout(VnStreamReader& r);
    void handleDestroyFence(VnStreamReader& r);
    void handleDestroySemaphore(VnStreamReader& r);
    void handleFreeMemory(VnStreamReader& r);

    void handleBridgeCreateSwapchain(VnStreamReader& r);
    void handleBridgeAcquireNextImage(VnStreamReader& r);
    void handleBridgeQueuePresent(VnStreamReader& r);
    void handleGetBufferDeviceAddress(VnStreamReader& r);

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
    std::unordered_map<uint64_t, VkBuffer> buffers_;
    std::unordered_map<uint64_t, VkImage> images_;
    std::unordered_map<uint64_t, VkFormat> imageFormats_; // image ID → format
    std::unordered_map<uint64_t, VkDeviceMemory> deviceMemories_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkDescriptorPool> descriptorPools_;
    std::unordered_map<uint64_t, VkDescriptorSet> descriptorSets_;
    std::unordered_map<uint64_t, VkDescriptorSetLayout> descriptorSetLayouts_;
    std::unordered_map<uint64_t, VkPipelineLayout> pipelineLayouts_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
    std::unordered_map<uint64_t, VkCommandPool> commandPools_;
    std::unordered_map<uint64_t, VkCommandBuffer> commandBuffers_;
    std::unordered_map<uint64_t, VkSemaphore> semaphores_;
    std::unordered_map<uint64_t, VkFence> fences_;
    std::unordered_map<uint64_t, VkImageView> imageViews_;
    std::unordered_map<uint64_t, HostSwapchain> swapchains_;
    bool activeRendering_ = false;
    bool activeRenderingIsSwapchain_ = false; // true if current render pass targets swapchain
    VkSemaphore acquireSemaphore_ = VK_NULL_HANDLE; // for swapchain acquire sync
    VkFence acquireFence_ = VK_NULL_HANDLE;
    VkFence readbackFence_ = VK_NULL_HANDLE; // sync for frame readback copies
    uint32_t lastPresentedImageIndex_ = 0; // for screenshot fidelity

    // Per-CB fence tracking: maps CB stream ID → last submit fence
    std::unordered_map<uint64_t, VkFence> cbLastFence_;
    // Fence pool: reusable fences for per-CB tracking
    std::vector<VkFence> fencePool_;
    VkFence allocateFence();
    void recycleFence(VkFence f);

    // Deferred present: collect during batch, execute after all QueueSubmits
    struct PendingPresent {
        uint64_t queueId;
        uint64_t scId;
        uint64_t waitSemId;
    };
    std::vector<PendingPresent> pendingPresents_;
    void flushPendingPresents();

    // Deferred destroy: collect during batch, execute after GPU is idle
    std::vector<std::function<void()>> pendingDestroys_;
    void flushPendingDestroys();

    // Frame readback infrastructure (persistent, reused per frame)
    struct FrameReadback {
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        void* mappedPtr = nullptr;
        uint32_t width = 0, height = 0;
        VkDeviceSize bufferSize = 0;
    };
    FrameReadback readback_;
    bool readbackValid_ = false;

    void ensureReadbackResources(uint32_t w, uint32_t h);
    bool readbackFrame(uint32_t imageIndex, HostSwapchain& sc);

    bool error_ = false;

public:
    // BDA query results: accumulated during execute(), consumed by server
    struct BdaResult { uint64_t bufferId; uint64_t address; };
    std::vector<BdaResult> pendingBdaResults_;
};

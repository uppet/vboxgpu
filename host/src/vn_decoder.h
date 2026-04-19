#pragma once

// Host-side Venus command stream decoder.
// Reads binary commands and executes them on real Vulkan.

// Performance optimization switches (define to 0 to disable)
#define VBOXGPU_PERF_FENCE_SYNC      1  // Remove vkDeviceWaitIdle from QueueSubmit
#define VBOXGPU_PERF_DIRTY_TRACK     1  // ICD mapped memory dirty tracking

#include "vk_bootstrap.h"
#include "../../common/venus/vn_command.h"
#include "../../common/venus/vn_stream.h"
#include "../../common/timing.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
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

    // Debug: capture any VkImage to a BMP file (for diagnosing black frames).
    void debugCaptureImage(VkImage img, VkFormat fmt, uint32_t w, uint32_t h,
                           VkImageLayout currentLayout, const char* path);

    // Frame readback: last presented frame available for TCP return
    // Double-buffered: GPU copies frame N while CPU reads frame N-1
    bool hasReadback() const { return rbReady_ >= 0; }
    const void* getReadbackData() const { return rbReady_ >= 0 ? readback_[rbReady_].mappedPtr : nullptr; }
    uint32_t getReadbackWidth() const { return rbReady_ >= 0 ? readback_[rbReady_].width : 0; }
    uint32_t getReadbackHeight() const { return rbReady_ >= 0 ? readback_[rbReady_].height : 0; }
    uint32_t getReadbackSize() const { return rbReady_ >= 0 ? static_cast<uint32_t>(readback_[rbReady_].bufferSize) : 0; }

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
    void handleCopyBufToImgInline(VnStreamReader& r);
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
    void handleBridgeRecordBDA(VnStreamReader& r);

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
    std::unordered_map<uint64_t, VkImageLayout> imageLayouts_; // image ID → current layout on host
    std::unordered_map<uint64_t, VkDeviceMemory> deviceMemories_;
    // Persistent memory maps: memId → base pointer (kept mapped across WriteMemory calls).
    // Eliminates vkMapMemory/vkUnmapMemory per-WriteMemory overhead (major perf bottleneck).
    std::unordered_map<uint64_t, void*> persistentMaps_;
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
    // Per-CB swapchain tracking: cbId → true if that CB is currently in a swapchain render pass.
    // Needed because multiple CBs may be recorded concurrently and interleave BeginRendering calls,
    // making the single bool above unreliable.
    std::unordered_map<uint64_t, bool> cbIsSwapchain_;
    // Swapchain image view tracking: viewId → swapchain image index.
    // Populated when handleCreateImageView sees a sentinel image ID (0xFFF00000+i).
    // Used by handleCmdBeginRendering to identify swapchain views without fallback.
    std::unordered_map<uint64_t, uint32_t> swapchainViewImageIndex_;
    VkSemaphore acquireSemaphore_ = VK_NULL_HANDLE; // for swapchain acquire sync
    VkFence acquireFence_ = VK_NULL_HANDLE;
    // Double-buffered readback fences: one per slot
    VkFence readbackFences_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
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

    // Pending submit batching: consecutive QueueSubmit calls with no semaphores/fences
    // are collected and submitted together in a single vkQueueSubmit to preserve
    // Vulkan-spec ordering (multiple CBs in same VkSubmitInfo execute in order).
    struct PendingSubmitCB {
        uint64_t cbId;
        VkCommandBuffer cb;
    };
    std::vector<PendingSubmitCB> pendingSubmitCBs_;
    // Flush all pending CBs in a single vkQueueSubmit, then clear the pending list.
    // waitSem/sigSem/fence apply to the entire batch (the "last" submit's semaphores).
    void flushPendingSubmits(VkSemaphore waitSem, VkSemaphore sigSem, VkFence userFence);

    // Deferred destroy: collect during batch, execute after GPU is idle
    std::vector<std::function<void()>> pendingDestroys_;
    void flushPendingDestroys();

    // Double-buffered frame readback infrastructure
    // Slot rbCur_ receives this frame's GPU copy (submitted async, no wait).
    // Slot 1-rbCur_ holds the previous frame's data (fence already waited).
    struct FrameReadback {
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        void* mappedPtr = nullptr;
        uint32_t width = 0, height = 0;
        VkDeviceSize bufferSize = 0;
    };
    FrameReadback readback_[2];
    bool readbackSubmitted_[2] = {false, false}; // GPU copy submitted, fence not yet waited
    int rbCur_ = 0;    // write slot for the current frame
    int rbReady_ = -1; // slot with CPU-readable data (-1 = none yet)

    void ensureReadbackResources(int slot, uint32_t w, uint32_t h);
    bool readbackFrameAsync(uint32_t imageIndex, HostSwapchain& sc, int slot);

    bool error_ = false;
    bool gpuHung_ = false;  // true if a fence wait timed out (GPU device lost)

    // Buffer → memory binding tracking (for staging snapshot in CopyBuffer*)
    struct BufferMemBinding { uint64_t memoryId; VkDeviceSize memoryOffset; };
    std::unordered_map<uint64_t, BufferMemBinding> bufferBindings_;

    // Reusable staging buffer for CopyBufferToImage/CopyBuffer snapshot.
    // Prevents later WriteMemory from overwriting copy source data before
    // the GPU executes the copy (staging data lifecycle problem).
    struct CopyStagingBuf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
    };
    CopyStagingBuf copyStagingBuf_;
    VkDeviceSize copyStagingUsed_ = 0;  // arena offset: advances per copy, reset per batch
    bool ensureCopyStagingBuf(VkDeviceSize needed);
    // Retired staging buffers: CBs spanning multiple batches may still reference old
    // staging buffer handles after a REALLOC. Keep them alive until cleanup() to avoid
    // GPU page faults when those CBs are finally submitted and executed.
    std::vector<CopyStagingBuf> retiredStagingBufs_;

    // Semaphore pending-signal tracking.
    // Binary semaphores start unsignaled. They enter "pending-signal" state when:
    //   (a) submitted as signalSemaphore in vkQueueSubmit, or
    //   (b) passed to vkAcquireNextImageKHR (presentation engine will signal it).
    // A QueueSubmit that waits on a semaphore NOT in this set would GPU-deadlock
    // (semaphore never signaled). In that case we skip the wait.
    // Entry is removed (consumed) when the semaphore is used as a wait.
    std::unordered_set<uint64_t> semPendingSignal_; // stream semaphore IDs

    // ---------------------------------------------------------------------------
    // Method-D pipelining: parallel CB recording.
    // After the parse loop, each CB's task list is executed by a worker thread.
    // Handle lookups are resolved at parse time (main thread); lambdas only call Vulkan.
    std::unordered_map<uint64_t, std::vector<std::function<void()>>> cbTasks_;
    // cbIds currently in RECORDING state (BeginCB seen, EndCB lambda not yet run).
    // Guards barrier/draw lambdas from firing on EXECUTABLE-state CBs.
    std::unordered_set<uint64_t> recordingCbIds_;

    // ---------------------------------------------------------------------------
    // Method-A pipelining: overlap decode with GPU execution.
    //
    // Two-slot command buffer scheme: each guest CB has 2 host CBs.
    //   Slot 0: used on even frames, slot 1: used on odd frames.
    //   When beginning a CB in slot S, we only wait slotFences_[S] — which is
    //   from 2 frames ago. With MAX_IN_FLIGHT=2 on the guest, that fence is
    //   typically already signaled before the batch arrives → 0 ms wait.
    //
    // WriteMemory staging: data is buffered in RAM during decode; applied to
    //   GPU-visible memory only in flushPendingSubmits(), AFTER waiting the
    //   prev-frame fence (slotFences_[1-frameSlot_]). This defers the fence
    //   wait to the END of decode so command recording proceeds without stalling.
    // ---------------------------------------------------------------------------
    struct StagedWrite {
        uint64_t    memId;
        VkDeviceSize offset;
        uint32_t    size;
        std::vector<uint8_t> data;
    };
    std::vector<StagedWrite> stagedWrites_;

    // Per-guest-CB: two host VkCommandBuffers (one per slot).
    std::unordered_map<uint64_t, std::array<VkCommandBuffer, 2>> cbDoubleBuffer_;
    // Pool each guest CB was allocated from (for on-demand slot-1 allocation).
    std::unordered_map<uint64_t, VkCommandPool> cbPoolMap_;

    // Current frame slot (0 or 1, toggled at the start of each execute()).
    int  frameSlot_        = 0;
    // Per-slot fence: set after flushPendingSubmits, waited before reusing that slot's CBs.
    VkFence slotFences_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Last fence waited per slot; re-wait when slotFences_[s] changes (intra-batch submit).
    VkFence slotFenceLastWaited_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

public:
    // BDA query results: accumulated during execute(), consumed by server
    struct BdaResult { uint64_t bufferId; uint64_t address; };
    std::vector<BdaResult> pendingBdaResults_;

    // Buffer usage tracking for auto-BDA on BindBufferMemory
    std::unordered_map<uint64_t, uint32_t> bufferUsageFlags_;

    // BDA replay patching: map live GPU addresses → replay GPU addresses
    // Populated when RecordBDA command is processed after BindBufferMemory (AutoBDA).
    std::unordered_map<uint64_t, uint64_t> replayBdaByBufferId_; // bufId → replay GPU addr
    std::unordered_map<uint64_t, uint64_t> liveBdaToReplayBda_;  // live addr → replay addr

    // Roundtrip timing: seqId from the most recent TimingSeq command in this batch
    uint32_t currentSeqId_ = 0;
    uint64_t batchRecvUs_ = 0;  // set by server before execute()

    // Cross-batch fence: wait for previous batch's GPU work before WriteMemory.
    // Prevents GPU still reading transform data when CPU overwrites it for next batch.
    VkFence lastBatchFence_ = VK_NULL_HANDLE;  // last fence submitted in previous batch
    bool    lastBatchWaitPending_ = false;      // true until first WriteMemory this batch waits it

    // Frame-level timing: tracks each present through the pipeline
    uint32_t frameCounter_ = 0; // monotonic, incremented per present
    struct FrameTiming {
        uint32_t frameId = 0;
        uint64_t presentUs = 0;   // when vkQueuePresentKHR was called
        uint64_t readbackUs = 0;  // when readback fence was waited (data CPU-accessible)
    };
    // Per-slot timing (matches readback_[2] double buffer)
    FrameTiming slotTiming_[2];
    // The timing for the frame whose pixels are currently in rbReady_
    FrameTiming readyFrameTiming_;
};

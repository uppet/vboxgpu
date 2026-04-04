#pragma once

// Minimal Venus-compatible command encoder.
// Encodes Vulkan-like calls into a binary command stream.
// Handles are represented as uint64_t IDs assigned by the caller.

#include "vn_command.h"
#include "vn_stream.h"
#include <mutex>

class VnEncoder {
public:
    // Lock for thread safety (DXVK is multithreaded)
    std::mutex mutex_;
    // Helper: lock must be held for entire beginCommand→endCommand sequence
    #define ENC_GUARD std::lock_guard<std::mutex> _lk(mutex_)
    // --- Instance / Device ---

    void cmdCreateRenderPass(uint64_t deviceId, uint64_t renderPassId,
                             uint32_t attachmentCount, const uint32_t* formats,
                             const uint32_t* loadOps, const uint32_t* storeOps,
                             const uint32_t* initialLayouts, const uint32_t* finalLayouts) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateRenderPass);
        w_.writeU64(deviceId);
        w_.writeU64(renderPassId);
        w_.writeU32(attachmentCount);
        for (uint32_t i = 0; i < attachmentCount; i++) {
            w_.writeU32(formats[i]);
            w_.writeU32(loadOps[i]);
            w_.writeU32(storeOps[i]);
            w_.writeU32(initialLayouts[i]);
            w_.writeU32(finalLayouts[i]);
        }
        w_.endCommand(off);
    }

    void cmdCreateShaderModule(uint64_t deviceId, uint64_t moduleId,
                               const uint32_t* spirvCode, size_t spirvSizeBytes) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateShaderModule);
        w_.writeU64(deviceId);
        w_.writeU64(moduleId);
        w_.writeU32(static_cast<uint32_t>(spirvSizeBytes));
        w_.writeBytes(spirvCode, spirvSizeBytes);
        w_.endCommand(off);
    }

    void cmdCreatePipelineLayout(uint64_t deviceId, uint64_t layoutId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreatePipelineLayout);
        w_.writeU64(deviceId);
        w_.writeU64(layoutId);
        w_.endCommand(off);
    }

    void cmdCreateGraphicsPipeline(uint64_t deviceId, uint64_t pipelineId,
                                   uint64_t renderPassId, uint64_t layoutId,
                                   uint64_t vertModuleId, uint64_t fragModuleId,
                                   uint32_t viewportWidth, uint32_t viewportHeight,
                                   uint32_t colorAttachmentFormat = 0) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateGraphicsPipelines);
        w_.writeU64(deviceId);
        w_.writeU64(pipelineId);
        w_.writeU64(renderPassId);
        w_.writeU64(layoutId);
        w_.writeU64(vertModuleId);
        w_.writeU64(fragModuleId);
        w_.writeU32(viewportWidth);
        w_.writeU32(viewportHeight);
        w_.writeU32(colorAttachmentFormat); // 0 = use renderPass, nonzero = dynamic rendering format
        w_.endCommand(off);
    }

    void cmdCreateFramebuffer(uint64_t deviceId, uint64_t framebufferId,
                              uint64_t renderPassId, uint64_t imageViewId,
                              uint32_t width, uint32_t height) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateFramebuffer);
        w_.writeU64(deviceId);
        w_.writeU64(framebufferId);
        w_.writeU64(renderPassId);
        w_.writeU64(imageViewId);
        w_.writeU32(width);
        w_.writeU32(height);
        w_.endCommand(off);
    }

    void cmdCreateCommandPool(uint64_t deviceId, uint64_t poolId, uint32_t queueFamily) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateCommandPool);
        w_.writeU64(deviceId);
        w_.writeU64(poolId);
        w_.writeU32(queueFamily);
        w_.endCommand(off);
    }

    void cmdAllocateCommandBuffers(uint64_t deviceId, uint64_t poolId,
                                   uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkAllocateCommandBuffers);
        w_.writeU64(deviceId);
        w_.writeU64(poolId);
        w_.writeU64(cmdBufferId);
        w_.endCommand(off);
    }

    void cmdBeginCommandBuffer(uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkBeginCommandBuffer);
        w_.writeU64(cmdBufferId);
        w_.endCommand(off);
    }

    void cmdEndCommandBuffer(uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkEndCommandBuffer);
        w_.writeU64(cmdBufferId);
        w_.endCommand(off);
    }

    void cmdBeginRenderPass(uint64_t cmdBufferId, uint64_t renderPassId,
                            uint64_t framebufferId,
                            uint32_t width, uint32_t height,
                            float clearR, float clearG, float clearB, float clearA) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBeginRenderPass);
        w_.writeU64(cmdBufferId);
        w_.writeU64(renderPassId);
        w_.writeU64(framebufferId);
        w_.writeU32(width);
        w_.writeU32(height);
        w_.writeF32(clearR);
        w_.writeF32(clearG);
        w_.writeF32(clearB);
        w_.writeF32(clearA);
        w_.endCommand(off);
    }

    void cmdEndRenderPass(uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdEndRenderPass);
        w_.writeU64(cmdBufferId);
        w_.endCommand(off);
    }

    // Vulkan 1.3 dynamic rendering — Host uses current swapchain image view
    void cmdBeginRendering(uint64_t cmdBufferId,
                           uint32_t renderAreaX, uint32_t renderAreaY,
                           uint32_t renderAreaW, uint32_t renderAreaH,
                           uint32_t loadOp, uint32_t storeOp,
                           float clearR, float clearG, float clearB, float clearA) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBeginRendering);
        w_.writeU64(cmdBufferId);
        w_.writeU32(renderAreaX);
        w_.writeU32(renderAreaY);
        w_.writeU32(renderAreaW);
        w_.writeU32(renderAreaH);
        w_.writeU32(loadOp);
        w_.writeU32(storeOp);
        w_.writeF32(clearR);
        w_.writeF32(clearG);
        w_.writeF32(clearB);
        w_.writeF32(clearA);
        w_.endCommand(off);
    }

    void cmdEndRendering(uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdEndRendering);
        w_.writeU64(cmdBufferId);
        w_.endCommand(off);
    }

    void cmdBindPipeline(uint64_t cmdBufferId, uint64_t pipelineId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBindPipeline);
        w_.writeU64(cmdBufferId);
        w_.writeU64(pipelineId);
        w_.endCommand(off);
    }

    void cmdSetViewport(uint64_t cmdBufferId,
                        float x, float y, float w, float h, float minD, float maxD) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdSetViewport);
        w_.writeU64(cmdBufferId);
        w_.writeF32(x); w_.writeF32(y);
        w_.writeF32(w); w_.writeF32(h);
        w_.writeF32(minD); w_.writeF32(maxD);
        w_.endCommand(off);
    }

    void cmdSetScissor(uint64_t cmdBufferId,
                       int32_t x, int32_t y, uint32_t w, uint32_t h) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdSetScissor);
        w_.writeU64(cmdBufferId);
        w_.writeI32(x); w_.writeI32(y);
        w_.writeU32(w); w_.writeU32(h);
        w_.endCommand(off);
    }

    void cmdDraw(uint64_t cmdBufferId,
                 uint32_t vertexCount, uint32_t instanceCount,
                 uint32_t firstVertex, uint32_t firstInstance) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdDraw);
        w_.writeU64(cmdBufferId);
        w_.writeU32(vertexCount);
        w_.writeU32(instanceCount);
        w_.writeU32(firstVertex);
        w_.writeU32(firstInstance);
        w_.endCommand(off);
    }

    // --- Sync ---

    void cmdCreateSemaphore(uint64_t deviceId, uint64_t semId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateSemaphore);
        w_.writeU64(deviceId);
        w_.writeU64(semId);
        w_.endCommand(off);
    }

    void cmdCreateFence(uint64_t deviceId, uint64_t fenceId, uint32_t flags) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateFence);
        w_.writeU64(deviceId);
        w_.writeU64(fenceId);
        w_.writeU32(flags);
        w_.endCommand(off);
    }

    // --- Swapchain (bridge-specific) ---

    void cmdBridgeCreateSwapchain(uint64_t deviceId, uint64_t swapchainId,
                                  uint32_t width, uint32_t height,
                                  uint32_t imageCount) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_BRIDGE_CreateSwapchain);
        w_.writeU64(deviceId);
        w_.writeU64(swapchainId);
        w_.writeU32(width);
        w_.writeU32(height);
        w_.writeU32(imageCount);
        w_.endCommand(off);
    }

    void cmdBridgeAcquireNextImage(uint64_t swapchainId, uint64_t semaphoreId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_BRIDGE_AcquireNextImage);
        w_.writeU64(swapchainId);
        w_.writeU64(semaphoreId);
        w_.endCommand(off);
    }

    void cmdBridgeQueuePresent(uint64_t queueId, uint64_t swapchainId,
                               uint64_t waitSemaphoreId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_BRIDGE_QueuePresent);
        w_.writeU64(queueId);
        w_.writeU64(swapchainId);
        w_.writeU64(waitSemaphoreId);
        w_.endCommand(off);
    }

    void cmdQueueSubmit(uint64_t queueId, uint64_t cmdBufferId,
                        uint64_t waitSemaphoreId, uint64_t signalSemaphoreId,
                        uint64_t fenceId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkQueueSubmit);
        w_.writeU64(queueId);
        w_.writeU64(cmdBufferId);
        w_.writeU64(waitSemaphoreId);
        w_.writeU64(signalSemaphoreId);
        w_.writeU64(fenceId);
        w_.endCommand(off);
    }

    void cmdWaitForFences(uint64_t deviceId, uint64_t fenceId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkWaitForFences);
        w_.writeU64(deviceId);
        w_.writeU64(fenceId);
        w_.endCommand(off);
    }

    void cmdResetFences(uint64_t deviceId, uint64_t fenceId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkResetFences);
        w_.writeU64(deviceId);
        w_.writeU64(fenceId);
        w_.endCommand(off);
    }

    // Unlocked version — caller must hold mutex_
    void cmdEndOfStreamUnlocked() {
        auto off = w_.beginCommand(VN_CMD_BRIDGE_EndOfStream);
        w_.endCommand(off);
    }

    void cmdEndOfStream() {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_BRIDGE_EndOfStream);
        w_.endCommand(off);
    }

    const uint8_t* data() const { return w_.data(); }
    size_t size() const { return w_.size(); }

    // Public for sendAndRecv to reset
    VnStreamWriter w_;
};

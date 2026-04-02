#include "vk_renderer.h"

void createCommandPool(const VulkanContext& vk, RendererContext& ren) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = vk.graphicsFamily;
    if (vkCreateCommandPool(vk.device, &poolInfo, nullptr, &ren.commandPool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create command pool");
}

void createCommandBuffers(const VulkanContext& vk, const PipelineContext& pip, RendererContext& ren) {
    ren.commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ren.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(ren.commandBuffers.size());
    if (vkAllocateCommandBuffers(vk.device, &allocInfo, ren.commandBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate command buffers");
}

void createSyncObjects(const VulkanContext& vk, RendererContext& ren) {
    ren.imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    ren.renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    ren.inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(vk.device, &semInfo, nullptr, &ren.imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(vk.device, &semInfo, nullptr, &ren.renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(vk.device, &fenceInfo, nullptr, &ren.inFlightFences[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sync objects");
    }
}

static void recordCommandBuffer(VkCommandBuffer cmd, const VulkanContext& vk,
                                const PipelineContext& pip, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = pip.renderPass;
    rpBegin.framebuffer = pip.framebuffers[imageIndex];
    rpBegin.renderArea.offset = { 0, 0 };
    rpBegin.renderArea.extent = vk.swapchainExtent;
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pip.graphicsPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);
}

void drawFrame(const VulkanContext& vk, const PipelineContext& pip, RendererContext& ren) {
    vkWaitForFences(vk.device, 1, &ren.inFlightFences[ren.currentFrame], VK_TRUE, UINT64_MAX);
    vkResetFences(vk.device, 1, &ren.inFlightFences[ren.currentFrame]);

    uint32_t imageIndex;
    vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                          ren.imageAvailableSemaphores[ren.currentFrame], VK_NULL_HANDLE, &imageIndex);

    vkResetCommandBuffer(ren.commandBuffers[ren.currentFrame], 0);
    recordCommandBuffer(ren.commandBuffers[ren.currentFrame], vk, pip, imageIndex);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &ren.imageAvailableSemaphores[ren.currentFrame];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &ren.commandBuffers[ren.currentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &ren.renderFinishedSemaphores[ren.currentFrame];

    if (vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, ren.inFlightFences[ren.currentFrame]) != VK_SUCCESS)
        throw std::runtime_error("Failed to submit draw command buffer");

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &ren.renderFinishedSemaphores[ren.currentFrame];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vk.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    vkQueuePresentKHR(vk.presentQueue, &presentInfo);

    ren.currentFrame = (ren.currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void cleanupRenderer(VkDevice device, RendererContext& ren) {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device, ren.renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(device, ren.imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, ren.inFlightFences[i], nullptr);
    }
    if (ren.commandPool) vkDestroyCommandPool(device, ren.commandPool, nullptr);
}

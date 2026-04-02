#pragma once

#include "vk_pipeline.h"

constexpr int MAX_FRAMES_IN_FLIGHT = 2;

struct RendererContext {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;
};

void createCommandPool(const VulkanContext& vk, RendererContext& ren);
void createCommandBuffers(const VulkanContext& vk, const PipelineContext& pip, RendererContext& ren);
void createSyncObjects(const VulkanContext& vk, RendererContext& ren);
void drawFrame(const VulkanContext& vk, const PipelineContext& pip, RendererContext& ren);
void cleanupRenderer(VkDevice device, RendererContext& ren);

#pragma once

#include "vk_bootstrap.h"

struct PipelineContext {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
};

void createRenderPass(const VulkanContext& vk, PipelineContext& pip);
void createGraphicsPipeline(const VulkanContext& vk, PipelineContext& pip);
void createFramebuffers(const VulkanContext& vk, PipelineContext& pip);
void cleanupPipeline(VkDevice device, PipelineContext& pip);

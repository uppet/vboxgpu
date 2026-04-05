#pragma once

// Venus-compatible command type IDs (matching VkCommandTypeEXT from venus-protocol)
// Only the subset needed for triangle rendering is listed here.

#include <cstdint>
#include <cstring>

enum VnCommandType : uint32_t {
    VN_CMD_vkCreateInstance                = 0,
    VN_CMD_vkDestroyInstance               = 1,
    VN_CMD_vkEnumeratePhysicalDevices      = 2,
    VN_CMD_vkGetPhysicalDeviceProperties   = 6,
    VN_CMD_vkGetPhysicalDeviceQueueFamilyProperties = 7,
    VN_CMD_vkGetPhysicalDeviceMemoryProperties = 8,
    VN_CMD_vkCreateDevice                  = 11,
    VN_CMD_vkDestroyDevice                 = 12,
    VN_CMD_vkGetDeviceQueue                = 17,
    VN_CMD_vkQueueSubmit                   = 18,
    VN_CMD_vkDeviceWaitIdle                = 20,
    VN_CMD_vkAllocateMemory                = 21,
    VN_CMD_vkFreeMemory                    = 22,
    VN_CMD_vkMapMemory                     = 23,
    VN_CMD_vkUnmapMemory                   = 24,
    VN_CMD_vkCreateFence                   = 33,
    VN_CMD_vkDestroyFence                  = 34,
    VN_CMD_vkResetFences                   = 35,
    VN_CMD_vkWaitForFences                 = 37,
    VN_CMD_vkCreateSemaphore               = 38,
    VN_CMD_vkDestroySemaphore              = 39,
    VN_CMD_vkCreateBuffer                  = 46,
    VN_CMD_vkDestroyBuffer                 = 47,
    VN_CMD_vkCreateImage                   = 50,
    VN_CMD_vkDestroyImage                  = 51,
    VN_CMD_vkCreateImageView               = 52,
    VN_CMD_vkDestroyImageView              = 53,
    VN_CMD_vkCreateShaderModule            = 54,
    VN_CMD_vkDestroyShaderModule           = 55,
    VN_CMD_vkCreatePipelineLayout          = 58,
    VN_CMD_vkDestroyPipelineLayout         = 59,
    VN_CMD_vkCreateGraphicsPipelines       = 61,
    VN_CMD_vkDestroyPipeline               = 63,
    VN_CMD_vkCreateRenderPass              = 67,
    VN_CMD_vkDestroyRenderPass             = 68,
    VN_CMD_vkCreateFramebuffer             = 69,
    VN_CMD_vkDestroyFramebuffer            = 70,
    VN_CMD_vkCreateCommandPool             = 71,
    VN_CMD_vkDestroyCommandPool            = 72,
    VN_CMD_vkAllocateCommandBuffers        = 73,
    VN_CMD_vkBeginCommandBuffer            = 75,
    VN_CMD_vkEndCommandBuffer              = 76,
    VN_CMD_vkCmdBindPipeline               = 78,
    VN_CMD_vkCmdSetViewport                = 79,
    VN_CMD_vkCmdSetScissor                 = 80,
    VN_CMD_vkCmdDraw                       = 86,
    VN_CMD_vkCmdBeginRenderPass            = 94,
    VN_CMD_vkCmdEndRenderPass              = 96,
    VN_CMD_vkBindBufferMemory              = 44,
    VN_CMD_vkBindImageMemory               = 48,

    VN_CMD_vkCmdPushConstants              = 87,

    // Vulkan 1.3 dynamic rendering
    VN_CMD_vkCmdBeginRendering             = 0x1000,  // bridge-defined (not in Venus)
    VN_CMD_vkCmdEndRendering               = 0x1001,

    // Extension: swapchain (handled specially by host)
    VN_CMD_BRIDGE_CreateSwapchain          = 0x10000,
    VN_CMD_BRIDGE_AcquireNextImage         = 0x10001,
    VN_CMD_BRIDGE_QueuePresent             = 0x10002,
    VN_CMD_BRIDGE_EndOfStream              = 0x1FFFF,
};

// Command flags (Venus-compatible)
enum VnCommandFlags : uint32_t {
    VN_CMD_FLAG_NONE = 0,
};

// All data in the stream is 4-byte aligned, little-endian.
// A command in the stream:
//   [uint32_t cmd_type]
//   [uint32_t cmd_size]    // total bytes including header (we add this for framing)
//   [payload...]           // 4-byte aligned parameters
//
// Handles are encoded as uint64_t IDs (assigned by encoder, mapped by decoder).
// Pointers: uint64_t (0 = NULL, non-zero = present, followed by data).

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
    VN_CMD_vkBindBufferMemory              = 28,   // Venus standard (was 44)
    VN_CMD_vkBindImageMemory               = 29,   // Venus standard (was 48)
    VN_CMD_vkCreateFence                   = 33,
    VN_CMD_vkDestroyFence                  = 36,   // Venus standard (was 34)
    VN_CMD_vkResetFences                   = 37,   // Venus standard
    VN_CMD_vkWaitForFences                 = 39,   // Venus standard (was 37)
    VN_CMD_vkCreateSemaphore               = 40,   // Venus standard (was 38)
    VN_CMD_vkDestroySemaphore              = 41,   // Venus standard (was 39)
    VN_CMD_vkCreateBuffer                  = 50,   // Venus standard (was 46)
    VN_CMD_vkDestroyBuffer                 = 51,   // Venus standard (was 47)
    VN_CMD_vkCreateImage                   = 54,   // Venus standard (was 50)
    VN_CMD_vkDestroyImage                  = 55,   // Venus standard (was 51)
    VN_CMD_vkCreateImageView               = 57,   // Venus standard (was 52)
    VN_CMD_vkDestroyImageView              = 58,   // Venus standard (was 53)
    VN_CMD_vkCreateShaderModule            = 59,   // Venus standard (was 54)
    VN_CMD_vkDestroyShaderModule           = 60,   // Venus standard (was 55)
    VN_CMD_vkCreateDescriptorSetLayout     = 72,   // Venus standard (was 56)
    VN_CMD_vkDestroyDescriptorSetLayout    = 73,   // Venus standard (new)
    VN_CMD_vkCreatePipelineLayout          = 68,   // Venus standard (was 58)
    VN_CMD_vkDestroyPipelineLayout         = 69,   // Venus standard (was 59)
    VN_CMD_vkCreateSampler                 = 70,   // Venus standard (was 0x1002)
    VN_CMD_vkDestroySampler                = 71,   // Venus standard (new)
    VN_CMD_vkCreateGraphicsPipelines       = 65,   // Venus standard (was 61)
    VN_CMD_vkDestroyPipeline               = 67,   // Venus standard (was 63)
    VN_CMD_vkCreateDescriptorPool          = 74,   // Venus standard (was 0x1003)
    VN_CMD_vkDestroyDescriptorPool         = 75,   // Venus standard (new)
    VN_CMD_vkCreateRenderPass              = 82,   // Venus standard (was 67)
    VN_CMD_vkDestroyRenderPass             = 83,   // Venus standard (was 68)
    VN_CMD_vkCreateFramebuffer             = 80,   // Venus standard (was 69)
    VN_CMD_vkDestroyFramebuffer            = 81,   // Venus standard (was 70)
    VN_CMD_vkCreateCommandPool             = 85,   // Venus standard (was 71)
    VN_CMD_vkDestroyCommandPool            = 86,   // Venus standard (was 72)
    VN_CMD_vkAllocateCommandBuffers        = 88,   // Venus standard (was 73)
    VN_CMD_vkBeginCommandBuffer            = 90,   // Venus standard (was 75)
    VN_CMD_vkEndCommandBuffer              = 91,   // Venus standard (was 76)
    VN_CMD_vkCmdBindPipeline               = 93,   // Venus standard (was 78)
    VN_CMD_vkCmdSetViewport                = 94,   // Venus standard (was 79)
    VN_CMD_vkCmdSetScissor                 = 95,   // Venus standard (was 80)
    VN_CMD_vkCmdDraw                       = 106,  // Venus standard (was 86)
    VN_CMD_vkCmdBeginRenderPass            = 133,  // Venus standard (was 94)
    VN_CMD_vkCmdEndRenderPass              = 135,  // Venus standard (was 96)
    VN_CMD_vkCmdPushConstants              = 132,  // Venus standard (was 87)
    VN_CMD_vkCmdBindIndexBuffer            = 104,  // Venus standard (was 0x100E)

    // Vulkan 1.3 dynamic rendering
    VN_CMD_vkCmdBeginRendering             = 0x1000,  // bridge-defined (not yet Venus)
    VN_CMD_vkCmdEndRendering               = 214,  // Venus standard (was 0x1001)

    // Bridge-defined resource commands (custom wire format, not yet in codegen)
    VN_CMD_vkAllocateDescriptorSets        = 0x1004,
    VN_CMD_vkUpdateDescriptorSets          = 0x1005,
    VN_CMD_vkCmdBindDescriptorSets         = 103,   // Venus standard (was 0x1006)
    VN_CMD_vkCmdPushDescriptorSet         = 0x1007,
    VN_CMD_vkCmdPipelineBarrier2          = 0x1008,  // image memory barriers only
    VN_CMD_vkCmdClearAttachments          = 0x1009,
    VN_CMD_vkCmdClearColorImage           = 0x100A,
    VN_CMD_vkCmdSetCullMode               = 215,   // Venus standard (was 0x100B)
    VN_CMD_vkCmdSetFrontFace              = 216,   // Venus standard (was 0x100C)
    VN_CMD_vkCmdBindVertexBuffers         = 0x100D,
    VN_CMD_vkCmdDrawIndexed               = 107,   // Venus standard (was 0x100F)
    VN_CMD_vkCmdCopyBuffer                = 0x1010,
    VN_CMD_vkCmdCopyBufferToImage         = 0x1011,
    VN_CMD_vkCmdUpdateBuffer              = 117,   // Venus standard (was 0x1012)

    // Extension: swapchain (handled specially by host)
    VN_CMD_BRIDGE_CreateSwapchain          = 0x10000,
    VN_CMD_BRIDGE_AcquireNextImage         = 0x10001,
    VN_CMD_BRIDGE_QueuePresent             = 0x10002,
    VN_CMD_BRIDGE_WriteMemory              = 0x10003,  // upload host-visible memory data
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

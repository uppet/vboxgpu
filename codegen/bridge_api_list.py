"""
API subset for bridge codegen.
Each entry maps a Vulkan command name to its Venus VkCommandTypeEXT value.
These IDs come from VK_EXT_command_serialization.xml.
"""

# Standard Vulkan APIs to generate encoder/decoder for.
# Format: "vkXxx": venus_command_id
CODEGEN_APIS = {
    # Instance/Device (not serialized in current flow, but reserve IDs)
    # Memory
    "vkAllocateMemory": 21,
    "vkFreeMemory": 22,
    "vkBindBufferMemory": 28,
    "vkBindImageMemory": 29,
    # Sync
    "vkCreateFence": 35,
    "vkDestroyFence": 36,
    "vkResetFences": 37,
    "vkWaitForFences": 39,
    "vkCreateSemaphore": 40,
    "vkDestroySemaphore": 41,
    # Resources
    "vkCreateBuffer": 50,
    "vkDestroyBuffer": 51,
    "vkCreateImage": 54,
    "vkDestroyImage": 55,
    "vkCreateImageView": 57,
    "vkDestroyImageView": 58,
    "vkCreateShaderModule": 59,
    "vkDestroyShaderModule": 60,
    # Pipeline
    "vkCreateGraphicsPipelines": 65,
    "vkDestroyPipeline": 67,
    "vkCreatePipelineLayout": 68,
    "vkDestroyPipelineLayout": 69,
    "vkCreateSampler": 70,
    "vkDestroySampler": 71,
    "vkCreateDescriptorSetLayout": 72,
    "vkDestroyDescriptorSetLayout": 73,
    "vkCreateDescriptorPool": 74,
    "vkDestroyDescriptorPool": 75,
    "vkAllocateDescriptorSets": 77,
    "vkUpdateDescriptorSets": 79,
    # Render pass / Framebuffer
    "vkCreateFramebuffer": 80,
    "vkDestroyFramebuffer": 81,
    "vkCreateRenderPass": 82,
    "vkDestroyRenderPass": 83,
    # Command pool / buffer
    "vkCreateCommandPool": 85,
    "vkDestroyCommandPool": 86,
    "vkAllocateCommandBuffers": 88,
    "vkBeginCommandBuffer": 90,
    "vkEndCommandBuffer": 91,
    # Command recording - pipeline
    "vkCmdBindPipeline": 93,
    "vkCmdSetViewport": 94,
    "vkCmdSetScissor": 95,
    # Command recording - descriptors
    "vkCmdBindDescriptorSets": 103,
    "vkCmdBindIndexBuffer": 104,
    "vkCmdBindVertexBuffers": 105,
    # Command recording - draw
    "vkCmdDraw": 106,
    "vkCmdDrawIndexed": 107,
    # Command recording - transfer
    "vkCmdCopyBuffer": 112,
    "vkCmdCopyBufferToImage": 115,
    "vkCmdUpdateBuffer": 117,
    # Command recording - clear
    "vkCmdClearColorImage": 119,
    "vkCmdClearAttachments": 121,
    # Command recording - barrier
    "vkCmdPipelineBarrier": 126,
    # Command recording - push constants
    "vkCmdPushConstants": 132,
    # Command recording - render pass
    "vkCmdBeginRenderPass": 133,
    "vkCmdEndRenderPass": 135,
    # Queue
    "vkQueueSubmit": 18,
    # Vulkan 1.3 dynamic rendering
    "vkCmdBeginRendering": 213,
    "vkCmdEndRendering": 214,
    "vkCmdSetCullMode": 215,
    "vkCmdSetFrontFace": 216,
    "vkCmdBindVertexBuffers2": 220,
}

# Bridge-specific commands (not in Vulkan spec, not code-generated)
BRIDGE_COMMANDS = {
    "BRIDGE_CreateSwapchain": 0x10000,
    "BRIDGE_AcquireNextImage": 0x10001,
    "BRIDGE_QueuePresent": 0x10002,
    "BRIDGE_WriteMemory": 0x10003,
    "BRIDGE_EndOfStream": 0x1FFFF,
}

#pragma once

// Minimal Venus-compatible command encoder.
// Encodes Vulkan-like calls into a binary command stream.
// Handles are represented as uint64_t IDs assigned by the caller.

#include "vn_command.h"
#include "vn_stream.h"
#include "vn_gen_encode.h"
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

#ifdef VK_VERSION_1_0 // Requires VkDescriptorSetLayoutBinding
    void cmdCreateDescriptorSetLayout(uint64_t deviceId, uint64_t layoutId,
                                       uint32_t bindingCount,
                                       const VkDescriptorSetLayoutBinding* pBindings) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateDescriptorSetLayout);
        w_.writeU64(deviceId);
        w_.writeU64(layoutId);
        w_.writeU32(bindingCount);
        for (uint32_t i = 0; i < bindingCount; i++) {
            w_.writeU32(pBindings[i].binding);
            w_.writeU32(pBindings[i].descriptorType);
            w_.writeU32(pBindings[i].descriptorCount);
            w_.writeU32(pBindings[i].stageFlags);
        }
        w_.endCommand(off);
    }

#endif // VK_VERSION_1_0

    // stageFlags/offset/size triplets packed as uint32_t arrays
    void cmdCreatePipelineLayout(uint64_t deviceId, uint64_t layoutId,
                                  uint32_t setLayoutCount, const uint64_t* setLayoutIds,
                                  uint32_t pushConstantRangeCount,
                                  const uint32_t* pushRangeData /* 3 u32 per range: stageFlags, offset, size */) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreatePipelineLayout);
        w_.writeU64(deviceId);
        w_.writeU64(layoutId);
        w_.writeU32(setLayoutCount);
        for (uint32_t i = 0; i < setLayoutCount; i++)
            w_.writeU64(setLayoutIds[i]);
        w_.writeU32(pushConstantRangeCount);
        for (uint32_t i = 0; i < pushConstantRangeCount; i++) {
            w_.writeU32(pushRangeData[i*3+0]); // stageFlags
            w_.writeU32(pushRangeData[i*3+1]); // offset
            w_.writeU32(pushRangeData[i*3+2]); // size
        }
        w_.endCommand(off);
    }

    // Vertex input binding/attribute info for pipeline creation
    struct VertexBinding { uint32_t binding, stride, inputRate; };
    struct VertexAttribute { uint32_t location, binding, format, offset; };

    // Legacy overload (no vertex input) for guest_sim / host_cmd compatibility
    void cmdCreateGraphicsPipeline(uint64_t deviceId, uint64_t pipelineId,
                                   uint64_t renderPassId, uint64_t layoutId,
                                   uint64_t vertModuleId, uint64_t fragModuleId,
                                   uint32_t viewportWidth, uint32_t viewportHeight,
                                   uint32_t colorAttachmentFormat) {
        cmdCreateGraphicsPipeline(deviceId, pipelineId, renderPassId, layoutId,
            vertModuleId, fragModuleId, viewportWidth, viewportHeight,
            colorAttachmentFormat, 0, nullptr, 0, nullptr);
    }

    void cmdCreateGraphicsPipeline(uint64_t deviceId, uint64_t pipelineId,
                                   uint64_t renderPassId, uint64_t layoutId,
                                   uint64_t vertModuleId, uint64_t fragModuleId,
                                   uint32_t viewportWidth, uint32_t viewportHeight,
                                   uint32_t colorAttachmentFormat,
                                   uint32_t bindingCount, const VertexBinding* bindings,
                                   uint32_t attributeCount, const VertexAttribute* attributes) {
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
        w_.writeU32(colorAttachmentFormat);
        // Vertex input state (appended after legacy fields)
        w_.writeU32(bindingCount);
        for (uint32_t i = 0; i < bindingCount; i++) {
            w_.writeU32(bindings[i].binding);
            w_.writeU32(bindings[i].stride);
            w_.writeU32(bindings[i].inputRate);
        }
        w_.writeU32(attributeCount);
        for (uint32_t i = 0; i < attributeCount; i++) {
            w_.writeU32(attributes[i].location);
            w_.writeU32(attributes[i].binding);
            w_.writeU32(attributes[i].format);
            w_.writeU32(attributes[i].offset);
        }
        w_.endCommand(off);
    }

    // --- GPU Resource creation ---

    void cmdCreateImage(uint64_t deviceId, uint64_t imageId,
                        uint32_t imageType, uint32_t format,
                        uint32_t width, uint32_t height, uint32_t depth,
                        uint32_t mipLevels, uint32_t arrayLayers, uint32_t samples,
                        uint32_t tiling, uint32_t usage) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateImage);
        w_.writeU64(deviceId); w_.writeU64(imageId);
        w_.writeU32(imageType); w_.writeU32(format);
        w_.writeU32(width); w_.writeU32(height); w_.writeU32(depth);
        w_.writeU32(mipLevels); w_.writeU32(arrayLayers); w_.writeU32(samples);
        w_.writeU32(tiling); w_.writeU32(usage);
        w_.endCommand(off);
    }

    void cmdAllocateMemory(uint64_t deviceId, uint64_t memoryId,
                           uint64_t allocationSize, uint32_t memoryTypeIndex) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkAllocateMemory);
        w_.writeU64(deviceId); w_.writeU64(memoryId);
        w_.writeU64(allocationSize); w_.writeU32(memoryTypeIndex);
        w_.endCommand(off);
    }

    void cmdBindImageMemory(uint64_t deviceId, uint64_t imageId,
                            uint64_t memoryId, uint64_t memoryOffset) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkBindImageMemory);
        vn_encode_vkBindImageMemory(&w_, deviceId, imageId, memoryId, memoryOffset);
        w_.endCommand(off);
    }

    void cmdCreateBuffer(uint64_t deviceId, uint64_t bufferId,
                         uint64_t size, uint32_t usage) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateBuffer);
        w_.writeU64(deviceId); w_.writeU64(bufferId);
        w_.writeU64(size); w_.writeU32(usage);
        w_.endCommand(off);
    }

    void cmdBindBufferMemory(uint64_t deviceId, uint64_t bufferId,
                             uint64_t memoryId, uint64_t memoryOffset) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkBindBufferMemory);
        vn_encode_vkBindBufferMemory(&w_, deviceId, bufferId, memoryId, memoryOffset);
        w_.endCommand(off);
    }

    void cmdWriteMemory(uint64_t memoryId, uint64_t offset,
                        uint32_t size, const void* data) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_BRIDGE_WriteMemory);
        w_.writeU64(memoryId);
        w_.writeU64(offset);
        w_.writeU32(size);
        w_.writeBytes(data, size);
        w_.endCommand(off);
    }

    void cmdCreateImageView(uint64_t deviceId, uint64_t viewId, uint64_t imageId,
                            uint32_t viewType, uint32_t format,
                            uint32_t compR, uint32_t compG, uint32_t compB, uint32_t compA,
                            uint32_t aspectMask, uint32_t baseMipLevel, uint32_t levelCount,
                            uint32_t baseArrayLayer, uint32_t layerCount) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateImageView);
        w_.writeU64(deviceId); w_.writeU64(viewId); w_.writeU64(imageId);
        w_.writeU32(viewType); w_.writeU32(format);
        w_.writeU32(compR); w_.writeU32(compG); w_.writeU32(compB); w_.writeU32(compA);
        w_.writeU32(aspectMask); w_.writeU32(baseMipLevel); w_.writeU32(levelCount);
        w_.writeU32(baseArrayLayer); w_.writeU32(layerCount);
        w_.endCommand(off);
    }

#ifdef VK_VERSION_1_0 // These methods require Vulkan types (VkSamplerCreateInfo etc.)
    void cmdCreateSampler(uint64_t deviceId, uint64_t samplerId,
                          const VkSamplerCreateInfo* ci) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateSampler);
        w_.writeU64(deviceId); w_.writeU64(samplerId);
        w_.writeU32(ci->magFilter); w_.writeU32(ci->minFilter); w_.writeU32(ci->mipmapMode);
        w_.writeU32(ci->addressModeU); w_.writeU32(ci->addressModeV); w_.writeU32(ci->addressModeW);
        w_.writeF32(ci->mipLodBias);
        w_.writeU32(ci->anisotropyEnable); w_.writeF32(ci->maxAnisotropy);
        w_.writeU32(ci->compareEnable); w_.writeU32(ci->compareOp);
        w_.writeF32(ci->minLod); w_.writeF32(ci->maxLod);
        w_.writeU32(ci->borderColor); w_.writeU32(ci->unnormalizedCoordinates);
        w_.endCommand(off);
    }

    void cmdCreateDescriptorPool(uint64_t deviceId, uint64_t poolId,
                                  uint32_t flags, uint32_t maxSets,
                                  uint32_t poolSizeCount,
                                  const VkDescriptorPoolSize* pPoolSizes) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCreateDescriptorPool);
        w_.writeU64(deviceId); w_.writeU64(poolId);
        w_.writeU32(flags); w_.writeU32(maxSets); w_.writeU32(poolSizeCount);
        for (uint32_t i = 0; i < poolSizeCount; i++) {
            w_.writeU32(pPoolSizes[i].type);
            w_.writeU32(pPoolSizes[i].descriptorCount);
        }
        w_.endCommand(off);
    }

    void cmdAllocateDescriptorSets(uint64_t deviceId, uint64_t poolId,
                                    uint32_t setCount,
                                    const uint64_t* layoutIds,
                                    const uint64_t* setIds) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkAllocateDescriptorSets);
        w_.writeU64(deviceId); w_.writeU64(poolId); w_.writeU32(setCount);
        for (uint32_t i = 0; i < setCount; i++) {
            w_.writeU64(layoutIds[i]); w_.writeU64(setIds[i]);
        }
        w_.endCommand(off);
    }

    void cmdUpdateDescriptorSets(uint64_t deviceId, uint32_t writeCount,
                                  const VkWriteDescriptorSet* pWrites) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkUpdateDescriptorSets);
        w_.writeU64(deviceId); w_.writeU32(writeCount);
        for (uint32_t i = 0; i < writeCount; i++) {
            w_.writeU64((uint64_t)pWrites[i].dstSet);
            w_.writeU32(pWrites[i].dstBinding);
            w_.writeU32(pWrites[i].dstArrayElement);
            w_.writeU32(pWrites[i].descriptorCount);
            w_.writeU32(pWrites[i].descriptorType);
            for (uint32_t j = 0; j < pWrites[i].descriptorCount; j++) {
                // Encode image info (sampler + imageView + layout)
                uint64_t samId = 0, ivId = 0; uint32_t layout = 0;
                if (pWrites[i].pImageInfo) {
                    samId = (uint64_t)pWrites[i].pImageInfo[j].sampler;
                    ivId = (uint64_t)pWrites[i].pImageInfo[j].imageView;
                    layout = pWrites[i].pImageInfo[j].imageLayout;
                }
                w_.writeU64(samId); w_.writeU64(ivId); w_.writeU32(layout);
                // Encode buffer info
                uint64_t bufId = 0; uint64_t bufOff = 0, bufRange = 0;
                if (pWrites[i].pBufferInfo) {
                    bufId = (uint64_t)pWrites[i].pBufferInfo[j].buffer;
                    bufOff = pWrites[i].pBufferInfo[j].offset;
                    bufRange = pWrites[i].pBufferInfo[j].range;
                }
                w_.writeU64(bufId); w_.writeU64(bufOff); w_.writeU64(bufRange);
            }
        }
        w_.endCommand(off);
    }

#endif // VK_VERSION_1_0

    void cmdBindDescriptorSets(uint64_t cbId, uint32_t bindPoint, uint64_t layoutId,
                                uint32_t firstSet, uint32_t setCount, const uint64_t* setIds,
                                uint32_t dynamicOffsetCount, const uint32_t* dynamicOffsets) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBindDescriptorSets);
        w_.writeU64(cbId); w_.writeU32(bindPoint); w_.writeU64(layoutId);
        w_.writeU32(firstSet); w_.writeU32(setCount);
        for (uint32_t i = 0; i < setCount; i++) w_.writeU64(setIds[i]);
        w_.writeU32(dynamicOffsetCount);
        for (uint32_t i = 0; i < dynamicOffsetCount; i++) w_.writeU32(dynamicOffsets[i]);
        w_.endCommand(off);
    }

    // Push descriptor set: encode writes directly into command stream
    // (used by vkCmdPushDescriptorSetKHR / vkCmdPushDescriptorSetWithTemplateKHR)
    void cmdPushDescriptorSet(uint64_t cbId, uint32_t bindPoint, uint64_t layoutId,
                               uint32_t set, uint32_t writeCount,
                               const uint64_t* dstBindings,    // [writeCount] binding indices
                               const uint32_t* descriptorCounts,
                               const uint32_t* descriptorTypes,
                               const uint64_t* samplerIds,     // per descriptor
                               const uint64_t* imageViewIds,
                               const uint32_t* imageLayouts,
                               const uint64_t* bufferIds,      // per descriptor (0 if image)
                               const uint64_t* bufferOffsets,
                               const uint64_t* bufferRanges,
                               uint32_t totalDescriptors) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdPushDescriptorSet);
        w_.writeU64(cbId); w_.writeU32(bindPoint); w_.writeU64(layoutId); w_.writeU32(set);
        w_.writeU32(writeCount);
        uint32_t descIdx = 0;
        for (uint32_t i = 0; i < writeCount; i++) {
            w_.writeU32((uint32_t)dstBindings[i]);
            w_.writeU32(descriptorCounts[i]);
            w_.writeU32(descriptorTypes[i]);
            for (uint32_t j = 0; j < descriptorCounts[i]; j++) {
                w_.writeU64(samplerIds[descIdx]);
                w_.writeU64(imageViewIds[descIdx]);
                w_.writeU32(imageLayouts[descIdx]);
                w_.writeU64(bufferIds[descIdx]);
                w_.writeU64(bufferOffsets[descIdx]);
                w_.writeU64(bufferRanges[descIdx]);
                descIdx++;
            }
        }
        w_.endCommand(off);
    }

    void cmdPipelineBarrier(uint64_t cbId,
                            uint32_t srcStage, uint32_t dstStage,
                            uint32_t imageBarrierCount,
                            const uint64_t* images, const uint32_t* oldLayouts, const uint32_t* newLayouts,
                            const uint32_t* srcAccess, const uint32_t* dstAccess) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdPipelineBarrier2);
        w_.writeU64(cbId);
        w_.writeU32(srcStage); w_.writeU32(dstStage);
        w_.writeU32(imageBarrierCount);
        for (uint32_t i = 0; i < imageBarrierCount; i++) {
            w_.writeU64(images[i]);
            w_.writeU32(oldLayouts[i]); w_.writeU32(newLayouts[i]);
            w_.writeU32(srcAccess[i]); w_.writeU32(dstAccess[i]);
        }
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
        vn_encode_vkEndCommandBuffer(&w_, cmdBufferId);
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
        vn_encode_vkCmdEndRenderPass(&w_, cmdBufferId);
        w_.endCommand(off);
    }

    // Vulkan 1.3 dynamic rendering — Host uses current swapchain image view
    void cmdBeginRendering(uint64_t cmdBufferId,
                           uint32_t renderAreaX, uint32_t renderAreaY,
                           uint32_t renderAreaW, uint32_t renderAreaH,
                           uint32_t loadOp, uint32_t storeOp,
                           float clearR, float clearG, float clearB, float clearA,
                           uint64_t imageViewId = 0) {
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
        w_.writeU64(imageViewId); // 0 = use swapchain (legacy), nonzero = specific view
        w_.endCommand(off);
    }

    void cmdEndRendering(uint64_t cmdBufferId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdEndRendering);
        vn_encode_vkCmdEndRendering(&w_, cmdBufferId);
        w_.endCommand(off);
    }

#ifdef VK_VERSION_1_0 // Requires VkClearAttachment, VkClearRect
    void cmdClearAttachments(uint64_t cmdBufferId,
                             uint32_t attachmentCount, const VkClearAttachment* pAttachments,
                             uint32_t rectCount, const VkClearRect* pRects) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdClearAttachments);
        w_.writeU64(cmdBufferId);
        w_.writeU32(attachmentCount);
        for (uint32_t i = 0; i < attachmentCount; i++) {
            w_.writeU32(pAttachments[i].aspectMask);
            w_.writeU32(pAttachments[i].colorAttachment);
            w_.writeF32(pAttachments[i].clearValue.color.float32[0]);
            w_.writeF32(pAttachments[i].clearValue.color.float32[1]);
            w_.writeF32(pAttachments[i].clearValue.color.float32[2]);
            w_.writeF32(pAttachments[i].clearValue.color.float32[3]);
        }
        w_.writeU32(rectCount);
        for (uint32_t i = 0; i < rectCount; i++) {
            w_.writeU32(pRects[i].rect.offset.x);
            w_.writeU32(pRects[i].rect.offset.y);
            w_.writeU32(pRects[i].rect.extent.width);
            w_.writeU32(pRects[i].rect.extent.height);
            w_.writeU32(pRects[i].baseArrayLayer);
            w_.writeU32(pRects[i].layerCount);
        }
        w_.endCommand(off);
    }

    void cmdClearColorImage(uint64_t cmdBufferId, uint64_t imageId,
                            uint32_t imageLayout,
                            float r, float g, float b, float a) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdClearColorImage);
        w_.writeU64(cmdBufferId);
        w_.writeU64(imageId);
        w_.writeU32(imageLayout);
        w_.writeF32(r); w_.writeF32(g); w_.writeF32(b); w_.writeF32(a);
        w_.endCommand(off);
    }
#endif // VK_VERSION_1_0

    void cmdBindPipeline(uint64_t cmdBufferId, uint32_t pipelineBindPoint, uint64_t pipelineId) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBindPipeline);
        vn_encode_vkCmdBindPipeline(&w_, cmdBufferId, pipelineBindPoint, pipelineId);
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

    void cmdSetCullMode(uint64_t cmdBufferId, uint32_t cullMode) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdSetCullMode);
        vn_encode_vkCmdSetCullMode(&w_, cmdBufferId, cullMode);
        w_.endCommand(off);
    }

    void cmdSetFrontFace(uint64_t cmdBufferId, uint32_t frontFace) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdSetFrontFace);
        vn_encode_vkCmdSetFrontFace(&w_, cmdBufferId, frontFace);
        w_.endCommand(off);
    }

    void cmdDraw(uint64_t cmdBufferId,
                 uint32_t vertexCount, uint32_t instanceCount,
                 uint32_t firstVertex, uint32_t firstInstance) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdDraw);
        vn_encode_vkCmdDraw(&w_, cmdBufferId, vertexCount, instanceCount,
                            firstVertex, firstInstance);
        w_.endCommand(off);
    }

    void cmdDrawIndexed(uint64_t cmdBufferId,
                        uint32_t indexCount, uint32_t instanceCount,
                        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdDrawIndexed);
        vn_encode_vkCmdDrawIndexed(&w_, cmdBufferId, indexCount, instanceCount,
                                   firstIndex, vertexOffset, firstInstance);
        w_.endCommand(off);
    }

    void cmdBindVertexBuffers(uint64_t cmdBufferId, uint32_t firstBinding,
                              uint32_t bindingCount, const uint64_t* bufferIds,
                              const uint64_t* offsets, const uint64_t* sizes,
                              const uint64_t* strides) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBindVertexBuffers);
        w_.writeU64(cmdBufferId);
        w_.writeU32(firstBinding); w_.writeU32(bindingCount);
        for (uint32_t i = 0; i < bindingCount; i++) {
            w_.writeU64(bufferIds[i]); w_.writeU64(offsets[i]);
            w_.writeU64(sizes ? sizes[i] : ~(uint64_t)0); // VK_WHOLE_SIZE when NULL
            w_.writeU64(strides ? strides[i] : 0);
        }
        w_.endCommand(off);
    }

    void cmdBindIndexBuffer(uint64_t cmdBufferId, uint64_t bufferId,
                            uint64_t offset, uint32_t indexType) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdBindIndexBuffer);
        vn_encode_vkCmdBindIndexBuffer(&w_, cmdBufferId, bufferId, offset, indexType);
        w_.endCommand(off);
    }

    void cmdCopyBuffer(uint64_t cmdBufferId, uint64_t srcBuf, uint64_t dstBuf,
                       uint32_t regionCount, const uint64_t* srcOffsets,
                       const uint64_t* dstOffsets, const uint64_t* sizes) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdCopyBuffer);
        w_.writeU64(cmdBufferId);
        w_.writeU64(srcBuf); w_.writeU64(dstBuf);
        w_.writeU32(regionCount);
        for (uint32_t i = 0; i < regionCount; i++) {
            w_.writeU64(srcOffsets[i]); w_.writeU64(dstOffsets[i]); w_.writeU64(sizes[i]);
        }
        w_.endCommand(off);
    }

    void cmdCopyBufferToImage(uint64_t cmdBufferId, uint64_t srcBuf, uint64_t dstImg,
                              uint32_t dstLayout, uint32_t regionCount,
                              const uint32_t* bufOffsets, const uint32_t* bufRowLengths,
                              const uint32_t* bufImgHeights,
                              const uint32_t* imgAspects, const uint32_t* imgMipLevels,
                              const uint32_t* imgBaseLayers, const uint32_t* imgLayerCounts,
                              const int32_t* imgOffX, const int32_t* imgOffY, const int32_t* imgOffZ,
                              const uint32_t* imgExtW, const uint32_t* imgExtH, const uint32_t* imgExtD) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdCopyBufferToImage);
        w_.writeU64(cmdBufferId);
        w_.writeU64(srcBuf); w_.writeU64(dstImg); w_.writeU32(dstLayout);
        w_.writeU32(regionCount);
        for (uint32_t i = 0; i < regionCount; i++) {
            w_.writeU32(bufOffsets[i]); w_.writeU32(bufRowLengths[i]); w_.writeU32(bufImgHeights[i]);
            w_.writeU32(imgAspects[i]); w_.writeU32(imgMipLevels[i]);
            w_.writeU32(imgBaseLayers[i]); w_.writeU32(imgLayerCounts[i]);
            w_.writeI32(imgOffX[i]); w_.writeI32(imgOffY[i]); w_.writeI32(imgOffZ[i]);
            w_.writeU32(imgExtW[i]); w_.writeU32(imgExtH[i]); w_.writeU32(imgExtD[i]);
        }
        w_.endCommand(off);
    }

    void cmdUpdateBuffer(uint64_t cmdBufferId, uint64_t bufferId,
                         uint64_t offset, uint64_t dataSize, const void* pData) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdUpdateBuffer);
        vn_encode_vkCmdUpdateBuffer(&w_, cmdBufferId, bufferId, offset, dataSize, pData);
        w_.endCommand(off);
    }

    void cmdPushConstants(uint64_t cmdBufferId, uint64_t layoutId,
                          uint32_t stageFlags, uint32_t offset, uint32_t size,
                          const void* pValues) {
        ENC_GUARD;
        auto off = w_.beginCommand(VN_CMD_vkCmdPushConstants);
        vn_encode_vkCmdPushConstants(&w_, cmdBufferId, layoutId,
                                     stageFlags, offset, size, pValues);
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

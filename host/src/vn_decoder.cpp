#include "vn_decoder.h"
#include "../../common/venus/vn_gen_decode.h"
#include "win_capture.h"
#include <fstream>
#include <algorithm>
#include <unordered_set>

void VnDecoder::init(VkPhysicalDevice physDevice, VkDevice device,
                     VkQueue graphicsQueue, uint32_t graphicsFamily,
                     VkSurfaceKHR surface) {
    physDevice_ = physDevice;
    device_ = device;
    graphicsQueue_ = graphicsQueue;
    graphicsFamily_ = graphicsFamily;
    surface_ = surface;

    // Create semaphore + fence for swapchain acquire synchronization
    VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &si, nullptr, &acquireSemaphore_);
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fi, nullptr, &acquireFence_);
    // Double-buffered readback fences: start SIGNALED so first reset is valid
    VkFenceCreateInfo fi3{}; fi3.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi3.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fi3, nullptr, &readbackFences_[0]);
    vkCreateFence(device_, &fi3, nullptr, &readbackFences_[1]);
}

VkFence VnDecoder::allocateFence() {
    while (!fencePool_.empty()) {
        VkFence f = fencePool_.back();
        fencePool_.pop_back();
        if (f == VK_NULL_HANDLE) continue;  // skip corrupted null handles
        vkResetFences(device_, 1, &f);
        return f;
    }
    VkFence f;
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fi, nullptr, &f);
    return f;
}

void VnDecoder::recycleFence(VkFence f) {
    fencePool_.push_back(f);
}

bool VnDecoder::execute(const uint8_t* data, size_t size) {
    pendingPresents_.clear();
    pendingBdaResults_.clear();
    copyStagingUsed_ = 0;  // reset staging arena for this batch
    VnStreamReader reader(data, size);
    while (reader.hasMore() && !error_) {
        // Record position before reading header
        const uint8_t* cmdStart = reader.currentPtr();

        uint32_t cmdType = reader.readU32();
        uint32_t cmdSize = reader.readU32();

        if (cmdType == VN_CMD_BRIDGE_EndOfStream)
            break;

        // Guard against corrupt stream: cmdSize must cover at least the 8-byte header
        if (cmdSize < 8) {
            fprintf(stderr, "[Decoder] ERROR: cmdSize=%u < 8 for cmd=%u, aborting batch\n",
                    cmdSize, cmdType);
            error_ = true;
            break;
        }

        dispatch(cmdType, reader, cmdSize);

        // Ensure reader is at the correct position for the next command.
        size_t cmdStartOff = cmdStart - data;
        size_t nextOff = cmdStartOff + cmdSize;
        size_t currentOff = reader.currentPtr() - data;
        if (currentOff != nextOff && nextOff <= size) {
            reader.setPos(nextOff);
        }
    }
    // All QueueSubmits in this batch have been decoded.
    // Flush any remaining collected CBs (those with no signal semaphore / fence
    // that were collected for batch ordering but never triggered by sync).
    flushPendingSubmits(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    // Now flush deferred Presents so the GPU work is done before presenting.
    RT_LOG(currentSeqId_, "T4", "decode done, flushing presents");
    flushPendingPresents();
    // Flush deferred destroys AFTER GPU is idle (presents do WaitIdle).
    flushPendingDestroys();
    RT_LOG(currentSeqId_, "T5", "execute done, total=%.2fms",
           (rtNowUs() - batchRecvUs_) / 1000.0);
    return !error_;
}

bool VnDecoder::executeOneFrame(VnStreamReader& reader) {
    while (reader.hasMore() && !error_) {
        size_t cmdStartPos = reader.pos();

        uint32_t cmdType = reader.readU32();
        uint32_t cmdSize = reader.readU32();

        if (cmdType == VN_CMD_BRIDGE_EndOfStream)
            return false;

        dispatch(cmdType, reader, cmdSize);

        // Ensure reader advances exactly cmdSize bytes (same protection as execute())
        size_t nextPos = cmdStartPos + cmdSize;
        if (reader.pos() != nextPos)
            reader.setPos(nextPos);

        if (cmdType == VN_CMD_BRIDGE_QueuePresent)
            return true; // frame boundary
    }
    return false;
}

void VnDecoder::dispatch(uint32_t cmdType, VnStreamReader& reader, uint32_t cmdSize) {
#ifdef VBOXGPU_VERBOSE
    fprintf(stderr, "[Decoder] cmd=%u size=%u\n", cmdType, cmdSize);
#endif
    switch (cmdType) {
    case VN_CMD_vkCreateRenderPass:       handleCreateRenderPass(reader); break;
    case VN_CMD_vkCreateShaderModule:     handleCreateShaderModule(reader); break;
    case VN_CMD_vkCreateDescriptorSetLayout: handleCreateDescriptorSetLayout(reader, cmdSize); break;
    case VN_CMD_vkCreatePipelineLayout:   handleCreatePipelineLayout(reader); break;
    case VN_CMD_vkCreateGraphicsPipelines:handleCreateGraphicsPipeline(reader); break;
    case VN_CMD_vkCreateFramebuffer:      handleCreateFramebuffer(reader); break;
    case VN_CMD_vkCreateCommandPool:      handleCreateCommandPool(reader); break;
    case VN_CMD_vkAllocateCommandBuffers: handleAllocateCommandBuffers(reader); break;
    case VN_CMD_vkBeginCommandBuffer:     handleBeginCommandBuffer(reader); break;
    case VN_CMD_vkEndCommandBuffer:       handleEndCommandBuffer(reader); break;
    case VN_CMD_vkCmdBeginRenderPass:     handleCmdBeginRenderPass(reader); break;
    case VN_CMD_vkCmdBeginRendering:      handleCmdBeginRendering(reader); break;
    case VN_CMD_vkCmdEndRendering:        handleCmdEndRendering(reader); break;
    case VN_CMD_vkCmdEndRenderPass:       handleCmdEndRenderPass(reader); break;
    case VN_CMD_vkCmdBindPipeline:        handleCmdBindPipeline(reader); break;
    case VN_CMD_vkCmdSetViewport:         handleCmdSetViewport(reader); break;
    case VN_CMD_vkCmdSetScissor:          handleCmdSetScissor(reader); break;
    case VN_CMD_vkCmdSetCullMode:         handleCmdSetCullMode(reader); break;
    case VN_CMD_vkCmdSetFrontFace:        handleCmdSetFrontFace(reader); break;
    case VN_CMD_vkCmdSetPrimitiveTopology:handleCmdSetPrimitiveTopology(reader); break;
    case VN_CMD_vkCmdSetDepthTestEnable:  handleCmdSetDepthTestEnable(reader); break;
    case VN_CMD_vkCmdSetDepthWriteEnable: handleCmdSetDepthWriteEnable(reader); break;
    case VN_CMD_vkCmdSetDepthCompareOp:   handleCmdSetDepthCompareOp(reader); break;
    case VN_CMD_vkCmdSetDepthBoundsTestEnable: handleCmdSetDepthBoundsTestEnable(reader); break;
    case VN_CMD_vkCmdSetDepthBiasEnable:  handleCmdSetDepthBiasEnable(reader); break;
    case VN_CMD_vkCmdBindVertexBuffers:   handleCmdBindVertexBuffers(reader); break;
    case VN_CMD_vkCmdBindIndexBuffer:     handleCmdBindIndexBuffer(reader); break;
    case VN_CMD_vkCmdDrawIndexed:         handleCmdDrawIndexed(reader); break;
    case VN_CMD_vkCmdCopyBuffer:          handleCmdCopyBuffer(reader); break;
    case VN_CMD_vkCmdCopyImage:           handleCmdCopyImage(reader); break;
    case VN_CMD_vkCmdBlitImage:           handleCmdBlitImage(reader); break;
    case VN_CMD_vkCmdCopyBufferToImage:   handleCmdCopyBufferToImage(reader); break;
    case VN_CMD_BRIDGE_CopyBufToImgInline: handleCopyBufToImgInline(reader); break;
    case VN_CMD_vkCmdUpdateBuffer:        handleCmdUpdateBuffer(reader); break;
    case VN_CMD_vkCmdDraw:                handleCmdDraw(reader); break;
    case VN_CMD_vkCmdPushConstants:      handleCmdPushConstants(reader); break;
    case VN_CMD_vkCreateImage:           handleCreateImage(reader); break;
    case VN_CMD_vkAllocateMemory:        handleAllocateMemory(reader); break;
    case VN_CMD_vkBindImageMemory:       handleBindImageMemory(reader); break;
    case VN_CMD_vkCreateImageView:       handleCreateImageView(reader); break;
    case VN_CMD_vkCreateSampler:         handleCreateSampler(reader); break;
    case VN_CMD_vkCreateDescriptorPool:  handleCreateDescriptorPool(reader); break;
    case VN_CMD_vkAllocateDescriptorSets:handleAllocateDescriptorSets(reader); break;
    case VN_CMD_vkUpdateDescriptorSets:  handleUpdateDescriptorSets(reader); break;
    case VN_CMD_vkCmdBindDescriptorSets: handleCmdBindDescriptorSets(reader); break;
    case VN_CMD_vkCmdPushDescriptorSet:  handleCmdPushDescriptorSet(reader); break;
    case VN_CMD_vkCmdPipelineBarrier2:   handleCmdPipelineBarrier(reader); break;
    case VN_CMD_vkCreateSemaphore:        handleCreateSemaphore(reader); break;
    case VN_CMD_vkCreateFence:            handleCreateFence(reader); break;
    case VN_CMD_vkQueueSubmit:            handleQueueSubmit(reader); break;
    case VN_CMD_vkWaitForFences:          handleWaitForFences(reader); break;
    case VN_CMD_vkResetFences:            handleResetFences(reader); break;
    case VN_CMD_vkCreateBuffer:           handleCreateBuffer(reader); break;
    case VN_CMD_vkBindBufferMemory:       handleBindBufferMemory(reader); break;
    case VN_CMD_vkCmdClearAttachments:    handleCmdClearAttachments(reader); break;
    case VN_CMD_vkCmdClearColorImage:     handleCmdClearColorImage(reader); break;
    // Destroy / Free
    case VN_CMD_vkDestroyBuffer:          handleDestroyBuffer(reader); break;
    case VN_CMD_vkDestroyImage:           handleDestroyImage(reader); break;
    case VN_CMD_vkDestroyImageView:       handleDestroyImageView(reader); break;
    case VN_CMD_vkDestroyShaderModule:    handleDestroyShaderModule(reader); break;
    case VN_CMD_vkDestroyPipeline:        handleDestroyPipeline(reader); break;
    case VN_CMD_vkDestroyPipelineLayout:  handleDestroyPipelineLayout(reader); break;
    case VN_CMD_vkDestroyRenderPass:      handleDestroyRenderPass(reader); break;
    case VN_CMD_vkDestroyFramebuffer:     handleDestroyFramebuffer(reader); break;
    case VN_CMD_vkDestroyCommandPool:     handleDestroyCommandPool(reader); break;
    case VN_CMD_vkDestroySampler:         handleDestroySampler(reader); break;
    case VN_CMD_vkDestroyDescriptorPool:  handleDestroyDescriptorPool(reader); break;
    case VN_CMD_vkDestroyDescriptorSetLayout: handleDestroyDescriptorSetLayout(reader); break;
    case VN_CMD_vkDestroyFence:           handleDestroyFence(reader); break;
    case VN_CMD_vkDestroySemaphore:       handleDestroySemaphore(reader); break;
    case VN_CMD_vkFreeMemory:             handleFreeMemory(reader); break;
    case VN_CMD_BRIDGE_WriteMemory:       handleWriteMemory(reader); break;
    case VN_CMD_BRIDGE_CreateSwapchain:   handleBridgeCreateSwapchain(reader); break;
    case VN_CMD_BRIDGE_AcquireNextImage:  handleBridgeAcquireNextImage(reader); break;
    case VN_CMD_BRIDGE_QueuePresent:      handleBridgeQueuePresent(reader); break;
    case VN_CMD_BRIDGE_GetBufferDeviceAddress: handleGetBufferDeviceAddress(reader); break;
    case VN_CMD_BRIDGE_RecordBDA:           handleBridgeRecordBDA(reader); break;
    case VN_CMD_BRIDGE_TimingSeq: {
        currentSeqId_ = reader.readU32();
        uint64_t guestTs = reader.readU64(); (void)guestTs;
        RT_LOG(currentSeqId_, "T3", "timing marker received");
        break;
    }
    default:
        fprintf(stderr, "[Decoder] Unknown command type %u, skipping\n", cmdType);
        // Don't set error — just skip (cmdSize-based framing will advance past it)
        break;
    }
}

// --- Resource creation handlers ---

void VnDecoder::handleCreateRenderPass(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t rpId = r.readU64();
    uint32_t attachCount = r.readU32();

    std::vector<VkAttachmentDescription> attachments(attachCount);
    for (uint32_t i = 0; i < attachCount; i++) {
        attachments[i] = {};
        attachments[i].format = static_cast<VkFormat>(r.readU32());
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = static_cast<VkAttachmentLoadOp>(r.readU32());
        attachments[i].storeOp = static_cast<VkAttachmentStoreOp>(r.readU32());
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = static_cast<VkImageLayout>(r.readU32());
        attachments[i].finalLayout = static_cast<VkImageLayout>(r.readU32());
    }

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = attachCount;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;

    VkRenderPass rp;
    if (vkCreateRenderPass(device_, &info, nullptr, &rp) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(renderPasses_, rpId, rp);
}

void VnDecoder::handleCreateShaderModule(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t moduleId = r.readU64();
    uint32_t codeSize = r.readU32();

    std::vector<uint32_t> code((codeSize + 3) / 4);
    r.readBytes(code.data(), codeSize);

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = codeSize;
    info.pCode = code.data();

    VkShaderModule mod;
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(shaderModules_, moduleId, mod);
}

void VnDecoder::handleCreateDescriptorSetLayout(VnStreamReader& r, uint32_t cmdSize) {
    size_t payloadStart = r.pos();
    uint64_t deviceId = r.readU64();
    uint64_t layoutId = r.readU64();
    uint32_t bindingCount = r.readU32();

    // Calculate expected payload size for old format (no immutable samplers)
    // payload: 8(deviceId) + 8(layoutId) + 4(bindingCount) + bindingCount * 16
    size_t oldPayloadSize = 8 + 8 + 4 + bindingCount * 16;
    size_t cmdPayload = cmdSize - 8; // subtract cmd header (cmdType + cmdSize)
    bool hasImmutSamplers = (cmdPayload > oldPayloadSize);

    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
    std::vector<std::vector<VkSampler>> immutableSamplers(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        bindings[i] = {};
        bindings[i].binding = r.readU32();
        bindings[i].descriptorType = static_cast<VkDescriptorType>(r.readU32());
        bindings[i].descriptorCount = r.readU32();
        bindings[i].stageFlags = r.readU32();
        // Decode immutable sampler IDs (only if new-format stream)
        if (hasImmutSamplers) {
            uint32_t immCount = r.readU32();
            if (immCount > 0 && immCount <= bindings[i].descriptorCount) {
                immutableSamplers[i].resize(immCount);
                for (uint32_t s = 0; s < immCount; s++) {
                    uint64_t samId = r.readU64();
                    immutableSamplers[i][s] = lookup(samplers_, samId);
                }
                bindings[i].pImmutableSamplers = immutableSamplers[i].data();
            }
        }
    }

    // DXVK uses regular descriptor sets (UpdateDescriptorSets + BindDescriptorSets),
    // NOT push descriptors. PUSH_DESCRIPTOR_BIT layouts cannot be used with
    // vkAllocateDescriptorSets per Vulkan spec — this was causing GPU to read zeros.
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = bindingCount;
    info.pBindings = bindings.data();
    info.flags = 0;

    VkDescriptorSetLayout layout;
    VkResult vr = vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout);
    if (vr != VK_SUCCESS) { error_ = true; return; }
    // Log bindings with immutable sampler info
    static int dslLog = 0;
    if (dslLog < 10) {
        fprintf(stderr, "[Decoder] CreateDSL: id=%llu bindings=%u", (unsigned long long)layoutId, bindingCount);
        for (uint32_t i = 0; i < bindingCount; i++) {
            fprintf(stderr, " [b=%u type=%u cnt=%u imm=%zu]",
                bindings[i].binding, bindings[i].descriptorType,
                bindings[i].descriptorCount, immutableSamplers[i].size());
        }
        fprintf(stderr, " result=%d\n", (int)vr);
        dslLog++;
    }
    store(descriptorSetLayouts_, layoutId, layout);
}

void VnDecoder::handleCreatePipelineLayout(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t layoutId = r.readU64();
    uint32_t setLayoutCount = r.readU32();

    std::vector<VkDescriptorSetLayout> setLayouts(setLayoutCount);
    for (uint32_t i = 0; i < setLayoutCount; i++) {
        uint64_t dsId = r.readU64();
        setLayouts[i] = lookup(descriptorSetLayouts_, dsId);
    }

    uint32_t pushRangeCount = r.readU32();
    std::vector<VkPushConstantRange> pushRanges(pushRangeCount);
    for (uint32_t i = 0; i < pushRangeCount; i++) {
        pushRanges[i].stageFlags = r.readU32();
        pushRanges[i].offset = r.readU32();
        pushRanges[i].size = r.readU32();
    }

    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = setLayoutCount;
    info.pSetLayouts = setLayouts.data();
    info.pushConstantRangeCount = pushRangeCount;
    info.pPushConstantRanges = pushRanges.data();

    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(device_, &info, nullptr, &layout) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(pipelineLayouts_, layoutId, layout);
}

// --- GPU Resource handlers ---

void VnDecoder::handleCreateImage(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t imageId = r.readU64();
    uint32_t imageType = r.readU32(), format = r.readU32();
    uint32_t w = r.readU32(), h = r.readU32(), d = r.readU32();
    uint32_t mipLevels = r.readU32(), arrayLayers = r.readU32(), samples = r.readU32();
    uint32_t tiling = r.readU32(), usage = r.readU32();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = static_cast<VkImageType>(imageType);
    ci.format = static_cast<VkFormat>(format);
    ci.extent = {w, h, d};
    ci.mipLevels = mipLevels; ci.arrayLayers = arrayLayers;
    ci.samples = static_cast<VkSampleCountFlagBits>(samples);
    ci.tiling = static_cast<VkImageTiling>(tiling);
    ci.usage = usage;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; // allow views with compatible formats
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image;
    VkResult vr = vkCreateImage(device_, &ci, nullptr, &image);
    fprintf(stderr, "[Decoder] CreateImage: id=%llu %ux%u fmt=%u usage=0x%x result=%d\n",
            (unsigned long long)imageId, w, h, format, usage, (int)vr);
    if (vr != VK_SUCCESS) return;
    store(images_, imageId, image);
    imageFormats_[imageId] = static_cast<VkFormat>(format);
    imageLayouts_[imageId] = VK_IMAGE_LAYOUT_UNDEFINED; // newly created image starts in UNDEFINED
}

uint32_t VnDecoder_mapMemoryType(VkPhysicalDevice physDev, uint32_t icdType) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physDev, &props);
    VkMemoryPropertyFlags wanted = (icdType == 0)
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return 0;
}

void VnDecoder::handleAllocateMemory(VnStreamReader& r) {
    VnDecode_vkAllocateMemory a;
    vn_decode_vkAllocateMemory(&r, &a);

    uint32_t hostType = VnDecoder_mapMemoryType(physDevice_, a.pAllocateInfo_memoryTypeIndex);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &allocFlags;
    ai.allocationSize = a.pAllocateInfo_allocationSize;
    ai.memoryTypeIndex = hostType;

    VkDeviceMemory mem;
    VkResult vr = vkAllocateMemory(device_, &ai, nullptr, &mem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] AllocMemory FAILED: id=%llu size=%llu\n",
                (unsigned long long)a.pMemory, (unsigned long long)a.pAllocateInfo_allocationSize);
        return;
    }
    store(deviceMemories_, a.pMemory, mem);
}

void VnDecoder::handleBindImageMemory(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t imageId = r.readU64();
    uint64_t memoryId = r.readU64();
    uint64_t offset = r.readU64();

    VkImage img = lookup(images_, imageId);
    VkDeviceMemory mem = lookup(deviceMemories_, memoryId);
    if (!img || !mem) return;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device_, img, &reqs);
    VkResult vr = vkBindImageMemory(device_, img, mem, offset);
    static int bindImageLog = 0;
    if (bindImageLog < 40 || vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] BindImageMemory: img=%llu mem=%llu off=%llu reqSize=%llu reqAlign=%llu result=%d\n",
                (unsigned long long)imageId, (unsigned long long)memoryId,
                (unsigned long long)offset, (unsigned long long)reqs.size,
                (unsigned long long)reqs.alignment, (int)vr);
        bindImageLog++;
    }
}

void VnDecoder::handleCreateImageView(VnStreamReader& r) {
    VnDecode_vkCreateImageView a;
    vn_decode_vkCreateImageView(&r, &a);

    VkImage img = lookup(images_, a.pCreateInfo_image);
    if (!img) {
        fprintf(stderr, "[Decoder] CreateImageView: SKIP id=%llu img=%llu NOT FOUND\n",
                (unsigned long long)a.pView, (unsigned long long)a.pCreateInfo_image);
        return;
    }

    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.flags = a.pCreateInfo_flags;
    ci.image = img;
    ci.viewType = static_cast<VkImageViewType>(a.pCreateInfo_viewType);
    ci.format = static_cast<VkFormat>(a.pCreateInfo_format);
    ci.components = {static_cast<VkComponentSwizzle>(a.pCreateInfo_components_r),
                     static_cast<VkComponentSwizzle>(a.pCreateInfo_components_g),
                     static_cast<VkComponentSwizzle>(a.pCreateInfo_components_b),
                     static_cast<VkComponentSwizzle>(a.pCreateInfo_components_a)};
    ci.subresourceRange = {a.pCreateInfo_subresourceRange_aspectMask,
                           a.pCreateInfo_subresourceRange_baseMipLevel,
                           a.pCreateInfo_subresourceRange_levelCount,
                           a.pCreateInfo_subresourceRange_baseArrayLayer,
                           a.pCreateInfo_subresourceRange_layerCount};

    VkImageView view;
    VkResult vr = vkCreateImageView(device_, &ci, nullptr, &view);
    fprintf(stderr, "[Decoder] CreateImageView: id=%llu img=%llu fmt=%u type=%u result=%d view=%p\n",
            (unsigned long long)a.pView, (unsigned long long)a.pCreateInfo_image,
            a.pCreateInfo_format, a.pCreateInfo_viewType, (int)vr, (void*)view);
    if (vr != VK_SUCCESS) return;
    store(imageViews_, a.pView, view);
}

void VnDecoder::handleCreateSampler(VnStreamReader& r) {
    VnDecode_vkCreateSampler a;
    vn_decode_vkCreateSampler(&r, &a);

    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.flags = a.pCreateInfo_flags;
    ci.magFilter = static_cast<VkFilter>(a.pCreateInfo_magFilter);
    ci.minFilter = static_cast<VkFilter>(a.pCreateInfo_minFilter);
    ci.mipmapMode = static_cast<VkSamplerMipmapMode>(a.pCreateInfo_mipmapMode);
    ci.addressModeU = static_cast<VkSamplerAddressMode>(a.pCreateInfo_addressModeU);
    ci.addressModeV = static_cast<VkSamplerAddressMode>(a.pCreateInfo_addressModeV);
    ci.addressModeW = static_cast<VkSamplerAddressMode>(a.pCreateInfo_addressModeW);
    ci.mipLodBias = a.pCreateInfo_mipLodBias;
    ci.anisotropyEnable = a.pCreateInfo_anisotropyEnable;
    ci.maxAnisotropy = a.pCreateInfo_maxAnisotropy;
    ci.compareEnable = a.pCreateInfo_compareEnable;
    ci.compareOp = static_cast<VkCompareOp>(a.pCreateInfo_compareOp);
    ci.minLod = a.pCreateInfo_minLod;
    ci.maxLod = a.pCreateInfo_maxLod;
    ci.borderColor = static_cast<VkBorderColor>(a.pCreateInfo_borderColor);
    ci.unnormalizedCoordinates = a.pCreateInfo_unnormalizedCoordinates;

    VkSampler sampler;
    VkResult vr = vkCreateSampler(device_, &ci, nullptr, &sampler);
    if (vr != VK_SUCCESS) return;
    store(samplers_, a.pSampler, sampler);
}

void VnDecoder::handleCreateDescriptorPool(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t poolId = r.readU64();
    uint32_t flags = r.readU32(), maxSets = r.readU32(), poolSizeCount = r.readU32();

    std::vector<VkDescriptorPoolSize> sizes(poolSizeCount);
    for (uint32_t i = 0; i < poolSizeCount; i++) {
        sizes[i].type = static_cast<VkDescriptorType>(r.readU32());
        sizes[i].descriptorCount = r.readU32();
    }

    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags = flags;
    ci.maxSets = maxSets;
    ci.poolSizeCount = poolSizeCount;
    ci.pPoolSizes = sizes.data();

    VkDescriptorPool pool;
    VkResult vr = vkCreateDescriptorPool(device_, &ci, nullptr, &pool);
    if (vr != VK_SUCCESS) return;
    store(descriptorPools_, poolId, pool);
}

void VnDecoder::handleAllocateDescriptorSets(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t poolId = r.readU64();
    uint32_t setCount = r.readU32();

    VkDescriptorPool pool = lookup(descriptorPools_, poolId);
    if (!pool) return;

    std::vector<VkDescriptorSetLayout> layouts(setCount);
    std::vector<uint64_t> setIds(setCount);
    for (uint32_t i = 0; i < setCount; i++) {
        uint64_t layoutId = r.readU64();
        setIds[i] = r.readU64();
        layouts[i] = lookup(descriptorSetLayouts_, layoutId);
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = setCount;
    ai.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(setCount);
    VkResult vr = vkAllocateDescriptorSets(device_, &ai, sets.data());
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] AllocDescSets FAILED: result=%d\n", (int)vr);
        return;
    }
    for (uint32_t i = 0; i < setCount; i++)
        store(descriptorSets_, setIds[i], sets[i]);
}

static bool isBufferDescriptorType(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

void VnDecoder::handleUpdateDescriptorSets(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint32_t writeCount = r.readU32();

    std::vector<VkWriteDescriptorSet> writes(writeCount);
    std::vector<std::vector<VkDescriptorImageInfo>> allImageInfos(writeCount);
    std::vector<std::vector<VkDescriptorBufferInfo>> allBufferInfos(writeCount);

    for (uint32_t i = 0; i < writeCount; i++) {
        uint64_t dstSetId = r.readU64();
        uint32_t dstBinding = r.readU32(), dstArrayElem = r.readU32();
        uint32_t descCount = r.readU32(), descType = r.readU32();

        allImageInfos[i].resize(descCount);
        allBufferInfos[i].resize(descCount);
        for (uint32_t j = 0; j < descCount; j++) {
            uint64_t samId = r.readU64(), ivId = r.readU64();
            uint32_t imgLayout = r.readU32();
            uint64_t bufId = r.readU64(), bufOff = r.readU64(), bufRange = r.readU64();
            allImageInfos[i][j].sampler = lookup(samplers_, samId);
            allImageInfos[i][j].imageView = lookup(imageViews_, ivId);
            allImageInfos[i][j].imageLayout = static_cast<VkImageLayout>(imgLayout);
            allBufferInfos[i][j].buffer = lookup(buffers_, bufId);
            allBufferInfos[i][j].offset = bufOff;
            allBufferInfos[i][j].range = bufRange;
            // Debug: log image descriptor bindings (type 0=SAMPLER, 2=SAMPLED_IMAGE)
            static int descLog = 0;
            if (descLog < 50000 && (descType == 0 || descType == 1 || descType == 2)) {
                VkDescriptorSet hostSet = lookup(descriptorSets_, dstSetId);
                fprintf(stderr, "[Decoder] DescBind: dstSet=%llu(%p) bind=%u arr=%u type=%u iv=%llu(%p) layout=%u sam=%llu(%p)\n",
                        (unsigned long long)dstSetId, (void*)hostSet,
                        dstBinding, dstArrayElem + j,
                        descType, (unsigned long long)ivId, (void*)allImageInfos[i][j].imageView,
                        imgLayout,
                        (unsigned long long)samId, (void*)allImageInfos[i][j].sampler);
                descLog++;
            }
        }

        VkDescriptorType dt = static_cast<VkDescriptorType>(descType);
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = lookup(descriptorSets_, dstSetId);
        writes[i].dstBinding = dstBinding;
        writes[i].dstArrayElement = dstArrayElem;
        writes[i].descriptorCount = descCount;
        writes[i].descriptorType = dt;
        if (isBufferDescriptorType(dt)) {
            writes[i].pBufferInfo = allBufferInfos[i].data();
        } else {
            writes[i].pImageInfo = allImageInfos[i].data();
        }
    }

    vkUpdateDescriptorSets(device_, writeCount, writes.data(), 0, nullptr);
}

void VnDecoder::handleCmdBindDescriptorSets(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t bindPoint = r.readU32();
    uint64_t layoutId = r.readU64();
    uint32_t firstSet = r.readU32(), setCount = r.readU32();

    std::vector<VkDescriptorSet> sets(setCount);
    for (uint32_t i = 0; i < setCount; i++) {
        uint64_t setId = r.readU64();
        sets[i] = lookup(descriptorSets_, setId);
    }
    uint32_t dynOffCount = r.readU32();
    std::vector<uint32_t> dynOffs(dynOffCount);
    for (uint32_t i = 0; i < dynOffCount; i++) dynOffs[i] = r.readU32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipelineLayout layout = lookup(pipelineLayouts_, layoutId);
    if (!cb || !layout) return;

    // Log descriptor set bindings
    static int dsLog = 0;
    if (dsLog < 50000) {
        fprintf(stderr, "[Decoder] BindDescSets: cb=%llu bp=%u first=%u count=%u",
                (unsigned long long)cbId, bindPoint, firstSet, setCount);
        for (uint32_t i = 0; i < setCount; i++)
            fprintf(stderr, " set[%u]=%p", firstSet + i, (void*)sets[i]);
        fprintf(stderr, "\n");
        dsLog++;
    }

    vkCmdBindDescriptorSets(cb, static_cast<VkPipelineBindPoint>(bindPoint), layout,
        firstSet, setCount, sets.data(), dynOffCount, dynOffs.data());
}

void VnDecoder::handleCmdPushDescriptorSet(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t bindPoint = r.readU32();
    uint64_t layoutId = r.readU64();
    uint32_t set = r.readU32();
    uint32_t writeCount = r.readU32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipelineLayout layout = lookup(pipelineLayouts_, layoutId);

    std::vector<VkWriteDescriptorSet> writes(writeCount);
    std::vector<std::vector<VkDescriptorImageInfo>> allImageInfos(writeCount);
    std::vector<std::vector<VkDescriptorBufferInfo>> allBufferInfos(writeCount);

    for (uint32_t i = 0; i < writeCount; i++) {
        uint32_t binding = r.readU32();
        uint32_t descCount = r.readU32();
        uint32_t descType = r.readU32();

        allImageInfos[i].resize(descCount);
        allBufferInfos[i].resize(descCount);
        for (uint32_t j = 0; j < descCount; j++) {
            uint64_t samId = r.readU64();
            uint64_t ivId = r.readU64();
            uint32_t imgLayout = r.readU32();
            uint64_t bufId = r.readU64();
            uint64_t bufOff = r.readU64();
            uint64_t bufRange = r.readU64();
            allImageInfos[i][j].sampler = lookup(samplers_, samId);
            allImageInfos[i][j].imageView = lookup(imageViews_, ivId);
            allImageInfos[i][j].imageLayout = static_cast<VkImageLayout>(imgLayout);
            allBufferInfos[i][j].buffer = lookup(buffers_, bufId);
            allBufferInfos[i][j].offset = bufOff;
            allBufferInfos[i][j].range = bufRange;
        }

        VkDescriptorType dt = static_cast<VkDescriptorType>(descType);
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = binding;
        writes[i].descriptorCount = descCount;
        writes[i].descriptorType = dt;
        if (isBufferDescriptorType(dt)) {
            writes[i].pBufferInfo = allBufferInfos[i].data();
        } else {
            writes[i].pImageInfo = allImageInfos[i].data();
        }
    }

    static int pushLog = 0;
    if (pushLog < 5)
        fprintf(stderr, "[Decoder] PushDescSet: cb=%p layout=%p set=%u writes=%u\n",
                (void*)cb, (void*)layout, set, writeCount);
    pushLog++;

    if (!cb || !layout) return;

    // Use VK_KHR_push_descriptor
    static PFN_vkCmdPushDescriptorSetKHR pfnPush = nullptr;
    if (!pfnPush) {
        pfnPush = (PFN_vkCmdPushDescriptorSetKHR)
            vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR");
    }
    if (pfnPush) {
        pfnPush(cb, static_cast<VkPipelineBindPoint>(bindPoint), layout,
                set, writeCount, writes.data());
    }
}

void VnDecoder::handleCreateBuffer(VnStreamReader& r) {
    VnDecode_vkCreateBuffer a;
    vn_decode_vkCreateBuffer(&r, &a);

    uint32_t usage = a.pCreateInfo_usage;
    VkDeviceSize size = a.pCreateInfo_size;
    uint64_t guestId = a.pBuffer;

    bufferUsageFlags_[guestId] = usage;

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.flags = a.pCreateInfo_flags;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = static_cast<VkSharingMode>(a.pCreateInfo_sharingMode);
    ci.queueFamilyIndexCount = a.pCreateInfo_queueFamilyIndexCount;
    ci.pQueueFamilyIndices = a.pCreateInfo_pQueueFamilyIndices.data();

    VkBuffer buffer;
    VkResult vr = vkCreateBuffer(device_, &ci, nullptr, &buffer);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] CreateBuffer FAILED: id=%llu size=%llu usage=0x%x\n",
                (unsigned long long)guestId, (unsigned long long)size, usage);
        return;
    }
    store(buffers_, guestId, buffer);
}

void VnDecoder::handleBindBufferMemory(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t bufferId = r.readU64();
    uint64_t memoryId = r.readU64();
    uint64_t offset = r.readU64();
    (void)deviceId;

    VkBuffer buf = lookup(buffers_, bufferId);
    VkDeviceMemory mem = lookup(deviceMemories_, memoryId);
    if (!buf || !mem) return;

    VkResult vr = vkBindBufferMemory(device_, buf, mem, offset);
    bufferBindings_[bufferId] = {memoryId, offset};

    // Auto-BDA: if buffer has SHADER_DEVICE_ADDRESS_BIT, proactively query and return
    if (vr == VK_SUCCESS) {
        auto uit = bufferUsageFlags_.find(bufferId);
        if (uit != bufferUsageFlags_.end() &&
            (uit->second & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
            VkBufferDeviceAddressInfo bdaInfo{};
            bdaInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            bdaInfo.buffer = buf;
            VkDeviceAddress addr = vkGetBufferDeviceAddress(device_, &bdaInfo);
            pendingBdaResults_.push_back({bufferId, addr});
            // Track for replay BDA patching: buf → replay GPU address
            replayBdaByBufferId_[bufferId] = (uint64_t)addr;
            fprintf(stderr, "[AutoBDA] buf=%llu mem=%llu off=%llu -> GPU addr=0x%llx\n",
                    (unsigned long long)bufferId, (unsigned long long)memoryId,
                    (unsigned long long)offset, (unsigned long long)addr);
        }
    }
}

void VnDecoder::handleCmdClearAttachments(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t attachCount = r.readU32();
    std::vector<VkClearAttachment> attachments(attachCount);
    for (uint32_t i = 0; i < attachCount; i++) {
        attachments[i].aspectMask = r.readU32();
        attachments[i].colorAttachment = r.readU32();
        attachments[i].clearValue.color.float32[0] = r.readF32();
        attachments[i].clearValue.color.float32[1] = r.readF32();
        attachments[i].clearValue.color.float32[2] = r.readF32();
        attachments[i].clearValue.color.float32[3] = r.readF32();
    }
    uint32_t rectCount = r.readU32();
    std::vector<VkClearRect> rects(rectCount);
    for (uint32_t i = 0; i < rectCount; i++) {
        rects[i].rect.offset.x = (int32_t)r.readU32();
        rects[i].rect.offset.y = (int32_t)r.readU32();
        rects[i].rect.extent.width = r.readU32();
        rects[i].rect.extent.height = r.readU32();
        rects[i].baseArrayLayer = r.readU32();
        rects[i].layerCount = r.readU32();
    }
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;
    vkCmdClearAttachments(cb, attachCount, attachments.data(), rectCount, rects.data());
    fprintf(stderr, "[Decoder] ClearAttachments: cb=%p attachments=%u rects=%u\n",
            (void*)cb, attachCount, rectCount);
}

void VnDecoder::handleCmdClearColorImage(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t imgId = r.readU64();
    uint32_t layout = r.readU32();
    float cr = r.readF32(), cg = r.readF32(), cb_ = r.readF32(), ca = r.readF32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkImage img = lookup(images_, imgId);
    if (!cb || !img) return;

    VkClearColorValue clearColor = {{cr, cg, cb_, ca}};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cb, img, static_cast<VkImageLayout>(layout), &clearColor, 1, &range);
    fprintf(stderr, "[Decoder] ClearColorImage: img=%p color=(%.2f,%.2f,%.2f,%.2f)\n",
            (void*)img, cr, cg, cb_, ca);
}

// ── Destroy / Free handlers ──────────────────────────────────────────────────
// All follow the same pattern: read [device:u64][object:u64], destroy, erase from map.

#define IMPL_DESTROY(HandlerName, VkType, vkDestroyFn, mapName) \
void VnDecoder::HandlerName(VnStreamReader& r) { \
    uint64_t deviceId = r.readU64(); \
    uint64_t objId = r.readU64(); \
    (void)deviceId; \
    VkType obj = lookup(mapName, objId); \
    if (obj) { \
        mapName.erase(objId); \
        pendingDestroys_.push_back([this, obj]() { vkDestroyFn(device_, obj, nullptr); }); \
    } \
}

IMPL_DESTROY(handleDestroyBuffer,              VkBuffer,              vkDestroyBuffer,              buffers_)
IMPL_DESTROY(handleDestroyImage,               VkImage,               vkDestroyImage,               images_)
IMPL_DESTROY(handleDestroyImageView,           VkImageView,           vkDestroyImageView,           imageViews_)
IMPL_DESTROY(handleDestroyShaderModule,        VkShaderModule,        vkDestroyShaderModule,        shaderModules_)
IMPL_DESTROY(handleDestroyPipeline,            VkPipeline,            vkDestroyPipeline,            pipelines_)
IMPL_DESTROY(handleDestroyPipelineLayout,      VkPipelineLayout,      vkDestroyPipelineLayout,      pipelineLayouts_)
IMPL_DESTROY(handleDestroyRenderPass,          VkRenderPass,          vkDestroyRenderPass,          renderPasses_)
IMPL_DESTROY(handleDestroyFramebuffer,         VkFramebuffer,         vkDestroyFramebuffer,         framebuffers_)
IMPL_DESTROY(handleDestroyCommandPool,         VkCommandPool,         vkDestroyCommandPool,         commandPools_)
IMPL_DESTROY(handleDestroySampler,             VkSampler,             vkDestroySampler,             samplers_)
IMPL_DESTROY(handleDestroyDescriptorPool,      VkDescriptorPool,      vkDestroyDescriptorPool,      descriptorPools_)
IMPL_DESTROY(handleDestroyDescriptorSetLayout, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout, descriptorSetLayouts_)
IMPL_DESTROY(handleDestroyFence,               VkFence,               vkDestroyFence,               fences_)
IMPL_DESTROY(handleDestroySemaphore,           VkSemaphore,           vkDestroySemaphore,           semaphores_)

void VnDecoder::handleFreeMemory(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t memId = r.readU64();
    (void)deviceId;
    VkDeviceMemory mem = lookup(deviceMemories_, memId);
    if (mem) {
        deviceMemories_.erase(memId);
        pendingDestroys_.push_back([this, mem]() { vkFreeMemory(device_, mem, nullptr); });
    }
}

#undef IMPL_DESTROY

void VnDecoder::handleWriteMemory(VnStreamReader& r) {
    uint64_t memId = r.readU64();
    uint64_t offset = r.readU64();
    uint32_t size = r.readU32();

    // Wait for the previous batch's GPU work before overwriting GPU-visible memory.
    // Only done once per batch (on the first WriteMemory). Subsequent WriteMemory
    // calls in the same batch are already safe since the GPU hasn't started yet.
    // In most cases the fence is already signaled (TCP round-trip gives GPU time),
    // so this wait is effectively free and only blocks when GPU is truly behind.
    if (lastBatchWaitPending_ && lastBatchFence_ != VK_NULL_HANDLE) {
        VkResult wr = vkWaitForFences(device_, 1, &lastBatchFence_, VK_TRUE, 5000000000ULL);
        if (wr != VK_SUCCESS)
            fprintf(stderr, "[Decoder] batch fence wait failed: %d\n", (int)wr);
        lastBatchWaitPending_ = false;
    }

    VkDeviceMemory mem = lookup(deviceMemories_, memId);
    if (!mem) {
        r.skip(size);
        return;
    }
    void* mapped = nullptr;
    VkResult vr = vkMapMemory(device_, mem, offset, size, 0, &mapped);
    if (vr != VK_SUCCESS) {
        r.skip(size);
        fprintf(stderr, "[Decoder] WriteMemory: MapMemory failed for mem=%llu result=%d\n",
                (unsigned long long)memId, (int)vr);
        return;
    }
    r.readBytes(mapped, size);
    // For large writes (likely textures), log first 4 pixels to detect all-zero data
    if (size >= 1024*1024) {
        auto* b = static_cast<uint8_t*>(mapped);
        bool allZero = true;
        for (uint32_t k = 0; k < std::min(size, 64u); k++) if (b[k]) { allZero = false; break; }
        fprintf(stderr, "[Decoder] WriteMemory: mem=%llu off=%llu size=%u px0=(%u,%u,%u,%u) %s\n",
                (unsigned long long)memId, (unsigned long long)offset, size,
                b[0], b[1], b[2], b[3], allZero ? "ALL_ZERO" : "HAS_DATA");
    } else {
        fprintf(stderr, "[Decoder] WriteMemory: mem=%llu off=%llu size=%u\n",
                (unsigned long long)memId, (unsigned long long)offset, size);
    }
    // BDA patching: replace live GPU addresses (from live recording session) with
    // current replay GPU addresses. Only active once RecordBDA commands populate
    // liveBdaToReplayBda_. Scans every 8-byte-aligned position in the write payload.
    if (!liveBdaToReplayBda_.empty()) {
        auto* b = static_cast<uint8_t*>(mapped);
        uint32_t patchCount = 0;
        for (uint32_t k = 0; k + 8 <= size; k += 8) {
            uint64_t v;
            memcpy(&v, b + k, 8);
            auto it = liveBdaToReplayBda_.find(v);
            if (it != liveBdaToReplayBda_.end()) {
                uint64_t replayAddr = it->second;
                memcpy(b + k, &replayAddr, 8);
                patchCount++;
            }
        }
        if (patchCount > 0) {
            fprintf(stderr, "[BDA-PATCH] mem=%llu off=%llu size=%u: patched %u addr(s)\n",
                    (unsigned long long)memId, (unsigned long long)offset, size, patchCount);
        }
    }
    vkUnmapMemory(device_, mem);
}

// Strip pipeline stage bits that require device features the Host GPU may not
// have enabled.  This is a defensive filter — the ICD should not report these
// features, but old command recordings or edge cases might still have them.
static uint32_t sanitizePipelineStageFlags(uint32_t flags) {
    // Remove geometry/tessellation stage bits — the ICD no longer advertises
    // these features, but guard against stale streams or future mismatches.
    flags &= ~(uint32_t)VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    flags &= ~(uint32_t)VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
    flags &= ~(uint32_t)VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    // If stripping bits leaves the mask empty, use a safe fallback
    if (flags == 0)
        flags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return flags;
}

void VnDecoder::handleCmdPipelineBarrier(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t srcStage = sanitizePipelineStageFlags(r.readU32());
    uint32_t dstStage = sanitizePipelineStageFlags(r.readU32());
    uint32_t imageBarrierCount = r.readU32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);

    std::vector<VkImageMemoryBarrier> barriers(imageBarrierCount);
    for (uint32_t i = 0; i < imageBarrierCount; i++) {
        uint64_t imgId = r.readU64();
        uint32_t oldLayout = r.readU32(), newLayout = r.readU32();
        uint32_t srcAccess = r.readU32(), dstAccess = r.readU32();

        barriers[i] = {};
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = srcAccess;
        barriers[i].dstAccessMask = dstAccess;
        // Trust DXVK's layout tracking — it correctly knows the GPU execution order.
        // Host-side stream-order tracking was unreliable: CBs may appear in stream
        // in a different order than they execute on GPU (e.g., init barrier CBs
        // recorded after main CB in stream, but submitted first on GPU).
        imageLayouts_[imgId] = static_cast<VkImageLayout>(newLayout);
        // Log all layout transitions (with CB info)
        fprintf(stderr, "[Decoder] Barrier: cb=%llu img=%llu old=%u new=%u\n",
                (unsigned long long)cbId, (unsigned long long)imgId, oldLayout, newLayout);
        barriers[i].oldLayout = static_cast<VkImageLayout>(oldLayout);  // trust DXVK
        barriers[i].newLayout = static_cast<VkImageLayout>(newLayout);
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = lookup(images_, imgId);
        // Determine correct aspect mask from image format
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        auto fmtIt = imageFormats_.find(imgId);
        if (fmtIt != imageFormats_.end()) {
            VkFormat fmt = fmtIt->second;
            if (fmt >= VK_FORMAT_D16_UNORM && fmt <= VK_FORMAT_D32_SFLOAT_S8_UINT) {
                aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                    fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
                    aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        if (!barriers[i].image && (imgId & 0xFFF00000ull) == 0xFFF00000ull) {
            HostSwapchain* sc = getFirstSwapchain();
            uint32_t swapchainIndex = static_cast<uint32_t>(imgId - 0xFFF00000ull);
            if (sc && swapchainIndex < sc->images.size()) {
                barriers[i].image = sc->images[swapchainIndex];
                aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }
        barriers[i].subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

        // If image not found in our map, it might be a swapchain image — skip
        if (!barriers[i].image) {
            // Swapchain image IDs are 0xFFF00000+i sentinel values — don't barrier them here
            continue;
        }
    }

    if (!cb) return;

    // Filter out barriers with null images
    std::vector<VkImageMemoryBarrier> validBarriers;
    for (auto& b : barriers)
        if (b.image) validBarriers.push_back(b);

    if (!validBarriers.empty()) {
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0,
            0, nullptr, 0, nullptr,
            (uint32_t)validBarriers.size(), validBarriers.data());
    }
}

void VnDecoder::handleCreateGraphicsPipeline(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t pipelineId = r.readU64();
    uint64_t renderPassId = r.readU64();
    uint64_t layoutId = r.readU64();
    uint64_t vertModuleId = r.readU64();
    uint64_t fragModuleId = r.readU64();
    uint32_t vpWidth = r.readU32();
    uint32_t vpHeight = r.readU32();
    uint32_t colorFmt = r.readU32(); // 0 = use renderPass, nonzero = dynamic rendering format

    VkShaderModule vertMod = lookup(shaderModules_, vertModuleId);
    VkShaderModule fragMod = lookup(shaderModules_, fragModuleId);
    bool dynamicRendering = (renderPassId == 0 && colorFmt != 0);

    fprintf(stderr, "[Decoder] CreatePipeline: id=%u rp=%u vert->%p frag->%p %ux%u dynRender=%d fmt=%u\n",
            (unsigned)pipelineId, (unsigned)renderPassId,
            (void*)vertMod, (void*)fragMod, vpWidth, vpHeight,
            (int)dynamicRendering, colorFmt);

    fprintf(stderr, "[Decoder] CreatePipeline DETAIL: dynRendering=%d colorFmt=%u vpW=%u vpH=%u vertMod=%p fragMod=%p\n",
            (int)dynamicRendering, colorFmt, vpWidth, vpHeight, (void*)vertMod, (void*)fragMod);
    fflush(stderr);

    if (!vertMod || !fragMod) {
        fprintf(stderr, "[Decoder] CreatePipeline: missing shader, skipping\n");
        store(pipelines_, pipelineId, (VkPipeline)VK_NULL_HANDLE);
        return;
    }
    if (!dynamicRendering && !lookup(renderPasses_, renderPassId)) {
        fprintf(stderr, "[Decoder] CreatePipeline: missing renderpass, skipping\n");
        store(pipelines_, pipelineId, (VkPipeline)VK_NULL_HANDLE);
        return;
    }

    // Shader stages
    uint32_t stageCount = fragMod ? 2 : 1;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    if (fragMod) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";
    }

    // Use swapchain extent for viewport/scissor instead of DXVK's garbage values.
    // DXVK uses dynamic viewport state; the values in pipeline create info are uninitialized.
    uint32_t realW = vpWidth, realH = vpHeight;
    HostSwapchain* scVP = getFirstSwapchain();
    if (scVP && (vpWidth > scVP->extent.width || vpHeight > scVP->extent.height || vpWidth == 0 || vpHeight == 0)) {
        realW = scVP->extent.width;
        realH = scVP->extent.height;
    }

    // Read vertex input state (appended after colorFmt in the command)
    std::vector<VkVertexInputBindingDescription> vtxBindings;
    std::vector<VkVertexInputAttributeDescription> vtxAttrs;
    if (r.remaining() >= 4) {  // backward compat: old streams have no vertex input
        uint32_t bindingCount = r.readU32();
        vtxBindings.resize(bindingCount);
        for (uint32_t i = 0; i < bindingCount; i++) {
            vtxBindings[i].binding = r.readU32();
            vtxBindings[i].stride = r.readU32();
            vtxBindings[i].inputRate = static_cast<VkVertexInputRate>(r.readU32());
        }
        uint32_t attrCount = r.readU32();
        vtxAttrs.resize(attrCount);
        for (uint32_t i = 0; i < attrCount; i++) {
            vtxAttrs[i].location = r.readU32();
            vtxAttrs[i].binding = r.readU32();
            vtxAttrs[i].format = static_cast<VkFormat>(r.readU32());
            vtxAttrs[i].offset = r.readU32();
        }
    }

    // Depth attachment format (appended field — backward compat)
    uint32_t depthFmt = 0;
    if (r.remaining() >= 4) {
        depthFmt = r.readU32();
    }

    // Blend attachment state (appended field — backward compat)
    bool hasBlendState = false;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (r.remaining() >= 4) {
        uint32_t hasBlend = r.readU32();
        if (hasBlend && r.remaining() >= 32) {
            hasBlendState = true;
            blendAtt.blendEnable = r.readU32();
            blendAtt.srcColorBlendFactor = static_cast<VkBlendFactor>(r.readU32());
            blendAtt.dstColorBlendFactor = static_cast<VkBlendFactor>(r.readU32());
            blendAtt.colorBlendOp = static_cast<VkBlendOp>(r.readU32());
            blendAtt.srcAlphaBlendFactor = static_cast<VkBlendFactor>(r.readU32());
            blendAtt.dstAlphaBlendFactor = static_cast<VkBlendFactor>(r.readU32());
            blendAtt.alphaBlendOp = static_cast<VkBlendOp>(r.readU32());
            blendAtt.colorWriteMask = r.readU32();
        }
    }

    uint32_t topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t primitiveRestartEnable = VK_FALSE;
    uint32_t depthClampEnable = VK_FALSE;
    uint32_t rasterizerDiscardEnable = VK_FALSE;
    uint32_t polygonMode = VK_POLYGON_MODE_FILL;
    uint32_t cullMode = VK_CULL_MODE_NONE;
    uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    uint32_t depthBiasEnable = VK_FALSE;
    uint32_t depthTestEnable = VK_FALSE;
    uint32_t depthWriteEnable = VK_FALSE;
    uint32_t depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    uint32_t depthBoundsTestEnable = VK_FALSE;
    uint32_t stencilTestEnable = VK_FALSE;
    std::vector<VkDynamicState> dynStates;
    if (r.remaining() >= 4) {
        uint32_t hasPipelineState = r.readU32();
        if (hasPipelineState && r.remaining() >= 56) {
            topology = r.readU32();
            primitiveRestartEnable = r.readU32();
            depthClampEnable = r.readU32();
            rasterizerDiscardEnable = r.readU32();
            polygonMode = r.readU32();
            cullMode = r.readU32();
            frontFace = r.readU32();
            depthBiasEnable = r.readU32();
            depthTestEnable = r.readU32();
            depthWriteEnable = r.readU32();
            depthCompareOp = r.readU32();
            depthBoundsTestEnable = r.readU32();
            stencilTestEnable = r.readU32();
            uint32_t dynamicStateCount = r.readU32();
            for (uint32_t i = 0; i < dynamicStateCount && r.remaining() >= 4; i++)
                dynStates.push_back(static_cast<VkDynamicState>(r.readU32()));
        }
    }
    dynStates.erase(std::remove_if(dynStates.begin(), dynStates.end(),
        [](VkDynamicState state) {
            return state == VK_DYNAMIC_STATE_DEPTH_BIAS ||
                   state == VK_DYNAMIC_STATE_DEPTH_BOUNDS ||
                   state == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK ||
                   state == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK ||
                   state == VK_DYNAMIC_STATE_STENCIL_REFERENCE ||
                   state == VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE ||
                   state == VK_DYNAMIC_STATE_STENCIL_OP;
        }), dynStates.end());

    // Ensure VERTEX_INPUT_BINDING_STRIDE is always dynamic when using dynamic rendering.
    // DXVK uses vkCmdBindVertexBuffers2 with explicit strides but may not declare this
    // dynamic state — the pipeline's static stride=0 would cause all vertices to overlap.
    if (dynamicRendering) {
        auto hasState = [&](VkDynamicState s) {
            return std::find(dynStates.begin(), dynStates.end(), s) != dynStates.end();
        };
        if (!hasState(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE))
            dynStates.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
        if (!hasState(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE))
            dynStates.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE);
    }

    fprintf(stderr, "[Decoder] CreatePipeline vtxInput: %zu bindings, %zu attrs depthFmt=%u blend=%d (en=%u src=%u dst=%u op=%u mask=0x%x) topo=%u cull=%u front=%u dt=%u dw=%u ste=%u rastDiscard=%u dyn=%zu dynVals=[",
            vtxBindings.size(), vtxAttrs.size(), depthFmt, (int)hasBlendState,
            blendAtt.blendEnable, blendAtt.srcColorBlendFactor, blendAtt.dstColorBlendFactor,
            blendAtt.colorBlendOp, blendAtt.colorWriteMask, topology, cullMode, frontFace,
            depthTestEnable, depthWriteEnable, stencilTestEnable, rasterizerDiscardEnable, dynStates.size());
    for (size_t i = 0; i < dynStates.size(); i++)
        fprintf(stderr, "%s%u", i?",":"", (unsigned)dynStates[i]);
    fprintf(stderr, "]\n");
    for (size_t i = 0; i < vtxBindings.size(); i++)
        fprintf(stderr, "  binding[%zu]: slot=%u stride=%u rate=%u\n", i,
                vtxBindings[i].binding, vtxBindings[i].stride, vtxBindings[i].inputRate);
    for (size_t i = 0; i < vtxAttrs.size(); i++)
        fprintf(stderr, "  attr[%zu]: loc=%u bind=%u fmt=%u off=%u\n", i,
                vtxAttrs[i].location, vtxAttrs[i].binding, vtxAttrs[i].format, vtxAttrs[i].offset);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = (uint32_t)vtxBindings.size();
    vertexInput.pVertexBindingDescriptions = vtxBindings.data();
    vertexInput.vertexAttributeDescriptionCount = (uint32_t)vtxAttrs.size();
    vertexInput.pVertexAttributeDescriptions = vtxAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = static_cast<VkPrimitiveTopology>(topology);
    inputAsm.primitiveRestartEnable = primitiveRestartEnable;

    // Use dynamic viewport/scissor — DXVK sends garbage viewport values
    // because it uses VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT.
    // We'll set the actual viewport in BeginRendering from the swapchain extent.
    VkViewport viewport{0, 0, (float)realW, (float)realH, 0, 1};
    VkRect2D scissor{{0,0}, {realW, realH}};

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    // Dynamic state: only for dynamic rendering (DXVK path).
    // Legacy render pass (guest_sim) uses static viewport/scissor.
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = (uint32_t)dynStates.size();
    dynState.pDynamicStates = dynStates.data();

    if (dynamicRendering) {
        // Dynamic viewport — set at BeginRendering time
    } else {
        // Static viewport for legacy render pass
        vpState.pViewports = &viewport;
        vpState.pScissors = &scissor;
        dynState.dynamicStateCount = 0; // no dynamic state
    }

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE; // don't cull — DXVK manages culling
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthClampEnable = depthClampEnable;
    raster.rasterizerDiscardEnable = rasterizerDiscardEnable;
    raster.polygonMode = static_cast<VkPolygonMode>(polygonMode);
    raster.cullMode = static_cast<VkCullModeFlags>(cullMode);
    raster.frontFace = static_cast<VkFrontFace>(frontFace);
    raster.depthBiasEnable = depthBiasEnable;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

    // Depth/stencil state (all values set via dynamic state)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = depthTestEnable;
    depthStencil.depthWriteEnable = depthWriteEnable;
    depthStencil.depthCompareOp = static_cast<VkCompareOp>(depthCompareOp);
    depthStencil.depthBoundsTestEnable = depthBoundsTestEnable;
    depthStencil.stencilTestEnable = stencilTestEnable;

    // Dynamic rendering info (Vulkan 1.3)
    VkPipelineRenderingCreateInfo renderingInfo{};
    VkFormat colorFormat = static_cast<VkFormat>(colorFmt);
    VkFormat depthFormat = static_cast<VkFormat>(depthFmt);
    HostSwapchain* scFmt = getFirstSwapchain();
    // Only override colorFormat for blit/present pipelines (0 vertex bindings)
    // that target the swapchain with an LDR format. HDR post-process pipelines
    // (colorFmt >= VK_FORMAT_R16G16B16A16_SFLOAT=97) target internal render targets
    // and must keep their original format to avoid pipeline/RT format mismatch.
    bool isLdrFormat = (colorFmt < 97);
    if (scFmt && dynamicRendering && vtxBindings.empty() && isLdrFormat) {
        fprintf(stderr, "[Decoder] CreatePipeline colorFmt override: %u -> %u (swapchain fmt)\n",
                colorFmt, (uint32_t)scFmt->format);
        colorFormat = scFmt->format;
    } else if (scFmt && dynamicRendering && vtxBindings.empty() && !isLdrFormat) {
        fprintf(stderr, "[Decoder] CreatePipeline colorFmt KEEP HDR: %u (no swapchain override)\n", colorFmt);
    }

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = stageCount;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &vertexInput;
    pInfo.pInputAssemblyState = &inputAsm;
    pInfo.pViewportState = &vpState;
    pInfo.pRasterizationState = &raster;
    pInfo.pMultisampleState = &ms;
    pInfo.pColorBlendState = &cb;
    pInfo.pDepthStencilState = &depthStencil;
    pInfo.pDynamicState = dynState.dynamicStateCount > 0 ? &dynState : nullptr;
    pInfo.layout = lookup(pipelineLayouts_, layoutId);

    if (dynamicRendering) {
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;
        if (depthFmt)
            renderingInfo.depthAttachmentFormat = depthFormat;
        pInfo.pNext = &renderingInfo;
        pInfo.renderPass = VK_NULL_HANDLE;
    } else {
        pInfo.renderPass = lookup(renderPasses_, renderPassId);
        pInfo.subpass = 0;
    }

    fprintf(stderr, "[Decoder] CreatePipeline PRE: layout=%p vertMod=%p fragMod=%p dynState=%p raster=%p\n",
            (void*)pInfo.layout, (void*)(stages[0].module), (void*)(stageCount>1?stages[1].module:VK_NULL_HANDLE),
            (void*)pInfo.pDynamicState, (void*)pInfo.pRasterizationState);
    VkPipeline pipeline;
    VkResult vr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline);
    fprintf(stderr, "[Decoder] CreatePipeline result=%d\n", (int)vr);
    if (vr != VK_SUCCESS) {
        store(pipelines_, pipelineId, (VkPipeline)VK_NULL_HANDLE);
        return;
    }
    store(pipelines_, pipelineId, pipeline);
}

void VnDecoder::handleCreateFramebuffer(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t fbId = r.readU64();
    uint64_t rpId = r.readU64();
    uint64_t ivId = r.readU64();
    uint32_t w = r.readU32();
    uint32_t h = r.readU32();

    VkImageView iv = lookup(imageViews_, ivId);

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = lookup(renderPasses_, rpId);
    info.attachmentCount = 1;
    info.pAttachments = &iv;
    info.width = w;
    info.height = h;
    info.layers = 1;

    VkFramebuffer fb;
    if (vkCreateFramebuffer(device_, &info, nullptr, &fb) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(framebuffers_, fbId, fb);
}

void VnDecoder::handleCreateCommandPool(VnStreamReader& r) {
    VnDecode_vkCreateCommandPool a;
    vn_decode_vkCreateCommandPool(&r, &a);
    fprintf(stderr, "[Decoder] CreateCmdPool: id=%llu flags=%u family=%u\n",
            (unsigned long long)a.pCommandPool, a.pCreateInfo_flags, a.pCreateInfo_queueFamilyIndex);

    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = a.pCreateInfo_flags | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = a.pCreateInfo_queueFamilyIndex;

    VkCommandPool pool;
    VkResult vr = vkCreateCommandPool(device_, &info, nullptr, &pool);
    fprintf(stderr, "[Decoder] CreateCmdPool result=%d pool=%p\n", (int)vr, (void*)pool);
    fflush(stderr);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] CreateCmdPool FAILED\n");
        error_ = true;
        return;
    }
    store(commandPools_, a.pCommandPool, pool);
}

void VnDecoder::handleAllocateCommandBuffers(VnStreamReader& r) {
    // Debug: print raw bytes at current position
    const uint8_t* p = r.currentPtr();
    fprintf(stderr, "[Decoder] AllocCmdBuf raw: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
            p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
            p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
            p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23]);

    uint64_t deviceId = r.readU64();
    uint64_t poolId = r.readU64();
    uint64_t cbId = r.readU64();

    VkCommandPool pool = lookup(commandPools_, poolId);
    fprintf(stderr, "[Decoder] AllocCmdBuf: dev=%llu pool=%llu->%p cb=%llu\n",
            (unsigned long long)deviceId, (unsigned long long)poolId, (void*)pool, (unsigned long long)cbId);
    if (!pool) { error_ = true; return; }

    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    VkCommandBuffer cb;
    VkResult vr = vkAllocateCommandBuffers(device_, &info, &cb);
    fprintf(stderr, "[Decoder] AllocCmdBuf result=%d cb=%p\n", (int)vr, (void*)cb);
    fflush(stderr);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] AllocCmdBuf FAILED\n");
        error_ = true;
        return;
    }
    store(commandBuffers_, cbId, cb);
}

// --- Command buffer recording ---

void VnDecoder::handleBeginCommandBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
#ifdef VBOXGPU_VERBOSE
    fprintf(stderr, "[Decoder] BeginCmdBuf: cbId=0x%llx -> cb=%p (map size=%zu)\n",
            (unsigned long long)cbId, (void*)cb, commandBuffers_.size());
    fflush(stderr);
#endif
    if (!cb) { error_ = true; return; }

    // Ensure CB is idle before resetting.
    // For CBs in batched submits, cbLastFence_ may be null; fall back to lastBatchFence_.
    {
        VkFence waitFence = VK_NULL_HANDLE;
        auto it = cbLastFence_.find(cbId);
        if (it != cbLastFence_.end() && it->second != VK_NULL_HANDLE)
            waitFence = it->second;
        else if (lastBatchFence_ != VK_NULL_HANDLE)
            waitFence = lastBatchFence_;

        if (waitFence != VK_NULL_HANDLE) {
            // Use 5-second timeout to avoid hanging forever on device lost.
            VkResult wr = vkWaitForFences(device_, 1, &waitFence, VK_TRUE, 5000000000ULL);
            if (wr != VK_SUCCESS) {
                fprintf(stderr, "[Decoder] BeginCB FENCE WAIT FAILED: cbId=0x%llx result=%d\n",
                        (unsigned long long)cbId, (int)wr);
                error_ = true; return;
            }
        }
    }
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VkResult beginRes = vkBeginCommandBuffer(cb, &info);
    fprintf(stderr, "[Decoder] BeginCB: cbId=0x%llx cb=%p result=%d\n",
            (unsigned long long)cbId, (void*)cb, (int)beginRes);
    if (beginRes != VK_SUCCESS) { error_ = true; return; }

    // Global memory barrier at start of every CB recording.
    // Compensates for stripped wait semaphores: ensures all GPU writes from
    // prior queue submissions on the same queue are visible when this CB runs.
    // Required for multi-CB pipelines (e.g., 3D render CB → blit CB) where
    // the cross-CB synchronization semaphores were stripped by the host decoder.
    VkMemoryBarrier memBarrier{};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

void VnDecoder::handleEndCommandBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkResult vr = vkEndCommandBuffer(cb);
    fprintf(stderr, "[Decoder] EndCB: cbId=0x%llx cb=%p result=%d\n",
            (unsigned long long)cbId, (void*)cb, (int)vr);
    fflush(stderr);
    if (vr != VK_SUCCESS) { error_ = true; }
}

void VnDecoder::handleCmdBeginRenderPass(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t rpId = r.readU64();
    uint64_t fbId = r.readU64();
    uint32_t w = r.readU32();
    uint32_t h = r.readU32();
    float cr = r.readF32(), cg = r.readF32(), cb_ = r.readF32(), ca = r.readF32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkRenderPass rp = lookup(renderPasses_, rpId);
    VkFramebuffer fb = lookup(framebuffers_, fbId);
    if (!cb || !rp || !fb) {
        fprintf(stderr, "[Decoder] BeginRenderPass SKIP: cb=%p rp=%p fb=%p (ids: cb=%u rp=%u fb=%u)\n",
                (void*)cb, (void*)rp, (void*)fb,
                (unsigned)cbId, (unsigned)rpId, (unsigned)fbId);
        return;
    }

    VkClearValue clearVal = {{{cr, cg, cb_, ca}}};
    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = rp;
    info.framebuffer = fb;
    info.renderArea = {{0,0}, {w, h}};
    info.clearValueCount = 1;
    info.pClearValues = &clearVal;

    vkCmdBeginRenderPass(cb, &info, VK_SUBPASS_CONTENTS_INLINE);
    activeRendering_ = true;
}

void VnDecoder::handleCmdEndRenderPass(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb || !activeRendering_) return;
    activeRendering_ = false;
    vkCmdEndRenderPass(cb);
}

void VnDecoder::handleCmdBeginRendering(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t areaX = r.readU32();
    uint32_t areaY = r.readU32();
    uint32_t areaW = r.readU32();
    uint32_t areaH = r.readU32();
    uint32_t loadOp = r.readU32();
    uint32_t storeOp = r.readU32();
    float cr = r.readF32(), cg = r.readF32(), cb_ = r.readF32(), ca = r.readF32();
    uint64_t imageViewId = r.readU64(); // 0 = swapchain, nonzero = specific view

    // Depth attachment (appended field — check remaining for backward compat)
    uint32_t hasDepth = 0;
    uint64_t depthViewId = 0;
    uint32_t depthLoadOp = 0, depthStoreOp = 0;
    float clearDepth = 1.0f;
    if (r.remaining() >= 4) {
        hasDepth = r.readU32();
        if (hasDepth && r.remaining() >= 20) { // u64 + u32 + u32 + f32 = 20 bytes
            depthViewId = r.readU64();
            depthLoadOp = r.readU32();
            depthStoreOp = r.readU32();
            clearDepth = r.readF32();
        }
    }

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;

    HostSwapchain* sc = nullptr;
    for (auto& [id, s] : swapchains_) { sc = &s; break; }

    VkImageView targetView = VK_NULL_HANDLE;
    VkImage targetImage = VK_NULL_HANDLE;
    bool isSwapchain = false;

    if (imageViewId != 0) {
        targetView = lookup(imageViews_, imageViewId);
        fprintf(stderr, "[Decoder] BeginRendering lookup: ivId=%llu -> view=%p (map size=%zu)\n",
                (unsigned long long)imageViewId, (void*)targetView, imageViews_.size());
    }
    // Only redirect to swapchain when the target is the swapchain itself
    // (imageViewId=0, i.e. DXVK's present/blit pass targeting the swapchain backbuffer).
    // When imageViewId != 0 and the view exists, it's an internal render target —
    // let DXVK render to it so the subsequent blit pass can read from it.
    if (imageViewId == 0 && sc && !sc->imageViews.empty()) {
        targetView = sc->imageViews[sc->currentImageIndex];
        targetImage = sc->images[sc->currentImageIndex];
        isSwapchain = true;
    } else if (!targetView && sc && !sc->imageViews.empty()) {
        // Fallback: imageViewId was nonzero but lookup failed — redirect to swapchain
        targetView = sc->imageViews[sc->currentImageIndex];
        targetImage = sc->images[sc->currentImageIndex];
        isSwapchain = true;
    }

    if (!targetView) {
        activeRendering_ = false;
        return;
    }
    activeRendering_ = true;
    activeRenderingIsSwapchain_ = isSwapchain;
    cbIsSwapchain_[cbId] = isSwapchain; // per-CB tracking (survives interleaved CBs)

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = targetView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = static_cast<VkAttachmentLoadOp>(loadOp);
    colorAttachment.storeOp = static_cast<VkAttachmentStoreOp>(storeOp);
    colorAttachment.clearValue.color = {{cr, cg, cb_, ca}};

    // Clamp render area only for swapchain passes (internal RTs keep their full size)
    uint32_t clampedW = areaW, clampedH = areaH;
    if (isSwapchain && sc) {
        clampedW = std::min(areaW, sc->extent.width);
        clampedH = std::min(areaH, sc->extent.height);
    }

    // Depth attachment info
    VkRenderingAttachmentInfo depthAttachment{};
    VkImageView depthView = VK_NULL_HANDLE;
    if (hasDepth) {
        depthView = lookup(imageViews_, depthViewId);
        if (depthView) {
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = depthView;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = static_cast<VkAttachmentLoadOp>(depthLoadOp);
            depthAttachment.storeOp = static_cast<VkAttachmentStoreOp>(depthStoreOp);
            depthAttachment.clearValue.depthStencil = {clearDepth, 0};
        }
    }

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{(int32_t)areaX, (int32_t)areaY}, {clampedW, clampedH}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    if (hasDepth && depthView)
        renderingInfo.pDepthAttachment = &depthAttachment;

    // Transition image to COLOR_ATTACHMENT_OPTIMAL (only for swapchain — non-swapchain images
    // are transitioned by the rendering commands themselves)
    if (isSwapchain && targetImage) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = targetImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    fprintf(stderr, "[Decoder] BeginRendering: cb=%p view=%p (ivId=%llu %s) area=%u,%u,%ux%u load=%u depth=%u clear=(%.2f,%.2f,%.2f,%.2f)\n",
            (void*)cb, (void*)targetView, (unsigned long long)imageViewId,
            isSwapchain ? "swapchain" : "internal", areaX, areaY, clampedW, clampedH, loadOp, hasDepth,
            cr, cg, cb_, ca);
    fflush(stderr);
    vkCmdBeginRendering(cb, &renderingInfo);

    // Removed — DXVK sets viewport/scissor via dynamic state commands
}

void VnDecoder::handleCmdEndRendering(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;

    // Use per-CB tracking to determine if this CB was in a swapchain render pass.
    // The global activeRenderingIsSwapchain_ flag is unreliable when multiple CBs
    // are recorded concurrently and interleave their BeginRendering calls.
    auto it = cbIsSwapchain_.find(cbId);
    if (it == cbIsSwapchain_.end()) {
        // CB not in a rendering pass — guard against double-EndRendering. Calling
        // vkCmdEndRendering outside a render pass causes NVIDIA driver null-deref crash.
        fprintf(stderr, "[Decoder] EndRendering SKIP: cbId=0x%llx not in rendering state\n",
                (unsigned long long)cbId);
        fflush(stderr);
        return;
    }
    bool isSwapchainCB = it->second;
    cbIsSwapchain_.erase(it);  // erase (not reset to false) to prevent spurious second call

    fprintf(stderr, "[Decoder] EndRendering: cbId=0x%llx cb=%p swapchain=%d imgIdx=%u\n",
            (unsigned long long)cbId, (void*)cb, (int)isSwapchainCB,
            getFirstSwapchain() ? getFirstSwapchain()->currentImageIndex : 999);
    fflush(stderr);
    vkCmdEndRendering(cb);
    // DXVK sends its own vkCmdPipelineBarrier2 in the command stream to transition
    // the swapchain image COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR before QueuePresent.
    // Do NOT insert an extra barrier here — it causes double-transition → device lost.
}

void VnDecoder::handleCmdBindPipeline(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t bindPoint = r.readU32();
    uint64_t pipId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipeline pip = lookup(pipelines_, pipId);
    if (!cb || !pip) {
        fprintf(stderr, "[Decoder] BindPipeline SKIP: cb=%p pip=%p (cbId=%llu pipId=%llu)\n",
                (void*)cb, (void*)pip, (unsigned long long)cbId, (unsigned long long)pipId);
        return;
    }
    vkCmdBindPipeline(cb, static_cast<VkPipelineBindPoint>(bindPoint), pip);
    // Dynamic state (viewport, scissor, cull mode, etc.) persists across pipeline
    // binds in Vulkan — do NOT override here; DXVK manages these via Set* commands.
}

void VnDecoder::handleCmdSetViewport(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkViewport vp;
    vp.x = r.readF32(); vp.y = r.readF32();
    vp.width = r.readF32(); vp.height = r.readF32();
    vp.minDepth = r.readF32(); vp.maxDepth = r.readF32();
    fprintf(stderr, "[Decoder] SetViewport: x=%.1f y=%.1f w=%.1f h=%.1f\n", vp.x, vp.y, vp.width, vp.height);
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;
    // Use WithCount — pipeline declares VIEWPORT_WITH_COUNT for dynamic rendering
    static PFN_vkCmdSetViewportWithCount pfn = nullptr;
    if (!pfn) pfn = (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(device_, "vkCmdSetViewportWithCount");
    if (pfn) pfn(cb, 1, &vp);
    else vkCmdSetViewport(cb, 0, 1, &vp);
}

void VnDecoder::handleCmdSetScissor(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkRect2D sc;
    sc.offset.x = r.readI32(); sc.offset.y = r.readI32();
    sc.extent.width = r.readU32(); sc.extent.height = r.readU32();
    static int scLog = 0;
    if (scLog++ < 5)
        fprintf(stderr, "[Decoder] SetScissor: cb=%llu (%d,%d,%u,%u)\n",
                (unsigned long long)cbId, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;
    // Use WithCount — pipeline declares SCISSOR_WITH_COUNT for dynamic rendering
    static PFN_vkCmdSetScissorWithCount pfn = nullptr;
    if (!pfn) pfn = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(device_, "vkCmdSetScissorWithCount");
    if (pfn) pfn(cb, 1, &sc);
    else vkCmdSetScissor(cb, 0, 1, &sc);
}

void VnDecoder::handleCmdSetCullMode(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t cullMode = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    static int cullLog = 0;
    if (cullLog++ < 5)
        fprintf(stderr, "[Decoder] SetCullMode: cb=%llu mode=%u (0=NONE,1=FRONT,2=BACK)\n",
                (unsigned long long)cbId, cullMode);
    if (cb) vkCmdSetCullMode(cb, static_cast<VkCullModeFlags>(cullMode));
}

void VnDecoder::handleCmdSetFrontFace(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t frontFace = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetFrontFace(cb, static_cast<VkFrontFace>(frontFace));
}

void VnDecoder::handleCmdSetPrimitiveTopology(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t topology = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetPrimitiveTopology(cb, static_cast<VkPrimitiveTopology>(topology));
}

void VnDecoder::handleCmdSetDepthTestEnable(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t enable = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    static int dtLog = 0;
    if (dtLog++ < 5)
        fprintf(stderr, "[Decoder] SetDepthTestEnable: cb=%llu enable=%u\n",
                (unsigned long long)cbId, enable);
    if (cb) vkCmdSetDepthTestEnable(cb, enable);
}

void VnDecoder::handleCmdSetDepthWriteEnable(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t enable = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetDepthWriteEnable(cb, enable);
}

void VnDecoder::handleCmdSetDepthCompareOp(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t compareOp = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    static int depthLog = 0;
    if (depthLog++ < 5)
        fprintf(stderr, "[Decoder] SetDepthCompareOp: cb=%llu op=%u\n",
                (unsigned long long)cbId, compareOp);
    if (cb) vkCmdSetDepthCompareOp(cb, static_cast<VkCompareOp>(compareOp));
}

void VnDecoder::handleCmdSetDepthBoundsTestEnable(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t enable = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetDepthBoundsTestEnable(cb, enable);
}

void VnDecoder::handleCmdSetDepthBiasEnable(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t enable = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetDepthBiasEnable(cb, enable);
}

void VnDecoder::handleCmdBindVertexBuffers(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t firstBinding = r.readU32(), bindingCount = r.readU32();
    std::vector<VkBuffer> buffers(bindingCount);
    std::vector<VkDeviceSize> offsets(bindingCount), sizes(bindingCount), strides(bindingCount);
    bool hasStrides = false;
    for (uint32_t i = 0; i < bindingCount; i++) {
        uint64_t bufId = r.readU64();
        buffers[i] = lookup(buffers_, bufId);
        offsets[i] = r.readU64();
        sizes[i] = r.readU64();
        strides[i] = r.readU64();
        if (strides[i] != 0) hasStrides = true;
    }
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;
    static int vbLog = 0;
    if (vbLog < 5000) {
        fprintf(stderr, "[Decoder] BindVB: first=%u count=%u hasStrides=%d",
                firstBinding, bindingCount, (int)hasStrides);
        for (uint32_t i = 0; i < bindingCount; i++)
            fprintf(stderr, " [%u: buf=%p off=%llu sz=%llu stride=%llu]", i,
                    (void*)buffers[i], (unsigned long long)offsets[i],
                    (unsigned long long)sizes[i], (unsigned long long)strides[i]);
        fprintf(stderr, "\n");
        vbLog++;
    }
    if (hasStrides) {
        vkCmdBindVertexBuffers2(cb, firstBinding, bindingCount,
            buffers.data(), offsets.data(), sizes.data(), strides.data());
    } else {
        vkCmdBindVertexBuffers(cb, firstBinding, bindingCount, buffers.data(), offsets.data());
    }
}

void VnDecoder::handleCmdBindIndexBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t bufId = r.readU64();
    uint64_t offset = r.readU64();
    uint32_t indexType = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkBuffer buf = lookup(buffers_, bufId);
    fprintf(stderr, "[Decoder] BindIndexBuffer: cbId=%llu bufId=%llu buf=%p off=%llu type=%u\n",
            (unsigned long long)cbId, (unsigned long long)bufId, (void*)buf,
            (unsigned long long)offset, indexType);
    if (!cb || !buf) return;
    vkCmdBindIndexBuffer(cb, buf, offset, static_cast<VkIndexType>(indexType));
}

void VnDecoder::handleCmdDrawIndexed(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t indexCount = r.readU32(), instanceCount = r.readU32();
    uint32_t firstIndex = r.readU32();
    int32_t vertexOffset = r.readI32();
    uint32_t firstInstance = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb || !activeRendering_) {
        static int diSkip = 0;
        if (diSkip++ < 5)
            fprintf(stderr, "[Decoder] DrawIndexed SKIP: cb=%p active=%d\n", (void*)cb, (int)activeRendering_);
        return;
    }
    static int diLog = 0;
    if (diLog++ < 5000)
        fprintf(stderr, "[Decoder] DrawIndexed: cb=%p idx=%u inst=%u first=%u vtxOff=%d swapchain=%d\n",
                (void*)cb, indexCount, instanceCount, firstIndex, vertexOffset, (int)activeRenderingIsSwapchain_);
    vkCmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VnDecoder::handleCmdCopyBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t srcId = r.readU64(), dstId = r.readU64();
    uint32_t regionCount = r.readU32();
    std::vector<VkBufferCopy> regions(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        regions[i].srcOffset = r.readU64();
        regions[i].dstOffset = r.readU64();
        regions[i].size = r.readU64();
    }
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkBuffer src = lookup(buffers_, srcId), dst = lookup(buffers_, dstId);
    if (!cb || !src || !dst) {
        static int cpSkip = 0;
        if (cpSkip++ < 5)
            fprintf(stderr, "[Decoder] CopyBuffer SKIP: cb=%p src=%llu(%p) dst=%llu(%p)\n",
                    (void*)cb, (unsigned long long)srcId, (void*)src,
                    (unsigned long long)dstId, (void*)dst);
        return;
    }

    // Staging snapshot: same as CopyBufferToImage — protect source data from
    // later WriteMemory overwriting it before GPU executes the copy.
    // Staging snapshot disabled for debugging — use original buffer
    vkCmdCopyBuffer(cb, src, dst, regionCount, regions.data());
}

void VnDecoder::handleCmdCopyImage(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t srcImgId = r.readU64();
    uint32_t srcLayout = r.readU32();
    uint64_t dstImgId = r.readU64();
    uint32_t dstLayout = r.readU32();
    uint32_t regionCount = r.readU32();
    std::vector<VkImageCopy> regions(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        regions[i].srcSubresource.aspectMask = r.readU32();
        regions[i].srcSubresource.mipLevel = r.readU32();
        regions[i].srcSubresource.baseArrayLayer = r.readU32();
        regions[i].srcSubresource.layerCount = r.readU32();
        regions[i].srcOffset.x = r.readI32();
        regions[i].srcOffset.y = r.readI32();
        regions[i].srcOffset.z = r.readI32();
        regions[i].dstSubresource.aspectMask = r.readU32();
        regions[i].dstSubresource.mipLevel = r.readU32();
        regions[i].dstSubresource.baseArrayLayer = r.readU32();
        regions[i].dstSubresource.layerCount = r.readU32();
        regions[i].dstOffset.x = r.readI32();
        regions[i].dstOffset.y = r.readI32();
        regions[i].dstOffset.z = r.readI32();
        regions[i].extent.width = r.readU32();
        regions[i].extent.height = r.readU32();
        regions[i].extent.depth = r.readU32();
    }
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkImage src = lookup(images_, srcImgId);
    VkImage dst = lookup(images_, dstImgId);
    static int ciLog = 0;
    if (ciLog++ < 10)
        fprintf(stderr, "[Decoder] CopyImage: src=%llu(%p) dst=%llu(%p) regions=%u %ux%u\n",
                (unsigned long long)srcImgId, (void*)src,
                (unsigned long long)dstImgId, (void*)dst, regionCount,
                regionCount>0 ? regions[0].extent.width : 0,
                regionCount>0 ? regions[0].extent.height : 0);
    if (!cb || !src || !dst) {
        if (ciLog <= 10)
            fprintf(stderr, "[Decoder] CopyImage SKIP: cb=%p src=%p dst=%p\n", (void*)cb, (void*)src, (void*)dst);
        return;
    }
    vkCmdCopyImage(cb, src, static_cast<VkImageLayout>(srcLayout),
                   dst, static_cast<VkImageLayout>(dstLayout),
                   regionCount, regions.data());
}

void VnDecoder::handleCmdBlitImage(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t srcImgId = r.readU64();
    uint32_t srcLayout = r.readU32();
    uint64_t dstImgId = r.readU64();
    uint32_t dstLayout = r.readU32();
    uint32_t regionCount = r.readU32();
    std::vector<VkImageBlit> regions(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        regions[i].srcSubresource.aspectMask = r.readU32();
        regions[i].srcSubresource.mipLevel = r.readU32();
        regions[i].srcSubresource.baseArrayLayer = r.readU32();
        regions[i].srcSubresource.layerCount = r.readU32();
        for (int j = 0; j < 2; j++) {
            regions[i].srcOffsets[j].x = r.readI32();
            regions[i].srcOffsets[j].y = r.readI32();
            regions[i].srcOffsets[j].z = r.readI32();
        }
        regions[i].dstSubresource.aspectMask = r.readU32();
        regions[i].dstSubresource.mipLevel = r.readU32();
        regions[i].dstSubresource.baseArrayLayer = r.readU32();
        regions[i].dstSubresource.layerCount = r.readU32();
        for (int j = 0; j < 2; j++) {
            regions[i].dstOffsets[j].x = r.readI32();
            regions[i].dstOffsets[j].y = r.readI32();
            regions[i].dstOffsets[j].z = r.readI32();
        }
    }
    uint32_t filter = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkImage src = lookup(images_, srcImgId);
    VkImage dst = lookup(images_, dstImgId);
    static int blitLog = 0;
    if (blitLog++ < 5)
        fprintf(stderr, "[Decoder] BlitImage: src=%llu(%p) dst=%llu(%p) regions=%u filter=%u\n",
                (unsigned long long)srcImgId, (void*)src,
                (unsigned long long)dstImgId, (void*)dst, regionCount, filter);
    if (!cb || !src || !dst) return;
    vkCmdBlitImage(cb, src, static_cast<VkImageLayout>(srcLayout),
                   dst, static_cast<VkImageLayout>(dstLayout),
                   regionCount, regions.data(), static_cast<VkFilter>(filter));
}

bool VnDecoder::ensureCopyStagingBuf(VkDeviceSize needed) {
    // Arena: check if current buffer has room from copyStagingUsed_
    VkDeviceSize totalNeeded = copyStagingUsed_ + needed;
    if (copyStagingBuf_.capacity >= totalNeeded && copyStagingBuf_.buffer)
        return true;
    // Need larger buffer — defer destruction of old buffer until GPU is done
    // (command buffers already recorded may reference the old VkBuffer handle).
    copyStagingUsed_ = 0;
    totalNeeded = needed;
    if (copyStagingBuf_.buffer) {
        fprintf(stderr, "[Decoder] StagingBuf REALLOC: oldCap=%llu needed=%llu\n",
                (unsigned long long)copyStagingBuf_.capacity,
                (unsigned long long)needed);
        // DO NOT put old buffer in pendingDestroys_: CBs spanning multiple batches may
        // still reference this VkBuffer handle (recorded with vkCmdCopyBufferToImage).
        // If freed early (when those CBs haven't been submitted yet), the GPU will
        // access freed memory → VK_ERROR_DEVICE_LOST.
        // Instead, keep old buffers alive in retiredStagingBufs_ until cleanup().
        retiredStagingBufs_.push_back(copyStagingBuf_);
    }
    copyStagingBuf_ = {};

    // Round up to 256KB granularity, at least 2x totalNeeded for arena headroom
    VkDeviceSize cap = totalNeeded * 2;
    cap = (cap + 0x3FFFF) & ~(VkDeviceSize)0x3FFFF;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = cap;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &copyStagingBuf_.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, copyStagingBuf_.buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(device_, copyStagingBuf_.buffer, nullptr);
        copyStagingBuf_.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &copyStagingBuf_.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, copyStagingBuf_.buffer, nullptr);
        copyStagingBuf_.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device_, copyStagingBuf_.buffer, copyStagingBuf_.memory, 0);
    vkMapMemory(device_, copyStagingBuf_.memory, 0, cap, 0, &copyStagingBuf_.mapped);
    copyStagingBuf_.capacity = cap;
    return true;
}

void VnDecoder::handleCmdCopyBufferToImage(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t srcBufId = r.readU64(), dstImgId = r.readU64();
    uint32_t dstLayout = r.readU32();
    uint32_t regionCount = r.readU32();
    std::vector<VkBufferImageCopy> regions(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        regions[i].bufferOffset = r.readU32();
        regions[i].bufferRowLength = r.readU32();
        regions[i].bufferImageHeight = r.readU32();
        regions[i].imageSubresource.aspectMask = r.readU32();
        regions[i].imageSubresource.mipLevel = r.readU32();
        regions[i].imageSubresource.baseArrayLayer = r.readU32();
        regions[i].imageSubresource.layerCount = r.readU32();
        regions[i].imageOffset.x = r.readI32();
        regions[i].imageOffset.y = r.readI32();
        regions[i].imageOffset.z = r.readI32();
        regions[i].imageExtent.width = r.readU32();
        regions[i].imageExtent.height = r.readU32();
        regions[i].imageExtent.depth = r.readU32();
    }
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkBuffer srcBuf = lookup(buffers_, srcBufId);
    VkImage dstImg = lookup(images_, dstImgId);
    static int copyLog = 0;
    if (copyLog < 5 || (regionCount > 0 && regions[0].imageExtent.width > 100)) {
        copyLog++;
        fprintf(stderr, "[Decoder] CopyBufToImg: cb=%llu(%p) srcBuf=%llu(%p) dstImg=%llu(%p) regions=%u %ux%u dstLayout=%u bufOff=%llu\n",
                (unsigned long long)cbId, (void*)cb,
                (unsigned long long)srcBufId, (void*)srcBuf,
                (unsigned long long)dstImgId, (void*)dstImg, regionCount,
                regionCount>0 ? regions[0].imageExtent.width : 0,
                regionCount>0 ? regions[0].imageExtent.height : 0,
                dstLayout,
                regionCount>0 ? (unsigned long long)regions[0].bufferOffset : 0ULL);
        copyLog++;
    }
    if (!cb || !srcBuf || !dstImg) {
        fprintf(stderr, "[Decoder] CopyBufToImg SKIP: cb=%p srcBuf=%llu(%p) dstImg=%llu(%p)\n",
                (void*)cb, (unsigned long long)srcBufId, (void*)srcBuf,
                (unsigned long long)dstImgId, (void*)dstImg);
        return;
    }

    // TODO: staging snapshot for copy-source protection (currently causes black screen,
    // needs investigation — see dirty_tracking_race_analysis.md "方向 C")
#if 0  // DISABLED — causes all-black frames, root cause TBD
    auto bit_disabled = bufferBindings_.find(srcBufId);
    VkDeviceMemory srcMem = (bit != bufferBindings_.end())
        ? lookup(deviceMemories_, bit->second.memoryId) : VK_NULL_HANDLE;
    VkDeviceSize srcMemOffset = (bit != bufferBindings_.end()) ? bit->second.memoryOffset : 0;

    if (srcMem) {
        // Calculate total byte range needed (min offset → max end)
        VkDeviceSize minOff = UINT64_MAX, maxEnd = 0;
        for (uint32_t i = 0; i < regionCount; i++) {
            VkDeviceSize regSize = (VkDeviceSize)regions[i].imageExtent.width *
                                   regions[i].imageExtent.height *
                                   regions[i].imageExtent.depth * 4; // assume 4 bpp
            VkDeviceSize off = regions[i].bufferOffset;
            if (off < minOff) minOff = off;
            if (off + regSize > maxEnd) maxEnd = off + regSize;
        }
        VkDeviceSize spanSize = maxEnd - minOff;

        if (spanSize > 0 && ensureCopyStagingBuf(spanSize)) {
            VkDeviceSize arenaOff = copyStagingUsed_;
            // Map source memory, snapshot the region into arena
            void* srcMapped = nullptr;
            if (vkMapMemory(device_, srcMem, srcMemOffset + minOff, spanSize, 0, &srcMapped) == VK_SUCCESS) {
                memcpy((uint8_t*)copyStagingBuf_.mapped + arenaOff, srcMapped, (size_t)spanSize);
                vkUnmapMemory(device_, srcMem);
                // Advance arena (align to 256 for buffer offset alignment)
                copyStagingUsed_ = (arenaOff + spanSize + 255) & ~(VkDeviceSize)255;

                // Adjust region offsets: base at arenaOff in staging buffer
                std::vector<VkBufferImageCopy> adjRegions = regions;
                for (uint32_t i = 0; i < regionCount; i++)
                    adjRegions[i].bufferOffset = (VkDeviceSize)(adjRegions[i].bufferOffset - minOff + arenaOff);

                vkCmdCopyBufferToImage(cb, copyStagingBuf_.buffer, dstImg,
                                       static_cast<VkImageLayout>(dstLayout),
                                       regionCount, adjRegions.data());
                return;
            }
        }
    }

#endif
    // Fallback: no binding info or staging alloc failed — use original buffer
    vkCmdCopyBufferToImage(cb, srcBuf, dstImg, static_cast<VkImageLayout>(dstLayout),
                           regionCount, regions.data());
}

void VnDecoder::handleCopyBufToImgInline(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t dstImgId = r.readU64();
    uint32_t dstLayout = r.readU32();
    uint32_t regionCount = r.readU32();
    std::vector<VkBufferImageCopy> regions(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        regions[i].bufferOffset = r.readU32();
        regions[i].bufferRowLength = r.readU32();
        regions[i].bufferImageHeight = r.readU32();
        regions[i].imageSubresource.aspectMask = r.readU32();
        regions[i].imageSubresource.mipLevel = r.readU32();
        regions[i].imageSubresource.baseArrayLayer = r.readU32();
        regions[i].imageSubresource.layerCount = r.readU32();
        regions[i].imageOffset.x = r.readI32();
        regions[i].imageOffset.y = r.readI32();
        regions[i].imageOffset.z = r.readI32();
        regions[i].imageExtent.width = r.readU32();
        regions[i].imageExtent.height = r.readU32();
        regions[i].imageExtent.depth = r.readU32();
    }
    uint32_t dataSize = r.readU32();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkImage dstImg = lookup(images_, dstImgId);
    if (!cb || !dstImg || dataSize == 0) {
        r.skip(dataSize);
        return;
    }

    // Write inline pixel data to staging arena, then CopyBufferToImage from staging.
    // Data is embedded in the command stream — immune to WriteMemory overwrites.
    if (!ensureCopyStagingBuf(dataSize)) {
        r.skip(dataSize);
        return;
    }
    VkDeviceSize arenaOff = copyStagingUsed_;
    r.readBytes((uint8_t*)copyStagingBuf_.mapped + arenaOff, dataSize);
    copyStagingUsed_ = (arenaOff + dataSize + 255) & ~(VkDeviceSize)255;

    // Adjust region bufferOffsets to point into staging arena
    for (uint32_t i = 0; i < regionCount; i++)
        regions[i].bufferOffset += arenaOff;

    vkCmdCopyBufferToImage(cb, copyStagingBuf_.buffer, dstImg,
                           static_cast<VkImageLayout>(dstLayout),
                           regionCount, regions.data());
}

void VnDecoder::handleCmdUpdateBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t bufId = r.readU64();
    uint64_t offset = r.readU64();
    uint64_t dataSize = r.readU64();
    std::vector<uint8_t> data(dataSize);
    r.readBytes(data.data(), dataSize);
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkBuffer buf = lookup(buffers_, bufId);
    if (!cb || !buf) return;
    vkCmdUpdateBuffer(cb, buf, offset, dataSize, data.data());
}

void VnDecoder::handleCmdDraw(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t vertexCount = r.readU32();
    uint32_t instanceCount = r.readU32();
    uint32_t firstVertex = r.readU32();
    uint32_t firstInstance = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb || !activeRendering_) {
        fprintf(stderr, "[Decoder] Draw SKIP: cb=%p activeRendering=%d\n", (void*)cb, (int)activeRendering_);
        return;
    }
    fprintf(stderr, "[Decoder] Draw: verts=%u instances=%u\n", vertexCount, instanceCount);
    vkCmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VnDecoder::handleCmdPushConstants(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t layoutId = r.readU64();
    uint32_t stageFlags = r.readU32();
    uint32_t offset = r.readU32();
    uint32_t size = r.readU32();
    std::vector<uint8_t> data(size);
    r.readBytes(data.data(), size);

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipelineLayout layout = lookup(pipelineLayouts_, layoutId);
    if (!cb || !layout) return;
    vkCmdPushConstants(cb, layout, stageFlags, offset, size, data.data());
}

// --- Sync ---

void VnDecoder::handleCreateSemaphore(VnStreamReader& r) {
    VnDecode_vkCreateSemaphore a;
    vn_decode_vkCreateSemaphore(&r, &a);

    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.flags = a.pCreateInfo_flags;
    VkSemaphore sem;
    if (vkCreateSemaphore(device_, &info, nullptr, &sem) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(semaphores_, a.pSemaphore, sem);
}

void VnDecoder::handleCreateFence(VnStreamReader& r) {
    VnDecode_vkCreateFence a;
    vn_decode_vkCreateFence(&r, &a);

    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = a.pCreateInfo_flags;
    VkFence fence;
    if (vkCreateFence(device_, &info, nullptr, &fence) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(fences_, a.pFence, fence);
}

void VnDecoder::flushPendingSubmits(VkSemaphore waitSem, VkSemaphore sigSem, VkFence userFence) {
    // Only skip if there is truly nothing to submit.
    if (pendingSubmitCBs_.empty() && waitSem == VK_NULL_HANDLE &&
        sigSem == VK_NULL_HANDLE && userFence == VK_NULL_HANDLE)
        return;

    std::vector<VkCommandBuffer> cbs;
    cbs.reserve(pendingSubmitCBs_.size());
    for (auto& p : pendingSubmitCBs_) cbs.push_back(p.cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.commandBufferCount = (uint32_t)cbs.size();
    info.pCommandBuffers = cbs.data();
    if (waitSem) {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &waitSem;
        info.pWaitDstStageMask = &waitStage;
    }
    if (sigSem) {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &sigSem;
    }

    // Allocate one batch fence to cover all CBs in this submit.
    VkFence batchFence = allocateFence();
    VkResult result = vkQueueSubmit(graphicsQueue_, 1, &info, batchFence);
    fprintf(stderr, "[Decoder] BatchSubmit: cbs=%zu waitSem=%p sigSem=%p fence=%p result=%d\n",
            cbs.size(), (void*)waitSem, (void*)sigSem, (void*)batchFence, (int)result);
    fflush(stderr);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] BatchSubmit FAILED!\n");
        error_ = true;
    }

    // Mark all CBs in the batch with null per-CB fence; they use lastBatchFence_ instead.
    // (BeginCommandBuffer falls back to lastBatchFence_ when per-CB fence is null.)
    for (auto& p : pendingSubmitCBs_) {
        auto it = cbLastFence_.find(p.cbId);
        if (it != cbLastFence_.end() && it->second != VK_NULL_HANDLE)
            recycleFence(it->second);  // only recycle valid handles
        cbLastFence_[p.cbId] = VK_NULL_HANDLE;
    }
    lastBatchFence_ = batchFence;
    lastBatchWaitPending_ = true;

    pendingSubmitCBs_.clear();

    // Signal the stream-side fence if provided.
    // vkQueueSubmit only accepts one fence per call, so we use a separate empty submit.
    // Queued after the main batch, it signals userFence once all prior GPU work completes.
    if (userFence != VK_NULL_HANDLE) {
        VkSubmitInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkResult fr = vkQueueSubmit(graphicsQueue_, 1, &fenceInfo, userFence);
        if (fr != VK_SUCCESS)
            fprintf(stderr, "[Decoder] FenceSubmit FAILED! result=%d\n", (int)fr);
    }
}

void VnDecoder::handleQueueSubmit(VnStreamReader& r) {
    uint64_t queueId = r.readU64();
    uint64_t cbId = r.readU64();
    uint64_t waitSemId = r.readU64();
    uint64_t signalSemId = r.readU64();
    uint64_t fenceId = r.readU64();

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkSemaphore waitSem = lookup(semaphores_, waitSemId);
    VkSemaphore sigSem = lookup(semaphores_, signalSemId);
    VkFence fence = lookup(fences_, fenceId);

    fprintf(stderr, "[Decoder] QueueSubmit: cb=%llu->%p waitSem=%llu sigSem=%llu fence=%llu\n",
            (unsigned long long)cbId, (void*)cb,
            (unsigned long long)waitSemId, (unsigned long long)signalSemId,
            (unsigned long long)fenceId);
    fflush(stderr);

    // Semaphore signal tracking: mark sigSem as pending-signal so future waitSem can proceed.
    if (signalSemId != 0)
        semPendingSignal_.insert(signalSemId);

    // If waitSem was never put in pending-signal state (i.e., nobody called sigSem or
    // vkAcquireNextImageKHR with it), the GPU would deadlock waiting for it.
    // This happens with ICD acquire semaphores that are silently dropped by icd_vkAcquireNextImageKHR.
    // Skip the wait to avoid GPU stalls.
    if (waitSemId != 0 && waitSem != VK_NULL_HANDLE) {
        if (semPendingSignal_.count(waitSemId) == 0) {
            fprintf(stderr, "[Decoder] QueueSubmit: SKIP waitSem=%llu (never signaled, would deadlock)\n",
                    (unsigned long long)waitSemId);
            fflush(stderr);
            waitSem = VK_NULL_HANDLE;  // suppress the wait
            waitSemId = 0;
        } else {
            semPendingSignal_.erase(waitSemId);  // consumed
        }
    }

    bool hasSync = (waitSem != VK_NULL_HANDLE || sigSem != VK_NULL_HANDLE ||
                    fence != VK_NULL_HANDLE || waitSemId != 0);

    if (!hasSync) {
        // No synchronization primitives — collect for batch submission.
        // The ICD splits a multi-CB vkQueueSubmit into individual cmdQueueSubmit
        // commands. Without batching, the host submits them as separate
        // vkQueueSubmit calls, which have no ordering guarantee in the Vulkan spec.
        // Collecting and submitting together in one call restores ordering.
        if (cb) pendingSubmitCBs_.push_back({cbId, cb});
        return;
    }

    // Has synchronization — flush any pending CBs first, then submit this one.
    // Add current CB to the batch so it's submitted together with the pending ones.
    if (cb) pendingSubmitCBs_.push_back({cbId, cb});
    flushPendingSubmits(waitSem, sigSem, fence);
}

void VnDecoder::handleWaitForFences(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint32_t fenceCount = r.readU32();
    std::vector<VkFence> fences(fenceCount);
    for (uint32_t i = 0; i < fenceCount; i++)
        fences[i] = lookup(fences_, r.readU64());
    uint32_t waitAll = r.readU32();
    uint64_t timeout = r.readU64();
    (void)deviceId;
    // Remove null fences (unknown IDs)
    fences.erase(std::remove(fences.begin(), fences.end(), VK_NULL_HANDLE), fences.end());
    fprintf(stderr, "[Decoder] WaitForFences: count=%u valid=%zu fencePtrs=[",
            fenceCount, fences.size(), waitAll, (unsigned long long)timeout);
    // Re-read fence IDs for logging (we already consumed them, so log the VkFence pointers)
    for (size_t i = 0; i < fences.size(); i++)
        fprintf(stderr, "%s%p", i ? "," : "", (void*)fences[i]);
    fprintf(stderr, "]\n");
    fflush(stderr);
    if (!fences.empty()) {
        // Cap timeout to 5s in replay mode to prevent infinite GPU deadlock hangs.
        // In live mode the guest drives the pace; in replay the GPU may deadlock
        // on a semaphore that was never re-signaled (acquire-sem reuse across frames).
        const uint64_t MAX_WAIT = 5000000000ULL; // 5s
        if (timeout > MAX_WAIT) timeout = MAX_WAIT;
        VkResult wr = vkWaitForFences(device_, (uint32_t)fences.size(), fences.data(),
                        waitAll ? VK_TRUE : VK_FALSE, timeout);
        if (wr == VK_TIMEOUT)
            fprintf(stderr, "[Decoder] WaitForFences TIMEOUT (GPU deadlock?)\n");
    }
}

void VnDecoder::handleResetFences(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint32_t fenceCount = r.readU32();
    std::vector<VkFence> fences(fenceCount);
    for (uint32_t i = 0; i < fenceCount; i++)
        fences[i] = lookup(fences_, r.readU64());
    (void)deviceId;
    fences.erase(std::remove(fences.begin(), fences.end(), VK_NULL_HANDLE), fences.end());
    if (!fences.empty())
        vkResetFences(device_, (uint32_t)fences.size(), fences.data());
}

// --- Bridge-specific: swapchain ---

void VnDecoder::handleBridgeCreateSwapchain(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t scId = r.readU64();
    uint32_t width = r.readU32();
    uint32_t height = r.readU32();
    uint32_t imageCount = r.readU32();

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDevice_, surface_, &caps);

    // Pick format
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice_, surface_, &fmtCount, fmts.data());

    // Use the first supported surface format (don't force SRGB — Host GPU may not support it)
    VkSurfaceFormatKHR surfFmt = fmts[0];

    HostSwapchain sc{};
    sc.format = surfFmt.format;
    sc.extent = { width, height };
    if (caps.currentExtent.width != UINT32_MAX)
        sc.extent = caps.currentExtent;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = sc.format;
    info.imageColorSpace = surfFmt.colorSpace;
    info.imageExtent = sc.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    fprintf(stderr, "[Decoder] CreateSwapchain: format=%u extent=%ux%u imageCount=%u\n",
            (unsigned)sc.format, sc.extent.width, sc.extent.height, imageCount);
    fflush(stderr);

    if (vkCreateSwapchainKHR(device_, &info, nullptr, &sc.swapchain) != VK_SUCCESS) {
        error_ = true;
        return;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, sc.swapchain, &count, nullptr);
    sc.images.resize(count);
    vkGetSwapchainImagesKHR(device_, sc.swapchain, &count, sc.images.data());

    // Create image views and register them with sequential IDs starting from scId+1
    sc.imageViews.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo ivInfo{};
        ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image = sc.images[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = sc.format;
        ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device_, &ivInfo, nullptr, &sc.imageViews[i]);
        // Register image views with IDs: scId*100 + i + 1
        store(imageViews_, scId * 100 + i + 1, sc.imageViews[i]);
    }

    // Acquire first image so rendering can start immediately
    vkAcquireNextImageKHR(device_, sc.swapchain, UINT64_MAX,
                          VK_NULL_HANDLE, acquireFence_, &sc.currentImageIndex);
    vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &acquireFence_);

    store(swapchains_, scId, sc);
}

void VnDecoder::handleBridgeAcquireNextImage(VnStreamReader& r) {
    uint64_t scId = r.readU64();
    uint64_t semId = r.readU64();

    auto it = swapchains_.find(scId);
    if (it == swapchains_.end()) { error_ = true; return; }

    VkSemaphore sem = lookup(semaphores_, semId);
    VkResult res = vkAcquireNextImageKHR(device_, it->second.swapchain, UINT64_MAX,
                          sem, VK_NULL_HANDLE, &it->second.currentImageIndex);
    // Mark semaphore as pending-signal (presentation engine will signal it when image is available)
    if (semId != 0)
        semPendingSignal_.insert(semId);
    fprintf(stderr, "[Decoder] AcquireNextImage: sc=%llu semId=%llu sem=%p imgIdx=%u result=%d\n",
            (unsigned long long)scId, (unsigned long long)semId,
            (void*)sem, it->second.currentImageIndex, (int)res);
    fflush(stderr);
}

void VnDecoder::handleBridgeQueuePresent(VnStreamReader& r) {
    uint64_t queueId = r.readU64();
    uint64_t scId = r.readU64();
    uint64_t waitSemId = r.readU64();

    // Defer present to end of batch — the corresponding QueueSubmit may come later
    // in the stream due to multi-threaded encoding in the ICD.
    pendingPresents_.push_back({queueId, scId, waitSemId});
    fprintf(stderr, "[Decoder] Present DEFERRED: sc=%llu waitSem=%llu (will flush at batch end)\n",
            (unsigned long long)scId, (unsigned long long)waitSemId);
}

void VnDecoder::handleGetBufferDeviceAddress(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t bufferId = r.readU64();
    (void)deviceId;

    VkBuffer buf = lookup(buffers_, bufferId);
    VkDeviceAddress addr = 0;
    if (buf) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buf;
        addr = vkGetBufferDeviceAddress(device_, &info);
    }
    fprintf(stderr, "[Decoder] GetBufferDeviceAddress: id=%llu buf=%p addr=0x%llx\n",
            (unsigned long long)bufferId, (void*)buf, (unsigned long long)addr);
    pendingBdaResults_.push_back({bufferId, addr});
}

void VnDecoder::handleBridgeRecordBDA(VnStreamReader& r) {
    uint64_t bufferId = r.readU64();
    uint64_t liveAddr = r.readU64();

    // Build live→replay mapping: look up this buffer's replay GPU address from AutoBDA.
    // Called during replay after BindBufferMemory (which populates replayBdaByBufferId_).
    auto it = replayBdaByBufferId_.find(bufferId);
    if (it != replayBdaByBufferId_.end()) {
        liveBdaToReplayBda_[liveAddr] = it->second;
        fprintf(stderr, "[RecordBDA] buf=%llu live=0x%llx -> replay=0x%llx\n",
                (unsigned long long)bufferId, (unsigned long long)liveAddr,
                (unsigned long long)it->second);
    } else {
        // Should not happen: BindBufferMemory always precedes vkGetBufferDeviceAddress
        fprintf(stderr, "[RecordBDA] buf=%llu live=0x%llx (no replay addr — BDA buffer not yet bound?)\n",
                (unsigned long long)bufferId, (unsigned long long)liveAddr);
    }
}

void VnDecoder::flushPendingPresents() {
    for (auto& pp : pendingPresents_) {
        auto it = swapchains_.find(pp.scId);
        if (it == swapchains_.end()) continue;

        // No vkDeviceWaitIdle: render and readback are on the same queue.
        // The pipeline barrier inside readbackFrameAsync ensures render finishes
        // before the copy starts (ALL_COMMANDS_BIT → TRANSFER_BIT barrier).

        int curSlot = rbCur_;
        int prevSlot = 1 - rbCur_;

        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.swapchainCount = 1;
        info.pSwapchains = &it->second.swapchain;
        info.pImageIndices = &it->second.currentImageIndex;

        uint32_t presentIdx = it->second.currentImageIndex;

#ifdef VBOXGPU_DEBUG_SCREENSHOTS
        // Capture screenshots at specific frames for animation debugging
        static int dbgFr2 = 0;
        dbgFr2++;
        if (dbgFr2 == 5 || dbgFr2 == 50 || dbgFr2 == 150) {
            uint32_t savedLPI = lastPresentedImageIndex_;
            lastPresentedImageIndex_ = presentIdx;
            char path[256];
            snprintf(path, sizeof(path), "S:/bld/vboxgpu/dbg_frame%d.bmp", dbgFr2);
            captureScreenshot(path);
            lastPresentedImageIndex_ = savedLPI;
            fprintf(stderr, "[Decoder] Screenshot saved: %s (presentIdx=%u)\n", path, presentIdx);
        }
#endif

        // Submit async readback to current slot (non-blocking).
        // GPU copies swapchain image → readback_[curSlot] staging buffer.
        // We will wait for this fence next frame when curSlot becomes prevSlot.
        if (!noReadback_) {
            readbackSubmitted_[curSlot] = readbackFrameAsync(presentIdx, it->second, curSlot);
        }

        VkResult vr = vkQueuePresentKHR(graphicsQueue_, &info);

#if VBOXGPU_TIMING
        // Tag this readback slot with frame ID and present timestamp
        frameCounter_++;
        slotTiming_[curSlot].frameId = frameCounter_;
        slotTiming_[curSlot].presentUs = rtNowUs();
        RT_LOG(currentSeqId_, "P", "frame=%u present done", frameCounter_);
#endif

#ifdef VBOXGPU_VERBOSE
        fprintf(stderr, "[Decoder] Present FLUSH: sc=%p imgIdx=%u result=%d\n",
                (void*)it->second.swapchain, presentIdx, (int)vr);
        fflush(stderr);
#endif

        lastPresentedImageIndex_ = presentIdx;
        // Auto-acquire next image for the next frame
        uint32_t oldIdx = it->second.currentImageIndex;
        vkAcquireNextImageKHR(device_, it->second.swapchain, UINT64_MAX,
                              VK_NULL_HANDLE, acquireFence_, &it->second.currentImageIndex);
        vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &acquireFence_);
#ifdef VBOXGPU_VERBOSE
        fprintf(stderr, "[Decoder] AutoAcquire: %u -> %u\n",
                oldIdx, it->second.currentImageIndex);
#endif

        // Wait for the PREVIOUS slot's readback fence.
        // By now present + acquire have completed, giving the GPU extra time to
        // finish the copy from the frame before last — minimising stall.
        rbReady_ = -1;
        if (!noReadback_ && readbackSubmitted_[prevSlot]) {
            vkWaitForFences(device_, 1, &readbackFences_[prevSlot], VK_TRUE, UINT64_MAX);
            readbackSubmitted_[prevSlot] = false;
            rbReady_ = prevSlot;
#if VBOXGPU_TIMING
            slotTiming_[prevSlot].readbackUs = rtNowUs();
            readyFrameTiming_ = slotTiming_[prevSlot];
            RT_LOG(currentSeqId_, "R", "frame=%u readback ready, age=%.2fms",
                   readyFrameTiming_.frameId,
                   (readyFrameTiming_.readbackUs - readyFrameTiming_.presentUs) / 1000.0);
#endif
        }

        // Flip write slot for next frame
        rbCur_ = 1 - rbCur_;
    }
    pendingPresents_.clear();
}

void VnDecoder::flushPendingDestroys() {
    if (pendingDestroys_.empty()) return;
    vkDeviceWaitIdle(device_);
    for (auto& fn : pendingDestroys_) fn();
    pendingDestroys_.clear();
}

HostSwapchain* VnDecoder::getSwapchain(uint64_t id) {
    auto it = swapchains_.find(id);
    return (it != swapchains_.end()) ? &it->second : nullptr;
}

void VnDecoder::cleanup() {
    fprintf(stderr, "[Decoder] cleanup() starting\n"); fflush(stderr);
    vkDeviceWaitIdle(device_);
    // Cleanup per-CB fence pool
    for (auto& [id, f] : cbLastFence_) if (f) vkDestroyFence(device_, f, nullptr);
    cbLastFence_.clear();
    for (auto f : fencePool_) vkDestroyFence(device_, f, nullptr);
    fencePool_.clear();
    // Cleanup double-buffered frame readback resources
    for (int s = 0; s < 2; s++) {
        FrameReadback& rb = readback_[s];
        if (rb.mappedPtr) { vkUnmapMemory(device_, rb.stagingMem); rb.mappedPtr = nullptr; }
        if (rb.cmdBuf) vkFreeCommandBuffers(device_, rb.cmdPool, 1, &rb.cmdBuf);
        if (rb.cmdPool) vkDestroyCommandPool(device_, rb.cmdPool, nullptr);
        if (rb.stagingBuf) vkDestroyBuffer(device_, rb.stagingBuf, nullptr);
        if (rb.stagingMem) vkFreeMemory(device_, rb.stagingMem, nullptr);
        rb = {};
        if (readbackFences_[s]) vkDestroyFence(device_, readbackFences_[s], nullptr);
    }
    // Cleanup copy staging buffer and all retired staging buffers.
    // Retired buffers may still be referenced by CBs that span multiple batches;
    // we hold them alive here and free only after vkDeviceWaitIdle (above).
    auto freeStagingBuf = [this](CopyStagingBuf& b) {
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = {};
    };
    freeStagingBuf(copyStagingBuf_);
    for (auto& b : retiredStagingBufs_) freeStagingBuf(b);
    retiredStagingBufs_.clear();

    if (acquireSemaphore_) vkDestroySemaphore(device_, acquireSemaphore_, nullptr);
    if (acquireFence_) vkDestroyFence(device_, acquireFence_, nullptr);
    for (auto& [id, p] : pipelines_) vkDestroyPipeline(device_, p, nullptr);
    for (auto& [id, l] : pipelineLayouts_) vkDestroyPipelineLayout(device_, l, nullptr);
    for (auto& [id, dsl] : descriptorSetLayouts_) vkDestroyDescriptorSetLayout(device_, dsl, nullptr);
    for (auto& [id, pool] : descriptorPools_) vkDestroyDescriptorPool(device_, pool, nullptr);
    for (auto& [id, s] : samplers_) vkDestroySampler(device_, s, nullptr);
    // Collect swapchain image view handles to avoid double-destroy
    std::unordered_set<VkImageView> scViews;
    for (auto& [id, sc] : swapchains_)
        for (auto iv : sc.imageViews) scViews.insert(iv);
    for (auto& [id, iv] : imageViews_)
        if (scViews.find(iv) == scViews.end()) vkDestroyImageView(device_, iv, nullptr);
    for (auto& [id, img] : images_) vkDestroyImage(device_, img, nullptr);
    for (auto& [id, mem] : deviceMemories_) vkFreeMemory(device_, mem, nullptr);
    for (auto& [id, m] : shaderModules_) vkDestroyShaderModule(device_, m, nullptr);
    for (auto& [id, fb] : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto& [id, rp] : renderPasses_) vkDestroyRenderPass(device_, rp, nullptr);
    for (auto& [id, pool] : commandPools_) vkDestroyCommandPool(device_, pool, nullptr);
    for (auto& [id, sem] : semaphores_) vkDestroySemaphore(device_, sem, nullptr);
    for (auto& [id, f] : fences_) vkDestroyFence(device_, f, nullptr);
    for (auto& [id, sc] : swapchains_) {
        for (auto iv : sc.imageViews) vkDestroyImageView(device_, iv, nullptr);
        vkDestroySwapchainKHR(device_, sc.swapchain, nullptr);
    }
}

void VnDecoder::debugCaptureImage(VkImage img, VkFormat fmt, uint32_t w, uint32_t h,
                                   VkImageLayout currentLayout, const char* path) {
    if (!img || commandPools_.empty()) return;

    // Compute bytes per pixel from format for correct buffer sizing
    uint32_t bytesPerPixel = 4; // default R8G8B8A8
    if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT) bytesPerPixel = 8;
    else if (fmt == VK_FORMAT_R32G32B32A32_SFLOAT) bytesPerPixel = 16;
    else if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) bytesPerPixel = 4;
    VkDeviceSize bufSize = VkDeviceSize(w) * h * bytesPerPixel;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &stagingBuf) != VK_SUCCESS) return;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, stagingBuf, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memType = i; break;
        }
    }
    if (memType == UINT32_MAX) { vkDestroyBuffer(device_, stagingBuf, nullptr); return; }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, stagingBuf, nullptr); return;
    }
    vkBindBufferMemory(device_, stagingBuf, stagingMem, 0);

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = commandPools_.begin()->second;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &cbAlloc, &cb);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    VkImageMemoryBarrier imgBarrier{};
    imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imgBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_SHADER_WRITE_BIT;
    imgBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imgBarrier.oldLayout = currentLayout;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imgBarrier.image = img;
    imgBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imgBarrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &copy);

    // Restore image to original layout so subsequent barriers remain valid
    if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier restoreBarrier{};
        restoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restoreBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        restoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restoreBarrier.newLayout = currentLayout;
        restoreBarrier.image = img;
        restoreBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &restoreBarrier);
    }

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);
    vkFreeCommandBuffers(device_, cbAlloc.commandPool, 1, &cb);

    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, bufSize, 0, &mapped);
    if (mapped) {
        auto* px = static_cast<uint8_t*>(mapped);
        uint32_t stride = bytesPerPixel;
        uint32_t cOff = (h/2 * w + w/2) * stride;
        fprintf(stderr, "[DebugCap] %s bpp=%u: px0=(%u,%u,%u,%u) center=(%u,%u,%u,%u)\n",
                path, bytesPerPixel,
                px[0], px[1], px[2], px[3],
                px[cOff], px[cOff+1], px[cOff+2], px[cOff+3]);

        uint32_t rowStride = (w * 4 + 3u) & ~3u;
        uint32_t pixelDataSize = rowStride * h;
        uint32_t fileSize = 14 + 40 + pixelDataSize;
        FILE* f = fopen(path, "wb");
        if (f) {
            uint8_t fh[14] = {};
            fh[0]='B'; fh[1]='M';
            *(uint32_t*)(fh+2) = fileSize;
            *(uint32_t*)(fh+10) = 54;
            fwrite(fh, 1, 14, f);
            uint8_t dh[40] = {};
            *(uint32_t*)(dh+0) = 40;
            *(int32_t*)(dh+4) = w;
            *(int32_t*)(dh+8) = -(int32_t)h;
            *(uint16_t*)(dh+12) = 1;
            *(uint16_t*)(dh+14) = 32;
            *(uint32_t*)(dh+20) = pixelDataSize;
            fwrite(dh, 1, 40, f);
            uint8_t pad[4] = {};
            uint32_t padSize = rowStride - w*4;
            for (uint32_t y = 0; y < h; y++) {
                fwrite(px + y*w*4, 1, w*4, f);
                if (padSize) fwrite(pad, 1, padSize, f);
            }
            fclose(f);
        } else {
            fprintf(stderr, "[DebugCap] Failed to open %s for writing\n", path);
        }
        vkUnmapMemory(device_, stagingMem);
    } else {
        fprintf(stderr, "[DebugCap] vkMapMemory failed for %s\n", path);
    }

    vkDestroyBuffer(device_, stagingBuf, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

bool VnDecoder::captureScreenshot(const char* path) {
    HostSwapchain* sc = getFirstSwapchain();
    if (!sc || sc->images.empty()) return false;

    uint32_t w = sc->extent.width;
    uint32_t h = sc->extent.height;

    // Prefer the readback buffer: it was captured BEFORE vkQueuePresentKHR, so the
    // contents are well-defined.  After present, the Vulkan spec says swapchain image
    // contents are undefined, which is why direct swapchain copies return black pixels.
    if (rbReady_ >= 0 && readback_[rbReady_].mappedPtr &&
        readback_[rbReady_].width == w && readback_[rbReady_].height == h) {
        fprintf(stderr, "[Screenshot] Using readback slot %d (frame %u, %ux%u)\n",
                rbReady_, readyFrameTiming_.frameId, w, h);
        const uint8_t* src = static_cast<const uint8_t*>(readback_[rbReady_].mappedPtr);
        // Invalidate host cache so CPU sees GPU writes
        VkMappedMemoryRange inv{};
        inv.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        inv.memory = readback_[rbReady_].stagingMem;
        inv.offset = 0;
        inv.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device_, 1, &inv);

        // Inspect a few pixels
        auto px = [&](uint32_t x, uint32_t y) {
            const uint8_t* p = src + (y * w + x) * 4;
            fprintf(stderr, "[Screenshot] Pixel(%u,%u) BGRA=(%u,%u,%u,%u)\n",
                    x, y, p[0], p[1], p[2], p[3]);
        };
        px(w/2, h/2); px(0, 0); px(w/4, h/4);

        uint32_t rowStride = (w * 4 + 3u) & ~3u;
        uint32_t pixelDataSize = rowStride * h;
        uint32_t fileSize = 14 + 40 + pixelDataSize;
        FILE* f = fopen(path, "wb");
        if (!f) return false;
        uint8_t fh[14] = {};
        fh[0] = 'B'; fh[1] = 'M';
        *(uint32_t*)(fh + 2) = fileSize;
        *(uint32_t*)(fh + 10) = 14 + 40;
        fwrite(fh, 1, 14, f);
        uint8_t ih[40] = {};
        *(uint32_t*)(ih + 0) = 40; *(int32_t*)(ih + 4) = (int32_t)w;
        *(int32_t*)(ih + 8) = -(int32_t)h;  // negative = top-down
        *(uint16_t*)(ih + 12) = 1; *(uint16_t*)(ih + 14) = 32;
        *(uint32_t*)(ih + 16) = 0; *(uint32_t*)(ih + 20) = pixelDataSize;
        fwrite(ih, 1, 40, f);
        std::vector<uint8_t> row(rowStride, 0);
        for (uint32_t y = 0; y < h; y++) {
            memcpy(row.data(), src + y * w * 4, w * 4);
            fwrite(row.data(), 1, rowStride, f);
        }
        fclose(f);
        fprintf(stderr, "[Screenshot] Saved %s (%ux%u) from readback\n", path, w, h);
        return true;
    }

    // Fallback: copy directly from swapchain image.
    // NOTE: After vkQueuePresentKHR the image contents are undefined per Vulkan spec;
    // this path may return black on some drivers (see readback path above).
    uint32_t imgIdx = lastPresentedImageIndex_ < sc->images.size() ? lastPresentedImageIndex_ : 0;
    VkImage srcImage = sc->images[imgIdx];
    fprintf(stderr, "[Screenshot] Capturing swapchain image %u (current=%u, lastPresented=%u)\n",
            imgIdx, sc->currentImageIndex, lastPresentedImageIndex_);

    // Create host-visible buffer for the copy
    VkDeviceSize bufferSize = VkDeviceSize(w) * h * 4; // BGRA8
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bufferSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, stagingBuf, &memReq);

    // Find HOST_VISIBLE memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, stagingBuf, stagingMem, 0);

    // Use a one-shot command buffer to copy
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = commandPools_.begin()->second;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &cbAlloc, &cb);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    // Transition image to TRANSFER_SRC
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = srcImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy image to buffer
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &copy);

    // Transition back to PRESENT_SRC
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cb);

    // Wait for the graphics queue to be idle before submitting the copy CB.
    // Same-queue ordering ensures render finishes before copy starts.
    VkResult waitIdleRes = vkQueueWaitIdle(graphicsQueue_);
    if (waitIdleRes != VK_SUCCESS)
        fprintf(stderr, "[Screenshot] vkQueueWaitIdle returned %d (device lost?)\n", (int)waitIdleRes);

    VkFence copyFence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fci, nullptr, &copyFence);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VkResult submitRes = vkQueueSubmit(graphicsQueue_, 1, &submit, copyFence);
    VkResult waitRes = vkWaitForFences(device_, 1, &copyFence, VK_TRUE, 5000000000ull); // 5s timeout
    fprintf(stderr, "[Screenshot] copy submit=%d waitFence=%d\n", (int)submitRes, (int)waitRes);
    vkDestroyFence(device_, copyFence, nullptr);

    vkFreeCommandBuffers(device_, cbAlloc.commandPool, 1, &cb);

    // Map first, then invalidate cache (required for non-HOST_COHERENT memory)
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, bufferSize, 0, &data);
    if (!data) {
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return false;
    }
    VkMappedMemoryRange flushRange{};
    flushRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    flushRange.memory = stagingMem;
    flushRange.offset = 0;
    flushRange.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device_, 1, &flushRange);

    // BMP: row padding to 4-byte boundary
    uint32_t rowStride = (w * 4 + 3u) & ~3u;
    uint32_t pixelDataSize = rowStride * h;
    uint32_t fileSize = 14 + 40 + pixelDataSize;

    FILE* f = fopen(path, "wb");
    if (!f) {
        vkUnmapMemory(device_, stagingMem);
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return false;
    }

    // BMP file header (14 bytes)
    uint8_t fh[14] = {};
    fh[0] = 'B'; fh[1] = 'M';
    *(uint32_t*)(fh + 2) = fileSize;
    *(uint32_t*)(fh + 10) = 14 + 40; // pixel data offset
    fwrite(fh, 1, 14, f);

    // DIB header (40 bytes, BITMAPINFOHEADER)
    uint8_t dh[40] = {};
    *(uint32_t*)(dh + 0) = 40;
    *(int32_t*)(dh + 4) = w;
    *(int32_t*)(dh + 8) = -(int32_t)h; // negative = top-down
    *(uint16_t*)(dh + 12) = 1;  // planes
    *(uint16_t*)(dh + 14) = 32; // bits per pixel (BGRA)
    *(uint32_t*)(dh + 20) = pixelDataSize;
    fwrite(dh, 1, 40, f);

    // Write pixel rows (already BGRA from Vulkan B8G8R8A8)
    auto* pixels = static_cast<uint8_t*>(data);
    uint8_t pad[4] = {};
    uint32_t padSize = rowStride - w * 4;
    for (uint32_t y = 0; y < h; y++) {
        fwrite(pixels + y * w * 4, 1, w * 4, f);
        if (padSize) fwrite(pad, 1, padSize, f);
    }

    fclose(f);
    vkUnmapMemory(device_, stagingMem);
    vkDestroyBuffer(device_, stagingBuf, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    // Log center pixel for quick black-screen diagnosis
    {
        auto* px = static_cast<uint8_t*>(data);
        uint32_t cx = w / 2, cy = h / 2;
        uint32_t off = (cy * w + cx) * 4;
        fprintf(stderr, "[Screenshot] Center pixel BGRA=(%u,%u,%u,%u)\n",
                px[off], px[off+1], px[off+2], px[off+3]);
        // Also log a few pixels from top-left and middle
        fprintf(stderr, "[Screenshot] TopLeft BGRA=(%u,%u,%u,%u)  Mid1 BGRA=(%u,%u,%u,%u)\n",
                px[0], px[1], px[2], px[3],
                px[(cy/2 * w + w/2) * 4], px[(cy/2 * w + w/2) * 4 + 1],
                px[(cy/2 * w + w/2) * 4 + 2], px[(cy/2 * w + w/2) * 4 + 3]);
    }
    fprintf(stderr, "[Screenshot] Saved %s (%ux%u)\n", path, w, h);

    // DEBUG: capture internal RTs to diagnose black swapchain output.
    // img=261 is the final LDR RT before the swapchain blit; img=265 is upstream.
    // If 261 is non-black but swapchain is black → blit broken.
    // If 261 is black too → rendering chain broken upstream.
    {
        static int dbgCap = 0;
        if (dbgCap < 3) {
            auto captureRT = [&](uint64_t imgId, VkFormat fmt, uint32_t rw, uint32_t rh, const char* tag) {
                auto it = images_.find(imgId);
                if (it == images_.end()) return;
                char p[256];
                snprintf(p, sizeof(p), "S:/bld/vboxgpu/dbg_%s_%d.bmp", tag, dbgCap);
                auto li = imageLayouts_.find(imgId);
                VkImageLayout lay = (li != imageLayouts_.end()) ? li->second : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                fprintf(stderr, "[DbgCap] img=%llu trackedLayout=%d -> %s\n",
                        (unsigned long long)imgId, (int)lay, p);
                debugCaptureImage(it->second, fmt, rw, rh, lay, p);
            };
            // uk_livetest.bin RTs (800x600 recording):
            // img=271: HDR color RT (fmt=91/R16G16B16A16_SFLOAT, 800x600)
            // img=268: LDR color RT (fmt=37/R8G8B8A8_UNORM, 800x600)
            // img=259: another LDR RT (fmt=37, 800x600)
            captureRT(271, VK_FORMAT_R16G16B16A16_SFLOAT, 800, 600, "rt271_hdr");
            captureRT(268, VK_FORMAT_R8G8B8A8_UNORM, 800, 600, "rt268_ldr");
            captureRT(259, VK_FORMAT_R8G8B8A8_UNORM, 800, 600, "rt259_ldr");
            dbgCap++;
        }
    }

    return true;
}

// --- Frame readback for TCP return ---

void VnDecoder::ensureReadbackResources(int slot, uint32_t w, uint32_t h) {
    FrameReadback& rb = readback_[slot];
    VkDeviceSize needed = VkDeviceSize(w) * h * 4; // BGRA8
    if (rb.stagingBuf && rb.bufferSize >= needed) {
        rb.width = w;
        rb.height = h;
        return;
    }

    // Cleanup old resources
    if (rb.mappedPtr) {
        vkUnmapMemory(device_, rb.stagingMem);
        rb.mappedPtr = nullptr;
    }
    if (rb.cmdBuf) {
        vkFreeCommandBuffers(device_, rb.cmdPool, 1, &rb.cmdBuf);
        rb.cmdBuf = VK_NULL_HANDLE;
    }
    if (rb.cmdPool) {
        vkDestroyCommandPool(device_, rb.cmdPool, nullptr);
        rb.cmdPool = VK_NULL_HANDLE;
    }
    if (rb.stagingBuf) {
        vkDestroyBuffer(device_, rb.stagingBuf, nullptr);
        rb.stagingBuf = VK_NULL_HANDLE;
    }
    if (rb.stagingMem) {
        vkFreeMemory(device_, rb.stagingMem, nullptr);
        rb.stagingMem = VK_NULL_HANDLE;
    }

    // Create dedicated command pool + buffer (resettable)
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &rb.cmdPool) != VK_SUCCESS) {
        fprintf(stderr, "[Readback] slot%d: Failed to create command pool\n", slot);
        return;
    }

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = rb.cmdPool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbAlloc, &rb.cmdBuf);

    // Create staging buffer
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = needed;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &rb.stagingBuf) != VK_SUCCESS) {
        fprintf(stderr, "[Readback] slot%d: Failed to create staging buffer\n", slot);
        return;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, rb.stagingBuf, &memReq);

    // Prefer HOST_VISIBLE + HOST_CACHED for fast CPU reads
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                memType = i;
                break;
            }
        }
    }
    if (memType == UINT32_MAX) {
        fprintf(stderr, "[Readback] slot%d: No suitable memory type\n", slot);
        vkDestroyBuffer(device_, rb.stagingBuf, nullptr);
        rb.stagingBuf = VK_NULL_HANDLE;
        return;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &rb.stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, rb.stagingBuf, nullptr);
        rb.stagingBuf = VK_NULL_HANDLE;
        return;
    }
    vkBindBufferMemory(device_, rb.stagingBuf, rb.stagingMem, 0);

    // Persistently map for zero-copy reads
    vkMapMemory(device_, rb.stagingMem, 0, needed, 0, &rb.mappedPtr);

    rb.width = w;
    rb.height = h;
    rb.bufferSize = needed;
    fprintf(stderr, "[Readback] slot%d: Allocated %ux%u staging buffer (%llu bytes)\n",
            slot, w, h, (unsigned long long)needed);
}

// Submit an async readback copy of the swapchain image into readback_[slot].
// Does NOT wait for the GPU — caller waits via readbackFences_[slot] next frame.
bool VnDecoder::readbackFrameAsync(uint32_t imageIndex, HostSwapchain& sc, int slot) {
    ensureReadbackResources(slot, sc.extent.width, sc.extent.height);
    FrameReadback& rb = readback_[slot];
    if (!rb.stagingBuf || !rb.mappedPtr) return false;

    VkImage srcImage = sc.images[imageIndex];

    vkResetCommandBuffer(rb.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(rb.cmdBuf, &beginInfo);

    // Transition swapchain image: PRESENT_SRC → TRANSFER_SRC
    // srcStage = ALL_COMMANDS ensures render work on same queue is complete first.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = srcImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(rb.cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy image to staging buffer
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {sc.extent.width, sc.extent.height, 1};
    vkCmdCopyImageToBuffer(rb.cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb.stagingBuf, 1, &copy);

    // Transition back: TRANSFER_SRC → PRESENT_SRC
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(rb.cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(rb.cmdBuf);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rb.cmdBuf;

    // Reset fence then submit — caller waits on readbackFences_[slot] next frame.
    vkResetFences(device_, 1, &readbackFences_[slot]);
    vkQueueSubmit(graphicsQueue_, 1, &submit, readbackFences_[slot]);
    // No vkWaitForFences here — that's the whole point of double-buffering.
    return true;
}

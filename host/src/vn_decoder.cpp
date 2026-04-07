#include "vn_decoder.h"
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
}

bool VnDecoder::execute(const uint8_t* data, size_t size) {
    pendingPresents_.clear();
    VnStreamReader reader(data, size);
    while (reader.hasMore() && !error_) {
        // Record position before reading header
        const uint8_t* cmdStart = reader.currentPtr();

        uint32_t cmdType = reader.readU32();
        uint32_t cmdSize = reader.readU32();

        if (cmdType == VN_CMD_BRIDGE_EndOfStream)
            break;

        dispatch(cmdType, reader, cmdSize);

        // Ensure reader is at the correct position for the next command.
        // Only adjust if handler read LESS than expected (e.g. unknown command skipped).
        // If handler read exactly right, reader is already positioned correctly.
        size_t cmdStartOff = cmdStart - data;
        size_t nextOff = cmdStartOff + cmdSize;
        size_t currentOff = reader.currentPtr() - data;
        if (currentOff != nextOff && nextOff <= size) {
            reader.setPos(nextOff);
        }
    }
    // All QueueSubmits in this batch have been executed.
    // Now flush deferred Presents so the GPU work is done before presenting.
    flushPendingPresents();
    return !error_;
}

bool VnDecoder::executeOneFrame(VnStreamReader& reader) {
    while (reader.hasMore() && !error_) {
        uint32_t cmdType = reader.readU32();
        uint32_t cmdSize = reader.readU32();

        if (cmdType == VN_CMD_BRIDGE_EndOfStream)
            return false;

        dispatch(cmdType, reader, cmdSize);

        if (cmdType == VN_CMD_BRIDGE_QueuePresent)
            return true; // frame boundary
    }
    return false;
}

void VnDecoder::dispatch(uint32_t cmdType, VnStreamReader& reader, uint32_t cmdSize) {
    fprintf(stderr, "[Decoder] cmd=%u size=%u\n", cmdType, cmdSize);
    switch (cmdType) {
    case VN_CMD_vkCreateRenderPass:       handleCreateRenderPass(reader); break;
    case VN_CMD_vkCreateShaderModule:     handleCreateShaderModule(reader); break;
    case VN_CMD_vkCreateDescriptorSetLayout: handleCreateDescriptorSetLayout(reader); break;
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
    case VN_CMD_vkCmdBindVertexBuffers:   handleCmdBindVertexBuffers(reader); break;
    case VN_CMD_vkCmdBindIndexBuffer:     handleCmdBindIndexBuffer(reader); break;
    case VN_CMD_vkCmdDrawIndexed:         handleCmdDrawIndexed(reader); break;
    case VN_CMD_vkCmdCopyBuffer:          handleCmdCopyBuffer(reader); break;
    case VN_CMD_vkCmdCopyBufferToImage:   handleCmdCopyBufferToImage(reader); break;
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

void VnDecoder::handleCreateDescriptorSetLayout(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t layoutId = r.readU64();
    uint32_t bindingCount = r.readU32();

    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        bindings[i] = {};
        bindings[i].binding = r.readU32();
        bindings[i].descriptorType = static_cast<VkDescriptorType>(r.readU32());
        bindings[i].descriptorCount = r.readU32();
        bindings[i].stageFlags = r.readU32();
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
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image;
    VkResult vr = vkCreateImage(device_, &ci, nullptr, &image);
    fprintf(stderr, "[Decoder] CreateImage: id=%llu %ux%u fmt=%u usage=0x%x result=%d\n",
            (unsigned long long)imageId, w, h, format, usage, (int)vr);
    if (vr != VK_SUCCESS) return;
    store(images_, imageId, image);
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
    uint64_t deviceId = r.readU64();
    uint64_t memoryId = r.readU64();
    uint64_t allocSize = r.readU64();
    uint32_t memTypeIdx = r.readU32();

    uint32_t hostType = VnDecoder_mapMemoryType(physDevice_, memTypeIdx);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = allocSize;
    ai.memoryTypeIndex = hostType;

    VkDeviceMemory mem;
    VkResult vr = vkAllocateMemory(device_, &ai, nullptr, &mem);
    fprintf(stderr, "[Decoder] AllocMemory: id=%llu size=%llu icdType=%u→hostType=%u result=%d\n",
            (unsigned long long)memoryId, (unsigned long long)allocSize, memTypeIdx, hostType, (int)vr);
    if (vr != VK_SUCCESS) return;
    store(deviceMemories_, memoryId, mem);
}

void VnDecoder::handleBindImageMemory(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t imageId = r.readU64();
    uint64_t memoryId = r.readU64();
    uint64_t offset = r.readU64();

    VkImage img = lookup(images_, imageId);
    VkDeviceMemory mem = lookup(deviceMemories_, memoryId);
    if (!img || !mem) return;
    vkBindImageMemory(device_, img, mem, offset);
}

void VnDecoder::handleCreateImageView(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t viewId = r.readU64();
    uint64_t imageId = r.readU64();
    uint32_t viewType = r.readU32(), format = r.readU32();
    uint32_t cR = r.readU32(), cG = r.readU32(), cB = r.readU32(), cA = r.readU32();
    uint32_t aspect = r.readU32(), baseMip = r.readU32(), levels = r.readU32();
    uint32_t baseLayer = r.readU32(), layers = r.readU32();

    VkImage img = lookup(images_, imageId);
    if (!img) {
        fprintf(stderr, "[Decoder] CreateImageView: SKIP id=%llu img=%llu NOT FOUND\n",
                (unsigned long long)viewId, (unsigned long long)imageId);
        return;
    }

    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img;
    ci.viewType = static_cast<VkImageViewType>(viewType);
    ci.format = static_cast<VkFormat>(format);
    ci.components = {static_cast<VkComponentSwizzle>(cR), static_cast<VkComponentSwizzle>(cG),
                     static_cast<VkComponentSwizzle>(cB), static_cast<VkComponentSwizzle>(cA)};
    ci.subresourceRange = {aspect, baseMip, levels, baseLayer, layers};

    VkImageView view;
    VkResult vr = vkCreateImageView(device_, &ci, nullptr, &view);
    fprintf(stderr, "[Decoder] CreateImageView: id=%llu img=%llu fmt=%u type=%u result=%d\n",
            (unsigned long long)viewId, (unsigned long long)imageId, format, viewType, (int)vr);
    if (vr != VK_SUCCESS) return;
    store(imageViews_, viewId, view);
}

void VnDecoder::handleCreateSampler(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t samplerId = r.readU64();

    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = static_cast<VkFilter>(r.readU32());
    ci.minFilter = static_cast<VkFilter>(r.readU32());
    ci.mipmapMode = static_cast<VkSamplerMipmapMode>(r.readU32());
    ci.addressModeU = static_cast<VkSamplerAddressMode>(r.readU32());
    ci.addressModeV = static_cast<VkSamplerAddressMode>(r.readU32());
    ci.addressModeW = static_cast<VkSamplerAddressMode>(r.readU32());
    ci.mipLodBias = r.readF32();
    ci.anisotropyEnable = r.readU32();
    ci.maxAnisotropy = r.readF32();
    ci.compareEnable = r.readU32();
    ci.compareOp = static_cast<VkCompareOp>(r.readU32());
    ci.minLod = r.readF32();
    ci.maxLod = r.readF32();
    ci.borderColor = static_cast<VkBorderColor>(r.readU32());
    ci.unnormalizedCoordinates = r.readU32();

    VkSampler sampler;
    VkResult vr = vkCreateSampler(device_, &ci, nullptr, &sampler);
    if (vr != VK_SUCCESS) return;
    store(samplers_, samplerId, sampler);
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
            // Debug: log missing image views for SAMPLED_IMAGE descriptors
            static int descImgLog = 0;
            if (descImgLog < 5 && descType == 2 && ivId != 0 && !allImageInfos[i][j].imageView) {
                fprintf(stderr, "[Decoder] UpdateDescSets: SAMPLED_IMAGE ivId=%llu NOT FOUND!\n",
                        (unsigned long long)ivId);
                descImgLog++;
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
    uint64_t deviceId = r.readU64();
    uint64_t bufferId = r.readU64();
    uint64_t size = r.readU64();
    uint32_t usage = r.readU32();

    // Strip SHADER_DEVICE_ADDRESS — host uses descriptors, not BDA;
    // this flag may cause memory-type incompatibility with HOST_VISIBLE memory
    usage &= ~(uint32_t)VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    VkResult vr = vkCreateBuffer(device_, &ci, nullptr, &buffer);
    fprintf(stderr, "[Decoder] CreateBuffer: id=%llu size=%llu usage=0x%x result=%d\n",
            (unsigned long long)bufferId, (unsigned long long)size, usage, (int)vr);
    if (vr != VK_SUCCESS) return;
    store(buffers_, bufferId, buffer);
}

void VnDecoder::handleBindBufferMemory(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t bufferId = r.readU64();
    uint64_t memoryId = r.readU64();
    uint64_t offset = r.readU64();

    VkBuffer buf = lookup(buffers_, bufferId);
    VkDeviceMemory mem = lookup(deviceMemories_, memoryId);
    if (!buf || !mem) return;
    // Check memory requirements
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(device_, buf, &reqs);
    VkResult vr = vkBindBufferMemory(device_, buf, mem, offset);
    static int bindLog = 0;
    if (bindLog < 10) {
        fprintf(stderr, "[Decoder] BindBufferMemory: buf=%llu mem=%llu off=%llu result=%d "
                "reqs(size=%llu align=%llu typeBits=0x%x)\n",
                (unsigned long long)bufferId, (unsigned long long)memoryId,
                (unsigned long long)offset, (int)vr,
                (unsigned long long)reqs.size, (unsigned long long)reqs.alignment,
                reqs.memoryTypeBits);
        bindLog++;
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
        vkDestroyFn(device_, obj, nullptr); \
        mapName.erase(objId); \
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
        vkFreeMemory(device_, mem, nullptr);
        deviceMemories_.erase(memId);
    }
}

#undef IMPL_DESTROY

void VnDecoder::handleWriteMemory(VnStreamReader& r) {
    uint64_t memId = r.readU64();
    uint64_t offset = r.readU64();
    uint32_t size = r.readU32();

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
    vkUnmapMemory(device_, mem);
    fprintf(stderr, "[Decoder] WriteMemory: mem=%llu off=%llu size=%u\n", (unsigned long long)memId, (unsigned long long)offset, size);
}

void VnDecoder::handleCmdPipelineBarrier(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t srcStage = r.readU32(), dstStage = r.readU32();
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
        static int barrierLog = 0;
        if (barrierLog < 10) {
            fprintf(stderr, "[Decoder] Barrier: img=%llu old=%u new=%u\n",
                    (unsigned long long)imgId, oldLayout, newLayout);
            barrierLog++;
        }
        barriers[i].oldLayout = static_cast<VkImageLayout>(oldLayout);
        barriers[i].newLayout = static_cast<VkImageLayout>(newLayout);
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = lookup(images_, imgId);
        barriers[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

        // If image not found in our map, it might be a swapchain image — skip
        if (!barriers[i].image) {
            HostSwapchain* sc = getFirstSwapchain();
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

    fprintf(stderr, "[Decoder] CreatePipeline: id=%u rp=%u vert→%p frag→%p %ux%u dynRender=%d fmt=%u\n",
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

    fprintf(stderr, "[Decoder] CreatePipeline vtxInput: %zu bindings, %zu attrs\n",
            vtxBindings.size(), vtxAttrs.size());
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
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
    VkDynamicState dynStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
    };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 5;
    dynState.pDynamicStates = dynStates;

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

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAtt;

    // Dynamic rendering info (Vulkan 1.3)
    VkPipelineRenderingCreateInfo renderingInfo{};
    VkFormat colorFormat = static_cast<VkFormat>(colorFmt);
    HostSwapchain* scFmt = getFirstSwapchain();
    // Only override colorFormat for blit/present pipelines (0 vertex bindings)
    // that target the swapchain. Draw pipelines with vertex input target internal
    // images and must keep their original format.
    if (scFmt && dynamicRendering && vtxBindings.empty()) colorFormat = scFmt->format;

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
    pInfo.pDynamicState = (dynamicRendering || dynState.dynamicStateCount > 0) ? &dynState : nullptr;
    pInfo.layout = lookup(pipelineLayouts_, layoutId);

    if (dynamicRendering) {
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;
        pInfo.pNext = &renderingInfo;
        pInfo.renderPass = VK_NULL_HANDLE;
    } else {
        pInfo.renderPass = lookup(renderPasses_, renderPassId);
        pInfo.subpass = 0;
    }

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
    uint64_t deviceId = r.readU64();
    uint64_t poolId = r.readU64();
    uint32_t queueFamily = r.readU32();
    fprintf(stderr, "[Decoder] CreateCmdPool: id=%llu family=%u\n",
            (unsigned long long)poolId, queueFamily);

    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamily;

    VkCommandPool pool;
    VkResult vr = vkCreateCommandPool(device_, &info, nullptr, &pool);
    fprintf(stderr, "[Decoder] CreateCmdPool result=%d pool=%p\n", (int)vr, (void*)pool);
    fflush(stderr);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Decoder] CreateCmdPool FAILED\n");
        error_ = true;
        return;
    }
    store(commandPools_, poolId, pool);
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
    fprintf(stderr, "[Decoder] AllocCmdBuf: dev=%llu pool=%llu→%p cb=%llu\n",
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
    fprintf(stderr, "[Decoder] BeginCmdBuf: cbId=0x%llx → cb=%p (map size=%zu)\n",
            (unsigned long long)cbId, (void*)cb, commandBuffers_.size());
    fflush(stderr);
    if (!cb) { error_ = true; return; }

    // Must wait for GPU to finish any prior submission of this CB before resetting.
    // Within a single batch, the same CB can be Submit'd then Begin'd again.
    vkDeviceWaitIdle(device_);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &info);
}

void VnDecoder::handleEndCommandBuffer(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkResult vr = vkEndCommandBuffer(cb);
    fprintf(stderr, "[Decoder] EndCmdBuf: cbId=0x%llx cb=%p result=%d\n",
            (unsigned long long)cbId, (void*)cb, (int)vr);
    fflush(stderr);
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

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;

    HostSwapchain* sc = nullptr;
    for (auto& [id, s] : swapchains_) { sc = &s; break; }

    VkImageView targetView = VK_NULL_HANDLE;
    VkImage targetImage = VK_NULL_HANDLE;
    bool isSwapchain = false;

    if (imageViewId != 0) {
        targetView = lookup(imageViews_, imageViewId);
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

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = targetView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = static_cast<VkAttachmentLoadOp>(loadOp);
    colorAttachment.storeOp = static_cast<VkAttachmentStoreOp>(storeOp);
    colorAttachment.clearValue.color = {{cr, cg, cb_, ca}};

    // Clamp render area
    uint32_t clampedW = areaW, clampedH = areaH;
    if (sc) {
        clampedW = std::min(areaW, sc->extent.width);
        clampedH = std::min(areaH, sc->extent.height);
    }

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{(int32_t)areaX, (int32_t)areaY}, {clampedW, clampedH}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

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

    fprintf(stderr, "[Decoder] BeginRendering: cb=%p view=%p (ivId=%llu %s) area=%u,%u,%ux%u load=%u\n",
            (void*)cb, (void*)targetView, (unsigned long long)imageViewId,
            isSwapchain ? "swapchain" : "internal", areaX, areaY, clampedW, clampedH, loadOp);
    fflush(stderr);
    vkCmdBeginRendering(cb, &renderingInfo);

    // Set viewport/scissor
    VkViewport vp{0.0f, 0.0f, (float)clampedW, (float)clampedH, 0.0f, 1.0f};
    VkRect2D sc2{{0, 0}, {clampedW, clampedH}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc2);
}

void VnDecoder::handleCmdEndRendering(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb || !activeRendering_) {
        fprintf(stderr, "[Decoder] EndRendering SKIP: cbId=0x%llx cb=%p active=%d\n",
                (unsigned long long)cbId, (void*)cb, (int)activeRendering_);
        return;
    }
    activeRendering_ = false;
    fprintf(stderr, "[Decoder] EndRendering: cbId=0x%llx cb=%p imgIdx=%u\n",
            (unsigned long long)cbId, (void*)cb,
            getFirstSwapchain() ? getFirstSwapchain()->currentImageIndex : 999);
    fflush(stderr);
    vkCmdEndRendering(cb);

    // Only transition to PRESENT_SRC if this was a swapchain render pass
    if (activeRenderingIsSwapchain_) {
        HostSwapchain* sc = nullptr;
        for (auto& [id, s] : swapchains_) { sc = &s; break; }
        if (sc) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = sc->images[sc->currentImageIndex];
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = 0;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }
    // Non-swapchain images: DXVK manages layout transitions via pipeline barriers
}

void VnDecoder::handleCmdBindPipeline(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t bindPoint = r.readU32();
    uint64_t pipId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipeline pip = lookup(pipelines_, pipId);
    if (!cb || !pip) {
        fprintf(stderr, "[Decoder] BindPipeline SKIP: cb=%p pip=%p (ids: cb=%u pip=%u)\n",
                (void*)cb, (void*)pip, (unsigned)cbId, (unsigned)pipId);
        return;
    }
    vkCmdBindPipeline(cb, static_cast<VkPipelineBindPoint>(bindPoint), pip);

    // Re-set viewport/scissor after pipeline bind — some drivers reset dynamic state
    HostSwapchain* sc = getFirstSwapchain();
    if (sc) {
        VkViewport vp{0, 0, (float)sc->extent.width, (float)sc->extent.height, 0, 1};
        VkRect2D scissor{{0,0}, sc->extent};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);
    }
}

void VnDecoder::handleCmdSetViewport(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkViewport vp;
    vp.x = r.readF32(); vp.y = r.readF32();
    vp.width = r.readF32(); vp.height = r.readF32();
    vp.minDepth = r.readF32(); vp.maxDepth = r.readF32();
    fprintf(stderr, "[Decoder] SetViewport: x=%.1f y=%.1f w=%.1f h=%.1f\n", vp.x, vp.y, vp.width, vp.height);
    vkCmdSetViewport(lookup(commandBuffers_, cbId), 0, 1, &vp);
}

void VnDecoder::handleCmdSetScissor(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkRect2D sc;
    sc.offset.x = r.readI32(); sc.offset.y = r.readI32();
    sc.extent.width = r.readU32(); sc.extent.height = r.readU32();
    vkCmdSetScissor(lookup(commandBuffers_, cbId), 0, 1, &sc);
}

void VnDecoder::handleCmdSetCullMode(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t cullMode = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetCullMode(cb, static_cast<VkCullModeFlags>(cullMode));
}

void VnDecoder::handleCmdSetFrontFace(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint32_t frontFace = r.readU32();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (cb) vkCmdSetFrontFace(cb, static_cast<VkFrontFace>(frontFace));
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
    if (!cb || !activeRendering_) return;
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
    if (!cb || !src || !dst) return;
    vkCmdCopyBuffer(cb, src, dst, regionCount, regions.data());
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
    if (!cb || !srcBuf || !dstImg) return;
    vkCmdCopyBufferToImage(cb, srcBuf, dstImg, static_cast<VkImageLayout>(dstLayout),
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
    uint64_t deviceId = r.readU64();
    uint64_t semId = r.readU64();

    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore sem;
    if (vkCreateSemaphore(device_, &info, nullptr, &sem) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(semaphores_, semId, sem);
}

void VnDecoder::handleCreateFence(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t fenceId = r.readU64();
    uint32_t flags = r.readU32();

    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = flags;
    VkFence fence;
    if (vkCreateFence(device_, &info, nullptr, &fence) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(fences_, fenceId, fence);
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

    fprintf(stderr, "[Decoder] QueueSubmit: cb=%llu→%p waitSem=%llu sigSem=%llu fence=%llu\n",
            (unsigned long long)cbId, (void*)cb,
            (unsigned long long)waitSemId, (unsigned long long)signalSemId,
            (unsigned long long)fenceId);
    fflush(stderr);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (cb) {
        info.commandBufferCount = 1;
        info.pCommandBuffers = &cb;
    }
    if (waitSem) {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &waitSem;
        info.pWaitDstStageMask = &waitStage;
    }
    if (sigSem) {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &sigSem;
    }

    VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &info, fence);
    fprintf(stderr, "[Decoder] QueueSubmit result=%d cb=%p\n", (int)submitResult, (void*)cb);
    fflush(stderr);
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
    if (!fences.empty()) {
        // Cap timeout to avoid blocking the host main thread indefinitely.
        // The host decoder runs synchronously on the main thread which also
        // processes window messages — a long block causes "Not Responding".
        constexpr uint64_t MAX_TIMEOUT_NS = 100000000ULL; // 100ms
        uint64_t cappedTimeout = (timeout > MAX_TIMEOUT_NS) ? MAX_TIMEOUT_NS : timeout;
        vkWaitForFences(device_, (uint32_t)fences.size(), fences.data(),
                        waitAll ? VK_TRUE : VK_FALSE, cappedTimeout);
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
    vkAcquireNextImageKHR(device_, it->second.swapchain, UINT64_MAX,
                          sem, VK_NULL_HANDLE, &it->second.currentImageIndex);
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

void VnDecoder::flushPendingPresents() {
    for (auto& pp : pendingPresents_) {
        auto it = swapchains_.find(pp.scId);
        if (it == swapchains_.end()) continue;

        // Wait for all GPU work to complete before presenting.
        // Don't use ICD semaphores for present wait — they may not be properly
        // signaled (ICD semaphore IDs don't map 1:1 to Host signal operations).
        // vkDeviceWaitIdle guarantees all rendering is done.
        vkDeviceWaitIdle(device_);

        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.swapchainCount = 1;
        info.pSwapchains = &it->second.swapchain;
        info.pImageIndices = &it->second.currentImageIndex;

        uint32_t presentIdx = it->second.currentImageIndex;

        // Capture screenshots at specific frames for animation debugging
        static int dbgFr2 = 0;
        dbgFr2++;
        if (dbgFr2 == 5 || dbgFr2 == 150 || dbgFr2 == 300) {
            uint32_t savedLPI = lastPresentedImageIndex_;
            lastPresentedImageIndex_ = presentIdx;
            char path[256];
            snprintf(path, sizeof(path), "S:/bld/vboxgpu/dbg_frame%d.bmp", dbgFr2);
            captureScreenshot(path);
            lastPresentedImageIndex_ = savedLPI;
            fprintf(stderr, "[Decoder] Screenshot saved: %s (presentIdx=%u)\n", path, presentIdx);
        }
        VkResult vr = vkQueuePresentKHR(graphicsQueue_, &info);

        // WGC disabled — was causing crashes after push descriptor changes

        fprintf(stderr, "[Decoder] Present FLUSH: sc=%p imgIdx=%u result=%d\n",
                (void*)it->second.swapchain, presentIdx, (int)vr);
        fflush(stderr);

        lastPresentedImageIndex_ = presentIdx;
        // Auto-acquire next image for the next frame
        uint32_t oldIdx = it->second.currentImageIndex;
        vkAcquireNextImageKHR(device_, it->second.swapchain, UINT64_MAX,
                              VK_NULL_HANDLE, acquireFence_, &it->second.currentImageIndex);
        vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &acquireFence_);
        fprintf(stderr, "[Decoder] AutoAcquire: %u -> %u\n",
                oldIdx, it->second.currentImageIndex);
    }
    pendingPresents_.clear();
}

HostSwapchain* VnDecoder::getSwapchain(uint64_t id) {
    auto it = swapchains_.find(id);
    return (it != swapchains_.end()) ? &it->second : nullptr;
}

void VnDecoder::cleanup() {
    fprintf(stderr, "[Decoder] cleanup() starting\n"); fflush(stderr);
    vkDeviceWaitIdle(device_);
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

bool VnDecoder::captureScreenshot(const char* path) {
    HostSwapchain* sc = getFirstSwapchain();
    if (!sc || sc->images.empty()) return false;

    uint32_t w = sc->extent.width;
    uint32_t h = sc->extent.height;
    // Use the image that was last presented to the window (not auto-acquired)
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

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, cbAlloc.commandPool, 1, &cb);

    // Map and write BMP
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, bufferSize, 0, &data);
    if (!data) {
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return false;
    }

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

    fprintf(stderr, "[Screenshot] Saved %s (%ux%u)\n", path, w, h);
    return true;
}

#include "vn_decoder.h"
#include <fstream>
#include <algorithm>

// Built-in simple triangle shaders (GLSL 450, compiled with glslc)
// Vertex: hardcoded triangle positions via gl_VertexIndex
static const uint32_t builtinVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000028, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000019, 0x0000001d, 0x00030003,
    0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79,
    0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45,
    0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00050005, 0x0000000c, 0x69736f70, 0x6e6f6974, 0x00000073, 0x00060005, 0x00000017, 0x505f6c67,
    0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000017, 0x00000000, 0x505f6c67, 0x7469736f,
    0x006e6f69, 0x00070006, 0x00000017, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000,
    0x00070006, 0x00000017, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006,
    0x00000017, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x00000019,
    0x00000000, 0x00060005, 0x0000001d, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00050048,
    0x00000017, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x00000017, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x00000017, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x00000017,
    0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x00000017, 0x00000002, 0x00040047, 0x0000001d,
    0x0000000b, 0x0000002a, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x00000008,
    0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000003, 0x0004001c, 0x0000000a,
    0x00000007, 0x00000009, 0x00040020, 0x0000000b, 0x00000006, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000006, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x0004002b, 0x00000006,
    0x0000000e, 0xbf000000, 0x0005002c, 0x00000007, 0x0000000f, 0x0000000d, 0x0000000e, 0x0004002b,
    0x00000006, 0x00000010, 0x3f000000, 0x0005002c, 0x00000007, 0x00000011, 0x00000010, 0x00000010,
    0x0005002c, 0x00000007, 0x00000012, 0x0000000e, 0x00000010, 0x0006002c, 0x0000000a, 0x00000013,
    0x0000000f, 0x00000011, 0x00000012, 0x00040017, 0x00000014, 0x00000006, 0x00000004, 0x0004002b,
    0x00000008, 0x00000015, 0x00000001, 0x0004001c, 0x00000016, 0x00000006, 0x00000015, 0x0006001e,
    0x00000017, 0x00000014, 0x00000006, 0x00000016, 0x00000016, 0x00040020, 0x00000018, 0x00000003,
    0x00000017, 0x0004003b, 0x00000018, 0x00000019, 0x00000003, 0x00040015, 0x0000001a, 0x00000020,
    0x00000001, 0x0004002b, 0x0000001a, 0x0000001b, 0x00000000, 0x00040020, 0x0000001c, 0x00000001,
    0x0000001a, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000001, 0x00040020, 0x0000001f, 0x00000006,
    0x00000007, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x00040020, 0x00000026, 0x00000003,
    0x00000014, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0003003e, 0x0000000c, 0x00000013, 0x0004003d, 0x0000001a, 0x0000001e, 0x0000001d, 0x00050041,
    0x0000001f, 0x00000020, 0x0000000c, 0x0000001e, 0x0004003d, 0x00000007, 0x00000021, 0x00000020,
    0x00050051, 0x00000006, 0x00000023, 0x00000021, 0x00000000, 0x00050051, 0x00000006, 0x00000024,
    0x00000021, 0x00000001, 0x00070050, 0x00000014, 0x00000025, 0x00000023, 0x00000024, 0x0000000d,
    0x00000022, 0x00050041, 0x00000026, 0x00000027, 0x00000019, 0x0000001b, 0x0003003e, 0x00000027,
    0x00000025, 0x000100fd, 0x00010038,
};
// Fragment: constant orange output
static const uint32_t builtinFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000000e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45,
    0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004, 0x475f4c47,
    0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f, 0x00000000, 0x00040047,
    0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b,
    0x00000006, 0x0000000a, 0x3f800000, 0x0004002b, 0x00000006, 0x0000000b, 0x3f000000, 0x0004002b,
    0x00000006, 0x0000000c, 0x00000000, 0x0007002c, 0x00000007, 0x0000000d, 0x0000000a, 0x0000000b,
    0x0000000c, 0x0000000a, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0003003e, 0x00000009, 0x0000000d, 0x000100fd, 0x00010038,
};

void VnDecoder::init(VkPhysicalDevice physDevice, VkDevice device,
                     VkQueue graphicsQueue, uint32_t graphicsFamily,
                     VkSurfaceKHR surface) {
    physDevice_ = physDevice;
    device_ = device;
    graphicsQueue_ = graphicsQueue;
    graphicsFamily_ = graphicsFamily;
    surface_ = surface;
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
    case VN_CMD_vkCmdDraw:                handleCmdDraw(reader); break;
    case VN_CMD_vkCmdPushConstants:      handleCmdPushConstants(reader); break;
    case VN_CMD_vkCreateSemaphore:        handleCreateSemaphore(reader); break;
    case VN_CMD_vkCreateFence:            handleCreateFence(reader); break;
    case VN_CMD_vkQueueSubmit:            handleQueueSubmit(reader); break;
    case VN_CMD_vkWaitForFences:          handleWaitForFences(reader); break;
    case VN_CMD_vkResetFences:            handleResetFences(reader); break;
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

void VnDecoder::handleCreatePipelineLayout(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t layoutId = r.readU64();

    // DXVK shaders use push constants for internal state (output swizzle, alpha test, etc.)
    // and descriptor sets for samplers/textures. Create a layout that covers these.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    pushRange.offset = 0;
    pushRange.size = 256; // max push constant size

    // Create descriptor set layouts for DXVK's expected bindings:
    // Set 0: sampler(s), Set 1: sampled images
    VkDescriptorSetLayoutBinding bindings0[1]{};
    bindings0[0].binding = 0;
    bindings0[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings0[0].descriptorCount = 1;
    bindings0[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding bindings1[4]{};
    for (int i = 0; i < 4; i++) {
        bindings1[i].binding = i;
        bindings1[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings1[i].descriptorCount = 1;
        bindings1[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dsLayout0Info{};
    dsLayout0Info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayout0Info.bindingCount = 1;
    dsLayout0Info.pBindings = bindings0;

    VkDescriptorSetLayoutCreateInfo dsLayout1Info{};
    dsLayout1Info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayout1Info.bindingCount = 4;
    dsLayout1Info.pBindings = bindings1;

    VkDescriptorSetLayout dsLayouts[2];
    vkCreateDescriptorSetLayout(device_, &dsLayout0Info, nullptr, &dsLayouts[0]);
    vkCreateDescriptorSetLayout(device_, &dsLayout1Info, nullptr, &dsLayouts[1]);

    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &pushRange;
    info.setLayoutCount = 2;
    info.pSetLayouts = dsLayouts;

    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(device_, &info, nullptr, &layout) != VK_SUCCESS) {
        error_ = true;
        return;
    }
    store(pipelineLayouts_, layoutId, layout);
    // Note: descriptor set layouts leak — acceptable for MVP
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

    // DEBUG: replace DXVK shaders with builtin simple triangle shaders
    if (dynamicRendering) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(builtinVertSpv);
        smci.pCode = builtinVertSpv;
        VkShaderModule bv; vkCreateShaderModule(device_, &smci, nullptr, &bv);
        smci.codeSize = sizeof(builtinFragSpv);
        smci.pCode = builtinFragSpv;
        VkShaderModule bf; vkCreateShaderModule(device_, &smci, nullptr, &bf);
        vertMod = bv; fragMod = bf;
        fprintf(stderr, "[Decoder] CreatePipeline: USING BUILTIN SHADERS\n");
    }

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

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
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
    // For builtin shaders, use a simple empty pipeline layout
    VkPipelineLayout usedLayout = lookup(pipelineLayouts_, layoutId);
    if (dynamicRendering) {
        // Builtin shaders don't need push constants or descriptors
        static VkPipelineLayout emptyLayout = VK_NULL_HANDLE;
        if (!emptyLayout) {
            VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            vkCreatePipelineLayout(device_, &pli, nullptr, &emptyLayout);
        }
        usedLayout = emptyLayout;
    }
    pInfo.layout = usedLayout;

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

    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    if (!cb) return;

    // Use the current swapchain image view (set by AcquireNextImage)
    HostSwapchain* sc = nullptr;
    for (auto& [id, s] : swapchains_) { sc = &s; break; }
    if (!sc || sc->imageViews.empty()) {
        fprintf(stderr, "[Decoder] BeginRendering SKIP: no swapchain yet (cbId=%llu)\n",
                (unsigned long long)cbId);
        activeRendering_ = false;
        return;
    }
    activeRendering_ = true;

    VkImageView currentView = sc->imageViews[sc->currentImageIndex];

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = currentView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = static_cast<VkAttachmentLoadOp>(loadOp);
    colorAttachment.storeOp = static_cast<VkAttachmentStoreOp>(storeOp);
    colorAttachment.clearValue.color = {{cr, cg, cb_, ca}};

    // Clamp render area to actual swapchain extent (DXVK may send 16384x16384)
    uint32_t clampedW = std::min(areaW, sc->extent.width);
    uint32_t clampedH = std::min(areaH, sc->extent.height);

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{(int32_t)areaX, (int32_t)areaY}, {clampedW, clampedH}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    // Transition swapchain image to COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = sc->images[sc->currentImageIndex];
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    fprintf(stderr, "[Decoder] BeginRendering: cb=%p imgIdx=%u view=%p area=%u,%u,%ux%u (clamped %ux%u) load=%u clear=%.1f,%.1f,%.1f,%.1f\n",
            (void*)cb, sc->currentImageIndex, (void*)currentView, areaX, areaY, areaW, areaH, clampedW, clampedH, loadOp, cr, cg, cb_, ca);
    fflush(stderr);
    vkCmdBeginRendering(cb, &renderingInfo);

    // Set viewport/scissor to actual swapchain extent.
    // Pipeline uses dynamic viewport/scissor because DXVK sends garbage values.
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

    // Transition swapchain image to PRESENT_SRC
    HostSwapchain* sc = nullptr;
    for (auto& [id, s] : swapchains_) { sc = &s; break; }
    if (!sc) return;

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

void VnDecoder::handleCmdBindPipeline(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    uint64_t pipId = r.readU64();
    VkCommandBuffer cb = lookup(commandBuffers_, cbId);
    VkPipeline pip = lookup(pipelines_, pipId);
    if (!cb || !pip) {
        fprintf(stderr, "[Decoder] BindPipeline SKIP: cb=%p pip=%p (ids: cb=%u pip=%u)\n",
                (void*)cb, (void*)pip, (unsigned)cbId, (unsigned)pipId);
        return;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pip);

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
    vkCmdSetViewport(lookup(commandBuffers_, cbId), 0, 1, &vp);
}

void VnDecoder::handleCmdSetScissor(VnStreamReader& r) {
    uint64_t cbId = r.readU64();
    VkRect2D sc;
    sc.offset.x = r.readI32(); sc.offset.y = r.readI32();
    sc.extent.width = r.readU32(); sc.extent.height = r.readU32();
    vkCmdSetScissor(lookup(commandBuffers_, cbId), 0, 1, &sc);
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
    fprintf(stderr, "[Decoder] PushConstants: cb=%p stage=0x%x offset=%u size=%u\n",
            (void*)cb, stageFlags, offset, size);
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
    uint64_t fenceId = r.readU64();
    VkFence fence = lookup(fences_, fenceId);
    if (fence)
        vkWaitForFences(device_, 1, &fence, VK_TRUE, 100000000ULL); // 100ms timeout
}

void VnDecoder::handleResetFences(VnStreamReader& r) {
    uint64_t deviceId = r.readU64();
    uint64_t fenceId = r.readU64();
    VkFence fence = lookup(fences_, fenceId);
    if (fence)
        vkResetFences(device_, 1, &fence);
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

    VkSurfaceFormatKHR surfFmt = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) { surfFmt = f; break; }
    }

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
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
                          VK_NULL_HANDLE, VK_NULL_HANDLE, &sc.currentImageIndex);

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

        VkSemaphore waitSem = lookup(semaphores_, pp.waitSemId);

        // Wait for all GPU work to complete before presenting
        vkDeviceWaitIdle(device_);

        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        if (waitSem) {
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &waitSem;
        }
        info.swapchainCount = 1;
        info.pSwapchains = &it->second.swapchain;
        info.pImageIndices = &it->second.currentImageIndex;

        uint32_t presentIdx = it->second.currentImageIndex;

        // Debug: capture screenshot before present (no debug clear)
        static int dbgPresentCount = 0;
        dbgPresentCount++;
        if (dbgPresentCount <= 3) {
            captureScreenshot(
                (std::string("S:/bld/vboxgpu/dbg_pre_present") + std::to_string(dbgPresentCount) + ".bmp").c_str());
        }

        VkResult vr = vkQueuePresentKHR(graphicsQueue_, &info);
        fprintf(stderr, "[Decoder] Present FLUSH: sc=%p imgIdx=%u result=%d\n",
                (void*)it->second.swapchain, presentIdx, (int)vr);
        fflush(stderr);

        lastPresentedImageIndex_ = presentIdx;
#if 0 // Debug capture moved before present
        // Debug: capture every swapchain image after first few presents
        static int presentCount = 0;
        presentCount++;
        if (presentCount <= 3) {
            for (uint32_t imgI = 0; imgI < (uint32_t)it->second.images.size(); imgI++) {
                char path[256];
                snprintf(path, sizeof(path), "S:/bld/vboxgpu/dbg_present%d_img%u.bmp", presentCount, imgI);
                // Need to capture this specific image
                VkImage srcImg = it->second.images[imgI];
                fprintf(stderr, "[Debug] Capturing image %u after present %d\n", imgI, presentCount);
                // Quick inline capture
                {
                    uint32_t w = it->second.extent.width, h = it->second.extent.height;
                    VkDeviceSize bufSz = VkDeviceSize(w) * h * 4;
                    VkBuffer sBuf; VkDeviceMemory sMem;
                    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bi.size = bufSz; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    vkCreateBuffer(device_, &bi, nullptr, &sBuf);
                    VkMemoryRequirements mreq; vkGetBufferMemoryRequirements(device_, sBuf, &mreq);
                    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physDevice_, &mp);
                    uint32_t mt = 0;
                    for (uint32_t mi = 0; mi < mp.memoryTypeCount; mi++)
                        if ((mreq.memoryTypeBits & (1u<<mi)) && (mp.memoryTypes[mi].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt = mi; break; }
                    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    ai.allocationSize = mreq.size; ai.memoryTypeIndex = mt;
                    vkAllocateMemory(device_, &ai, nullptr, &sMem);
                    vkBindBufferMemory(device_, sBuf, sMem, 0);
                    VkCommandBuffer tcb;
                    VkCommandBufferAllocateInfo ca{}; ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    ca.commandPool = commandPools_.begin()->second; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
                    vkAllocateCommandBuffers(device_, &ca, &tcb);
                    VkCommandBufferBeginInfo cbi{}; cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(tcb, &cbi);
                    VkImageMemoryBarrier b1{}; b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b1.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    b1.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    b1.image = srcImg; b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,1,&b1);
                    VkBufferImageCopy cp{}; cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; cp.imageExtent = {w,h,1};
                    vkCmdCopyImageToBuffer(tcb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sBuf, 1, &cp);
                    vkEndCommandBuffer(tcb);
                    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &tcb;
                    vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
                    vkQueueWaitIdle(graphicsQueue_);
                    vkFreeCommandBuffers(device_, ca.commandPool, 1, &tcb);
                    void* data = nullptr; vkMapMemory(device_, sMem, 0, bufSz, 0, &data);
                    if (data) {
                        // Check first few pixels
                        auto* px = (uint8_t*)data;
                        uint32_t nonzero = 0;
                        for (uint32_t pi = 0; pi < w*h*4; pi++) if (px[pi]) nonzero++;
                        fprintf(stderr, "[Debug] img%u: nonzero bytes=%u/%u px[0]=%u,%u,%u,%u center=%u,%u,%u,%u\n",
                            imgI, nonzero, w*h*4,
                            px[0],px[1],px[2],px[3],
                            px[(h/2*w+w/2)*4],px[(h/2*w+w/2)*4+1],px[(h/2*w+w/2)*4+2],px[(h/2*w+w/2)*4+3]);
                        // Write BMP
                        FILE* ff = fopen(path, "wb");
                        if (ff) {
                            uint32_t rs = w*4, pds = rs*h, fs = 54+pds;
                            uint8_t fh[14]={}; fh[0]='B'; fh[1]='M'; *(uint32_t*)(fh+2)=fs; *(uint32_t*)(fh+10)=54; fwrite(fh,1,14,ff);
                            uint8_t dh[40]={}; *(uint32_t*)dh=40; *(int32_t*)(dh+4)=w; *(int32_t*)(dh+8)=-(int32_t)h;
                            *(uint16_t*)(dh+12)=1; *(uint16_t*)(dh+14)=32; fwrite(dh,1,40,ff);
                            fwrite(px,1,w*h*4,ff); fclose(ff);
                        }
                        vkUnmapMemory(device_, sMem);
                    }
                    vkDestroyBuffer(device_, sBuf, nullptr); vkFreeMemory(device_, sMem, nullptr);
                }
            }
        }

#endif
        // Auto-acquire next image for the next frame
        uint32_t oldIdx = it->second.currentImageIndex;
        VkSemaphore acqSem = VK_NULL_HANDLE;
        for (auto& [sid, sem] : semaphores_) { acqSem = sem; break; }
        vkAcquireNextImageKHR(device_, it->second.swapchain, UINT64_MAX,
                              acqSem, VK_NULL_HANDLE, &it->second.currentImageIndex);
        fprintf(stderr, "[Decoder] AutoAcquire: %u -> %u (acqSem=%p)\n",
                oldIdx, it->second.currentImageIndex, (void*)acqSem);
    }
    pendingPresents_.clear();
}

HostSwapchain* VnDecoder::getSwapchain(uint64_t id) {
    auto it = swapchains_.find(id);
    return (it != swapchains_.end()) ? &it->second : nullptr;
}

void VnDecoder::cleanup() {
    vkDeviceWaitIdle(device_);
    for (auto& [id, p] : pipelines_) vkDestroyPipeline(device_, p, nullptr);
    for (auto& [id, l] : pipelineLayouts_) vkDestroyPipelineLayout(device_, l, nullptr);
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

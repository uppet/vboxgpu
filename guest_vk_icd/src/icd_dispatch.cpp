// VBox GPU Bridge — Vulkan ICD dispatch table.
// Maps Vulkan function names to our proxy implementations.

#include "icd_dispatch.h"
#include <cstring>
#include <cstdio>

IcdState g_icd;

// Dispatchable handles (VkInstance, VkDevice, VkQueue, VkCommandBuffer)
// need a loader dispatch table pointer as their first member.
// We store a dummy pointer followed by our ID.
struct DispatchableHandle {
    void* loaderDisp; // the Vulkan loader writes its trampoline table pointer here
    uint64_t id;
};

static uint64_t toId(void* handle) {
    if (!handle) return 0;
    return reinterpret_cast<DispatchableHandle*>(handle)->id;
}

static void* makeDispatchable(uint64_t id) {
    auto* h = new DispatchableHandle();
    h->loaderDisp = nullptr; // loader will overwrite this
    h->id = id;
    return h;
}

// Non-dispatchable handles: cast uint64_t directly
static uint64_t ndToId(uint64_t handle) { return handle; }
static uint64_t idToNd(uint64_t id) { return id; }

// Forward declaration
static PFN_vkVoidFunction lookupFunc(const char* pName);

// --- ICD State ---

bool IcdState::connectToHost(const char* host, uint16_t port) {
    if (!transport.connect(host, port)) return false;
    connected = true;
    return true;
}

bool IcdState::sendAndRecv(uint32_t* imageIndexOut) {
    encoder.cmdEndOfStream();
    bool ok = transport.send(encoder.data(), encoder.size());
    // Reset encoder for next batch
    encoder = VnEncoder();
    if (!ok) return false;

    // Receive host response
    uint8_t resp[64];
    size_t n = transport.recv(resp, sizeof(resp));
    if (n >= 4 && imageIndexOut) {
        memcpy(imageIndexOut, resp, 4);
    }
    return true;
}

// =============================================================
// Vulkan function implementations
// =============================================================

// --- Instance ---
static VkResult VKAPI_CALL icd_vkCreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* pInstance)
{
    g_icd.initDefaults();

    // Connect to host (env var or default)
    const char* hostAddr = getenv("VBOX_GPU_HOST");
    if (!hostAddr) hostAddr = "127.0.0.1";
    const char* portStr = getenv("VBOX_GPU_PORT");
    uint16_t port = portStr ? (uint16_t)atoi(portStr) : DEFAULT_PORT;

    if (!g_icd.connectToHost(hostAddr, port)) {
        fprintf(stderr, "[ICD] Failed to connect to Host at %s:%u\n", hostAddr, port);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    fprintf(stderr, "[ICD] Connected to Host at %s:%u\n", hostAddr, port);

    uint64_t id = g_icd.handles.alloc();
    *pInstance = reinterpret_cast<VkInstance>(makeDispatchable(id));
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    if (instance) delete reinterpret_cast<DispatchableHandle*>(instance);
}

static VkResult VKAPI_CALL icd_vkEnumeratePhysicalDevices(
    VkInstance, uint32_t* pCount, VkPhysicalDevice* pDevices)
{
    if (!pDevices) { *pCount = 1; return VK_SUCCESS; }
    if (*pCount < 1) { *pCount = 1; return VK_INCOMPLETE; }
    static DispatchableHandle physDev = { nullptr, 1 };
    pDevices[0] = reinterpret_cast<VkPhysicalDevice>(&physDev);
    *pCount = 1;
    return VK_SUCCESS;
}

// --- Physical device queries ---
static void VKAPI_CALL icd_vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = g_icd.physDeviceProps;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    icd_vkGetPhysicalDeviceProperties(pd, &p->properties);
    // Walk pNext chain and zero-fill extension structs
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) {
        // For now, leave extension structs at their zero-initialized defaults
        next = next->pNext;
    }
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* p) {
    *p = g_icd.physDeviceFeatures;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* p) {
    icd_vkGetPhysicalDeviceFeatures(pd, &p->features);
    // Walk pNext and enable all boolean features in known structs
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) {
        // Set all VkBool32 fields to VK_TRUE for feature structs.
        // Feature structs have sType + pNext + VkBool32 fields.
        // We memset the fields after the header to VK_TRUE.
        switch (next->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            // Fill all VkBool32 fields after sType+pNext with VK_TRUE
            VkBool32* bools = reinterpret_cast<VkBool32*>(
                reinterpret_cast<uint8_t*>(next) + sizeof(VkBaseOutStructure));
            // Conservatively fill up to 64 VkBool32 fields
            // (largest feature struct has ~50 fields)
            size_t structSize = 256; // safe upper bound
            size_t headerSize = sizeof(VkBaseOutStructure);
            size_t numBools = (structSize - headerSize) / sizeof(VkBool32);
            if (numBools > 64) numBools = 64;
            for (size_t i = 0; i < numBools; i++)
                bools[i] = VK_TRUE;
            break;
        }
        default:
            break;
        }
        next = next->pNext;
    }
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) {
    *p = g_icd.memProps;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2* p) {
    icd_vkGetPhysicalDeviceMemoryProperties(pd, &p->memoryProperties);
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.queueFamilies.size(); return; }
    uint32_t n = (uint32_t)g_icd.queueFamilies.size();
    if (*pCount < n) n = *pCount;
    memcpy(p, g_icd.queueFamilies.data(), n * sizeof(VkQueueFamilyProperties));
    *pCount = n;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties2* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.queueFamilies.size(); return; }
    uint32_t n = (uint32_t)g_icd.queueFamilies.size();
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++)
        p[i].queueFamilyProperties = g_icd.queueFamilies[i];
    *pCount = n;
}

static VkResult VKAPI_CALL icd_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t* pCount, VkExtensionProperties* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.deviceExtensions.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.deviceExtensions.size();
    if (*pCount < n) { memcpy(p, g_icd.deviceExtensions.data(), *pCount * sizeof(VkExtensionProperties)); return VK_INCOMPLETE; }
    memcpy(p, g_icd.deviceExtensions.data(), n * sizeof(VkExtensionProperties));
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEnumerateInstanceExtensionProperties(
    const char*, uint32_t* pCount, VkExtensionProperties* p)
{
    // Report surface extensions
    static VkExtensionProperties exts[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, 1 },
        { VK_KHR_WIN32_SURFACE_EXTENSION_NAME, 1 },
    };
    if (!p) { *pCount = 2; return VK_SUCCESS; }
    uint32_t n = (*pCount < 2) ? *pCount : 2;
    memcpy(p, exts, n * sizeof(VkExtensionProperties));
    *pCount = n;
    return (n < 2) ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice, VkFormat, VkFormatProperties* p)
{
    // Report full support for all formats (simplification for MVP)
    p->linearTilingFeatures = 0;
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    p->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                        VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice pd, VkFormat format, VkFormatProperties2* p)
{
    icd_vkGetPhysicalDeviceFormatProperties(pd, format, &p->formatProperties);
}

// --- Surface ---
static VkResult VKAPI_CALL icd_vkCreateWin32SurfaceKHR(
    VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR* p)
{
    *p = (VkSurfaceKHR)g_icd.handles.alloc();
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* p)
{
    *p = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* p)
{
    memset(p, 0, sizeof(*p));
    p->minImageCount = 2;
    p->maxImageCount = 8;
    p->currentExtent = g_icd.swapchainExtent;
    p->minImageExtent = { 1, 1 };
    p->maxImageExtent = { 16384, 16384 };
    p->maxImageArrayLayers = 1;
    p->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    p->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    p->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    p->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount, VkSurfaceFormatKHR* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.surfaceFormats.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.surfaceFormats.size();
    if (*pCount < n) n = *pCount;
    memcpy(p, g_icd.surfaceFormats.data(), n * sizeof(VkSurfaceFormatKHR));
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount, VkPresentModeKHR* p)
{
    if (!p) { *pCount = 1; return VK_SUCCESS; }
    p[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pCount = 1;
    return VK_SUCCESS;
}

// --- Device ---
static VkResult VKAPI_CALL icd_vkCreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice* pDevice)
{
    uint64_t id = g_icd.handles.alloc();
    *pDevice = reinterpret_cast<VkDevice>(makeDispatchable(id));
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    if (device) delete reinterpret_cast<DispatchableHandle*>(device);
}

static void VKAPI_CALL icd_vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue* pQueue) {
    static DispatchableHandle queueHandle = { nullptr, 2 }; // H_QUEUE = 2
    *pQueue = reinterpret_cast<VkQueue>(&queueHandle);
}

static VkResult VKAPI_CALL icd_vkDeviceWaitIdle(VkDevice) {
    return VK_SUCCESS;
}

// --- Swapchain ---
static VkResult VKAPI_CALL icd_vkCreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR* pInfo, const VkAllocationCallbacks*, VkSwapchainKHR* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkSwapchainKHR)id;

    g_icd.swapchainExtent = pInfo->imageExtent;
    g_icd.swapchainFormat = pInfo->imageFormat;

    // Tell host to create swapchain
    g_icd.encoder.cmdBridgeCreateSwapchain(1, id,
        pInfo->imageExtent.width, pInfo->imageExtent.height,
        g_icd.swapchainImageCount);

    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkGetSwapchainImagesKHR(
    VkDevice, VkSwapchainKHR, uint32_t* pCount, VkImage* pImages)
{
    if (!pImages) { *pCount = g_icd.swapchainImageCount; return VK_SUCCESS; }
    uint32_t n = g_icd.swapchainImageCount;
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++)
        pImages[i] = (VkImage)(0xFFF00000ull + i); // sentinel values
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkAcquireNextImageKHR(
    VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex)
{
    *pIndex = g_icd.currentImageIndex;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR* pInfo)
{
    g_icd.encoder.cmdBridgeQueuePresent(2, // H_QUEUE
        ndToId((uint64_t)pInfo->pSwapchains[0]),
        pInfo->waitSemaphoreCount > 0 ? ndToId((uint64_t)pInfo->pWaitSemaphores[0]) : 0);

    // Send the accumulated frame to host and get next image index
    uint32_t nextIdx = 0;
    g_icd.sendAndRecv(&nextIdx);
    g_icd.currentImageIndex = nextIdx;

    return VK_SUCCESS;
}

// --- Functions DXVK needs during init ---

static VkResult VKAPI_CALL icd_vkEnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEnumerateInstanceVersion(uint32_t* pVersion) {
    *pVersion = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char* pName);

// --- Resource creation stubs (forward to encoder) ---

static VkResult VKAPI_CALL icd_vkCreateImageView(
    VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* p)
{
    *p = (VkImageView)g_icd.handles.alloc();
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateRenderPass(
    VkDevice, const VkRenderPassCreateInfo* pInfo, const VkAllocationCallbacks*, VkRenderPass* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkRenderPass)id;

    std::vector<uint32_t> fmts, loads, stores, initLayouts, finalLayouts;
    for (uint32_t i = 0; i < pInfo->attachmentCount; i++) {
        fmts.push_back(pInfo->pAttachments[i].format);
        loads.push_back(pInfo->pAttachments[i].loadOp);
        stores.push_back(pInfo->pAttachments[i].storeOp);
        initLayouts.push_back(pInfo->pAttachments[i].initialLayout);
        finalLayouts.push_back(pInfo->pAttachments[i].finalLayout);
    }
    g_icd.encoder.cmdCreateRenderPass(1, id, pInfo->attachmentCount,
        fmts.data(), loads.data(), stores.data(), initLayouts.data(), finalLayouts.data());
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* pInfo,
    const VkAllocationCallbacks* a, VkRenderPass* p)
{
    // Simplified: treat like CreateRenderPass
    uint64_t id = g_icd.handles.alloc();
    *p = (VkRenderPass)id;

    std::vector<uint32_t> fmts, loads, stores, initLayouts, finalLayouts;
    for (uint32_t i = 0; i < pInfo->attachmentCount; i++) {
        fmts.push_back(pInfo->pAttachments[i].format);
        loads.push_back(pInfo->pAttachments[i].loadOp);
        stores.push_back(pInfo->pAttachments[i].storeOp);
        initLayouts.push_back(pInfo->pAttachments[i].initialLayout);
        finalLayouts.push_back(pInfo->pAttachments[i].finalLayout);
    }
    g_icd.encoder.cmdCreateRenderPass(1, id, pInfo->attachmentCount,
        fmts.data(), loads.data(), stores.data(), initLayouts.data(), finalLayouts.data());
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateShaderModule(
    VkDevice, const VkShaderModuleCreateInfo* pInfo, const VkAllocationCallbacks*, VkShaderModule* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkShaderModule)id;
    g_icd.encoder.cmdCreateShaderModule(1, id,
        pInfo->pCode, pInfo->codeSize);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreatePipelineLayout(
    VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkPipelineLayout)id;
    g_icd.encoder.cmdCreatePipelineLayout(1, id);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateGraphicsPipelines(
    VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* pInfos,
    const VkAllocationCallbacks*, VkPipeline* pPipelines)
{
    for (uint32_t i = 0; i < count; i++) {
        uint64_t id = g_icd.handles.alloc();
        pPipelines[i] = (VkPipeline)id;

        uint64_t vertMod = 0, fragMod = 0;
        for (uint32_t s = 0; s < pInfos[i].stageCount; s++) {
            if (pInfos[i].pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                vertMod = (uint64_t)pInfos[i].pStages[s].module;
            if (pInfos[i].pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                fragMod = (uint64_t)pInfos[i].pStages[s].module;
        }

        uint32_t w = 800, h = 600;
        if (pInfos[i].pViewportState && pInfos[i].pViewportState->pViewports) {
            w = (uint32_t)pInfos[i].pViewportState->pViewports[0].width;
            h = (uint32_t)pInfos[i].pViewportState->pViewports[0].height;
        }

        g_icd.encoder.cmdCreateGraphicsPipeline(1, id,
            (uint64_t)pInfos[i].renderPass, (uint64_t)pInfos[i].layout,
            vertMod, fragMod, w, h);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateFramebuffer(
    VkDevice, const VkFramebufferCreateInfo* pInfo, const VkAllocationCallbacks*, VkFramebuffer* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkFramebuffer)id;
    uint64_t ivId = pInfo->attachmentCount > 0 ? (uint64_t)pInfo->pAttachments[0] : 0;
    g_icd.encoder.cmdCreateFramebuffer(1, id,
        (uint64_t)pInfo->renderPass, ivId, pInfo->width, pInfo->height);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateCommandPool(
    VkDevice, const VkCommandPoolCreateInfo* pInfo, const VkAllocationCallbacks*, VkCommandPool* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkCommandPool)id;
    g_icd.encoder.cmdCreateCommandPool(1, id, pInfo->queueFamilyIndex);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) { return VK_SUCCESS; }

static VkResult VKAPI_CALL icd_vkAllocateCommandBuffers(
    VkDevice, const VkCommandBufferAllocateInfo* pInfo, VkCommandBuffer* pCBs)
{
    for (uint32_t i = 0; i < pInfo->commandBufferCount; i++) {
        uint64_t id = g_icd.handles.alloc();
        pCBs[i] = reinterpret_cast<VkCommandBuffer>(makeDispatchable(id));
        g_icd.encoder.cmdAllocateCommandBuffers(1, (uint64_t)pInfo->commandPool, id);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer* pCBs) {
    for (uint32_t i = 0; i < count; i++)
        if (pCBs[i]) delete reinterpret_cast<DispatchableHandle*>(pCBs[i]);
}

static VkResult VKAPI_CALL icd_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo*) {
    g_icd.encoder.cmdBeginCommandBuffer(toId(cb));
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEndCommandBuffer(VkCommandBuffer cb) {
    g_icd.encoder.cmdEndCommandBuffer(toId(cb));
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }

// --- Command recording ---

static void VKAPI_CALL icd_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* pInfo, VkSubpassContents) {
    float cr = 0, cg = 0, cb_ = 0, ca = 1;
    if (pInfo->clearValueCount > 0) {
        cr = pInfo->pClearValues[0].color.float32[0];
        cg = pInfo->pClearValues[0].color.float32[1];
        cb_ = pInfo->pClearValues[0].color.float32[2];
        ca = pInfo->pClearValues[0].color.float32[3];
    }
    g_icd.encoder.cmdBeginRenderPass(toId(cb),
        (uint64_t)pInfo->renderPass, (uint64_t)pInfo->framebuffer,
        pInfo->renderArea.extent.width, pInfo->renderArea.extent.height,
        cr, cg, cb_, ca);
}

static void VKAPI_CALL icd_vkCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo*) {
    icd_vkCmdBeginRenderPass(cb, pInfo, VK_SUBPASS_CONTENTS_INLINE);
}

static void VKAPI_CALL icd_vkCmdEndRenderPass(VkCommandBuffer cb) {
    g_icd.encoder.cmdEndRenderPass(toId(cb));
}

static void VKAPI_CALL icd_vkCmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo*) {
    icd_vkCmdEndRenderPass(cb);
}

static void VKAPI_CALL icd_vkCmdNextSubpass(VkCommandBuffer, VkSubpassContents) {}
static void VKAPI_CALL icd_vkCmdNextSubpass2(VkCommandBuffer, const VkSubpassBeginInfo*, const VkSubpassEndInfo*) {}

static void VKAPI_CALL icd_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint, VkPipeline pipeline) {
    g_icd.encoder.cmdBindPipeline(toId(cb), (uint64_t)pipeline);
}

static void VKAPI_CALL icd_vkCmdSetViewport(VkCommandBuffer cb, uint32_t, uint32_t count, const VkViewport* vps) {
    if (count > 0)
        g_icd.encoder.cmdSetViewport(toId(cb), vps[0].x, vps[0].y, vps[0].width, vps[0].height, vps[0].minDepth, vps[0].maxDepth);
}

static void VKAPI_CALL icd_vkCmdSetScissor(VkCommandBuffer cb, uint32_t, uint32_t count, const VkRect2D* rects) {
    if (count > 0)
        g_icd.encoder.cmdSetScissor(toId(cb), rects[0].offset.x, rects[0].offset.y, rects[0].extent.width, rects[0].extent.height);
}

static void VKAPI_CALL icd_vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance)
{
    g_icd.encoder.cmdDraw(toId(cb), vertexCount, instanceCount, firstVertex, firstInstance);
}

// --- Sync ---

static VkResult VKAPI_CALL icd_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkSemaphore)id;
    g_icd.encoder.cmdCreateSemaphore(1, id);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateFence(VkDevice, const VkFenceCreateInfo* pInfo, const VkAllocationCallbacks*, VkFence* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkFence)id;
    g_icd.encoder.cmdCreateFence(1, id, pInfo ? pInfo->flags : 0);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkWaitForFences(VkDevice, uint32_t, const VkFence* p, VkBool32, uint64_t) {
    if (p) g_icd.encoder.cmdWaitForFences(1, (uint64_t)p[0]);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkResetFences(VkDevice, uint32_t, const VkFence* p) {
    if (p) g_icd.encoder.cmdResetFences(1, (uint64_t)p[0]);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetFenceStatus(VkDevice, VkFence) { return VK_SUCCESS; }

// --- Queue submit ---

static VkResult VKAPI_CALL icd_vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t cbId = pSubmits[i].commandBufferCount > 0 ? toId(pSubmits[i].pCommandBuffers[0]) : 0;
        uint64_t waitSem = pSubmits[i].waitSemaphoreCount > 0 ? (uint64_t)pSubmits[i].pWaitSemaphores[0] : 0;
        uint64_t sigSem = pSubmits[i].signalSemaphoreCount > 0 ? (uint64_t)pSubmits[i].pSignalSemaphores[0] : 0;
        g_icd.encoder.cmdQueueSubmit(toId(q), cbId, waitSem, sigSem, (uint64_t)fence);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueueSubmit2(VkQueue q, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t cbId = pSubmits[i].commandBufferInfoCount > 0 ? toId(pSubmits[i].pCommandBufferInfos[0].commandBuffer) : 0;
        uint64_t waitSem = pSubmits[i].waitSemaphoreInfoCount > 0 ? (uint64_t)pSubmits[i].pWaitSemaphoreInfos[0].semaphore : 0;
        uint64_t sigSem = pSubmits[i].signalSemaphoreInfoCount > 0 ? (uint64_t)pSubmits[i].pSignalSemaphoreInfos[0].semaphore : 0;
        g_icd.encoder.cmdQueueSubmit(toId(q), cbId, waitSem, sigSem, (uint64_t)fence);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueueWaitIdle(VkQueue) { return VK_SUCCESS; }

// --- Memory ---

static VkResult VKAPI_CALL icd_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* p) {
    *p = (VkDeviceMemory)g_icd.handles.alloc();
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize size, VkMemoryMapFlags, void** ppData) {
    // Allocate shadow memory for host-visible mappings
    *ppData = calloc(1, (size_t)size);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkUnmapMemory(VkDevice, VkDeviceMemory) {}
static VkResult VKAPI_CALL icd_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
static VkResult VKAPI_CALL icd_vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
static void VKAPI_CALL icd_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* p) {
    p->size = 65536; p->alignment = 256; p->memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* p) {
    p->size = 4 * 1024 * 1024; p->alignment = 256; p->memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetBufferMemoryRequirements2(VkDevice, const VkBufferMemoryRequirementsInfo2*, VkMemoryRequirements2* p) {
    p->memoryRequirements.size = 65536; p->memoryRequirements.alignment = 256; p->memoryRequirements.memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2*, VkMemoryRequirements2* p) {
    p->memoryRequirements.size = 4*1024*1024; p->memoryRequirements.alignment = 256; p->memoryRequirements.memoryTypeBits = 0x3;
}

// --- Buffer / Image creation ---

static VkResult VKAPI_CALL icd_vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* p) {
    *p = (VkBuffer)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* p) {
    *p = (VkImage)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}

// --- Sampler / Descriptor ---

static VkResult VKAPI_CALL icd_vkCreateSampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* p) {
    *p = (VkSampler)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* p) {
    *p = (VkDescriptorSetLayout)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* p) {
    *p = (VkDescriptorPool)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkResetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) { return VK_SUCCESS; }

static VkResult VKAPI_CALL icd_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* pInfo, VkDescriptorSet* p) {
    for (uint32_t i = 0; i < pInfo->descriptorSetCount; i++)
        p[i] = (VkDescriptorSet)g_icd.handles.alloc();
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*) { return VK_SUCCESS; }
static void VKAPI_CALL icd_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) {}

// --- Pipeline cache ---

static VkResult VKAPI_CALL icd_vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache* p) {
    *p = (VkPipelineCache)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks*) {}

// --- Various command stubs ---

static void VKAPI_CALL icd_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout,
    uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static void VKAPI_CALL icd_vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*) {}
static void VKAPI_CALL icd_vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {}
static void VKAPI_CALL icd_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    g_icd.encoder.cmdDraw(toId(cb), indexCount, instanceCount, firstIndex, firstInstance);
}
static void VKAPI_CALL icd_vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*) {}
static void VKAPI_CALL icd_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*) {}
static void VKAPI_CALL icd_vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*) {}
static void VKAPI_CALL icd_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*) {}
static void VKAPI_CALL icd_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
static void VKAPI_CALL icd_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo*) {}
static void VKAPI_CALL icd_vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {}
static void VKAPI_CALL icd_vkCmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment*, uint32_t, const VkClearRect*) {}
static void VKAPI_CALL icd_vkCmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags, uint32_t) {}
static void VKAPI_CALL icd_vkCmdSetBlendConstants(VkCommandBuffer, const float[4]) {}
static void VKAPI_CALL icd_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) {}
static void VKAPI_CALL icd_vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
static void VKAPI_CALL icd_vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t) {}
static void VKAPI_CALL icd_vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
static void VKAPI_CALL icd_vkCmdResolveImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageResolve*) {}
static void VKAPI_CALL icd_vkCmdSetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags) {}
static void VKAPI_CALL icd_vkCmdResetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags) {}
static void VKAPI_CALL icd_vkCmdSetEvent2(VkCommandBuffer, VkEvent, const VkDependencyInfo*) {}
static void VKAPI_CALL icd_vkCmdResetEvent2(VkCommandBuffer, VkEvent, VkPipelineStageFlags2) {}
static void VKAPI_CALL icd_vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdWriteTimestamp2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags) {}
static void VKAPI_CALL icd_vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}
static void VKAPI_CALL icd_vkCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}

// --- Event / Query / misc ---

static VkResult VKAPI_CALL icd_vkCreateEvent(VkDevice, const VkEventCreateInfo*, const VkAllocationCallbacks*, VkEvent* p) {
    *p = (VkEvent)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyEvent(VkDevice, VkEvent, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool* p) {
    *p = (VkQueryPool)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkQueryResultFlags) {
    return VK_NOT_READY;
}
static void VKAPI_CALL icd_vkResetQueryPool(VkDevice, VkQueryPool, uint32_t, uint32_t) {}

static VkResult VKAPI_CALL icd_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo*,
    const VkAllocationCallbacks*, VkPipeline* p) {
    for (uint32_t i = 0; i < count; i++) p[i] = (VkPipeline)g_icd.handles.alloc();
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
    VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t* pCount, VkSparseImageFormatProperties*) { *pCount = 0; }

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
    VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* p) {
    p->maxExtent = { 16384, 16384, 2048 };
    p->maxMipLevels = 15;
    p->maxArrayLayers = 2048;
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    p->maxResourceSize = 2ull * 1024 * 1024 * 1024;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
    const VkPhysicalDeviceImageFormatInfo2* pInfo, VkImageFormatProperties2* p) {
    return icd_vkGetPhysicalDeviceImageFormatProperties(pd, pInfo->format, pInfo->type,
        pInfo->tiling, pInfo->usage, pInfo->flags, &p->imageFormatProperties);
}

static void VKAPI_CALL icd_vkFlushMappedMemoryRanges() {}
static void VKAPI_CALL icd_vkInvalidateMappedMemoryRanges() {}

// --- Additional stubs DXVK queries ---

static void VKAPI_CALL icd_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* p) {
    memset(p, 0, sizeof(*p));
    p->sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* pCount, VkSparseImageFormatProperties2*) { *pCount = 0; }

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR* p) {
    return icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, VK_NULL_HANDLE, &p->surfaceCapabilities);
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t* pCount, VkSurfaceFormat2KHR* p) {
    if (!p) { *pCount = (uint32_t)g_icd.surfaceFormats.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.surfaceFormats.size();
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++) {
        p[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
        p[i].surfaceFormat = g_icd.surfaceFormats[i];
    }
    *pCount = n;
    return VK_SUCCESS;
}

static VkBool32 VKAPI_CALL icd_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t) { return VK_TRUE; }

// Debug utils - no-ops
static void VKAPI_CALL icd_vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
static void VKAPI_CALL icd_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer) {}
static void VKAPI_CALL icd_vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
static VkResult VKAPI_CALL icd_vkCreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* p) {
    *p = (VkDebugUtilsMessengerEXT)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {}
static void VKAPI_CALL icd_vkSubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*) {}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t* pCount, VkPresentModeKHR* p) {
    return icd_vkGetPhysicalDeviceSurfacePresentModesKHR(pd, VK_NULL_HANDLE, pCount, p);
}

static VkResult VKAPI_CALL icd_vkReleaseSwapchainImagesEXT(VkDevice, const void*) { return VK_SUCCESS; }

// --- GetDeviceProcAddr ---

static PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return lookupFunc(pName);
}

// =============================================================
// Function dispatch table
// =============================================================

struct FuncEntry {
    const char* name;
    PFN_vkVoidFunction func;
};

#define ENTRY(fn) { #fn, reinterpret_cast<PFN_vkVoidFunction>(icd_##fn) }

static const FuncEntry g_funcTable[] = {
    // Instance
    ENTRY(vkCreateInstance),
    ENTRY(vkDestroyInstance),
    ENTRY(vkEnumeratePhysicalDevices),
    ENTRY(vkEnumerateInstanceExtensionProperties),
    ENTRY(vkEnumerateInstanceLayerProperties),
    ENTRY(vkEnumerateInstanceVersion),
    ENTRY(vkGetDeviceProcAddr),

    // Physical device
    ENTRY(vkGetPhysicalDeviceProperties),
    ENTRY(vkGetPhysicalDeviceProperties2),
    ENTRY(vkGetPhysicalDeviceFeatures),
    ENTRY(vkGetPhysicalDeviceFeatures2),
    ENTRY(vkGetPhysicalDeviceMemoryProperties),
    ENTRY(vkGetPhysicalDeviceMemoryProperties2),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2),
    ENTRY(vkEnumerateDeviceExtensionProperties),
    ENTRY(vkGetPhysicalDeviceFormatProperties),
    ENTRY(vkGetPhysicalDeviceFormatProperties2),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties2),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties),

    // Surface
    ENTRY(vkCreateWin32SurfaceKHR),
    ENTRY(vkDestroySurfaceKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR),
    ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR),

    // Device
    ENTRY(vkCreateDevice),
    ENTRY(vkDestroyDevice),
    ENTRY(vkGetDeviceQueue),
    ENTRY(vkDeviceWaitIdle),
    ENTRY(vkQueueWaitIdle),

    // Swapchain
    ENTRY(vkCreateSwapchainKHR),
    ENTRY(vkDestroySwapchainKHR),
    ENTRY(vkGetSwapchainImagesKHR),
    ENTRY(vkAcquireNextImageKHR),
    ENTRY(vkQueuePresentKHR),

    // Memory
    ENTRY(vkAllocateMemory),
    ENTRY(vkFreeMemory),
    ENTRY(vkMapMemory),
    ENTRY(vkUnmapMemory),
    ENTRY(vkBindBufferMemory),
    ENTRY(vkBindImageMemory),
    ENTRY(vkGetBufferMemoryRequirements),
    ENTRY(vkGetImageMemoryRequirements),
    ENTRY(vkGetBufferMemoryRequirements2),
    ENTRY(vkGetImageMemoryRequirements2),
    ENTRY(vkFlushMappedMemoryRanges),
    ENTRY(vkInvalidateMappedMemoryRanges),

    // Buffer / Image
    ENTRY(vkCreateBuffer),
    ENTRY(vkDestroyBuffer),
    ENTRY(vkCreateImage),
    ENTRY(vkDestroyImage),
    ENTRY(vkCreateImageView),
    ENTRY(vkDestroyImageView),

    // Shader / Pipeline
    ENTRY(vkCreateShaderModule),
    ENTRY(vkDestroyShaderModule),
    ENTRY(vkCreatePipelineLayout),
    ENTRY(vkDestroyPipelineLayout),
    ENTRY(vkCreateGraphicsPipelines),
    ENTRY(vkCreateComputePipelines),
    ENTRY(vkDestroyPipeline),
    ENTRY(vkCreatePipelineCache),
    ENTRY(vkDestroyPipelineCache),
    ENTRY(vkCreateRenderPass),
    ENTRY(vkCreateRenderPass2),
    ENTRY(vkDestroyRenderPass),
    ENTRY(vkCreateFramebuffer),
    ENTRY(vkDestroyFramebuffer),

    // Sampler / Descriptor
    ENTRY(vkCreateSampler),
    ENTRY(vkDestroySampler),
    ENTRY(vkCreateDescriptorSetLayout),
    ENTRY(vkDestroyDescriptorSetLayout),
    ENTRY(vkCreateDescriptorPool),
    ENTRY(vkDestroyDescriptorPool),
    ENTRY(vkResetDescriptorPool),
    ENTRY(vkAllocateDescriptorSets),
    ENTRY(vkFreeDescriptorSets),
    ENTRY(vkUpdateDescriptorSets),

    // Command pool / buffer
    ENTRY(vkCreateCommandPool),
    ENTRY(vkDestroyCommandPool),
    ENTRY(vkResetCommandPool),
    ENTRY(vkAllocateCommandBuffers),
    ENTRY(vkFreeCommandBuffers),
    ENTRY(vkBeginCommandBuffer),
    ENTRY(vkEndCommandBuffer),
    ENTRY(vkResetCommandBuffer),

    // Command recording
    ENTRY(vkCmdBeginRenderPass),
    ENTRY(vkCmdBeginRenderPass2),
    ENTRY(vkCmdEndRenderPass),
    ENTRY(vkCmdEndRenderPass2),
    ENTRY(vkCmdNextSubpass),
    ENTRY(vkCmdNextSubpass2),
    ENTRY(vkCmdBindPipeline),
    ENTRY(vkCmdSetViewport),
    ENTRY(vkCmdSetScissor),
    ENTRY(vkCmdDraw),
    ENTRY(vkCmdDrawIndexed),
    ENTRY(vkCmdBindDescriptorSets),
    ENTRY(vkCmdBindVertexBuffers),
    ENTRY(vkCmdBindIndexBuffer),
    ENTRY(vkCmdCopyBuffer),
    ENTRY(vkCmdCopyImage),
    ENTRY(vkCmdCopyBufferToImage),
    ENTRY(vkCmdCopyImageToBuffer),
    ENTRY(vkCmdPipelineBarrier),
    ENTRY(vkCmdPipelineBarrier2),
    ENTRY(vkCmdClearColorImage),
    ENTRY(vkCmdClearAttachments),
    ENTRY(vkCmdSetStencilReference),
    ENTRY(vkCmdSetBlendConstants),
    ENTRY(vkCmdPushConstants),
    ENTRY(vkCmdDispatch),
    ENTRY(vkCmdFillBuffer),
    ENTRY(vkCmdUpdateBuffer),
    ENTRY(vkCmdResolveImage),
    ENTRY(vkCmdSetEvent),
    ENTRY(vkCmdResetEvent),
    ENTRY(vkCmdSetEvent2),
    ENTRY(vkCmdResetEvent2),
    ENTRY(vkCmdWriteTimestamp),
    ENTRY(vkCmdWriteTimestamp2),
    ENTRY(vkCmdBeginQuery),
    ENTRY(vkCmdEndQuery),
    ENTRY(vkCmdResetQueryPool),
    ENTRY(vkCmdCopyQueryPoolResults),

    // Sync
    ENTRY(vkCreateSemaphore),
    ENTRY(vkDestroySemaphore),
    ENTRY(vkCreateFence),
    ENTRY(vkDestroyFence),
    ENTRY(vkWaitForFences),
    ENTRY(vkResetFences),
    ENTRY(vkGetFenceStatus),
    ENTRY(vkQueueSubmit),
    ENTRY(vkQueueSubmit2),

    // Additional DXVK queries
    ENTRY(vkGetPhysicalDeviceExternalSemaphoreProperties),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilities2KHR),
    ENTRY(vkGetPhysicalDeviceSurfaceFormats2KHR),
    ENTRY(vkGetPhysicalDeviceWin32PresentationSupportKHR),
    ENTRY(vkCmdBeginDebugUtilsLabelEXT),
    ENTRY(vkCmdEndDebugUtilsLabelEXT),
    ENTRY(vkCmdInsertDebugUtilsLabelEXT),
    ENTRY(vkCreateDebugUtilsMessengerEXT),
    ENTRY(vkDestroyDebugUtilsMessengerEXT),
    ENTRY(vkSubmitDebugUtilsMessageEXT),
    ENTRY(vkGetPhysicalDeviceSurfacePresentModes2EXT),
    ENTRY(vkReleaseSwapchainImagesEXT),

    // Event / Query
    ENTRY(vkCreateEvent),
    ENTRY(vkDestroyEvent),
    ENTRY(vkCreateQueryPool),
    ENTRY(vkDestroyQueryPool),
    ENTRY(vkGetQueryPoolResults),
    ENTRY(vkResetQueryPool),
};

#undef ENTRY

static PFN_vkVoidFunction lookupFunc(const char* pName) {
    for (const auto& e : g_funcTable) {
        if (strcmp(e.name, pName) == 0)
            return e.func;
    }
    // Log unknown functions for debugging
    fprintf(stderr, "[ICD] Unknown function: %s\n", pName);
    return nullptr;
}

// --- ICD entry points ---
extern "C" {

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return lookupFunc(pName);
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return lookupFunc(pName);
}

__declspec(dllexport) VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    *pVersion = 5;
    return VK_SUCCESS;
}

} // extern "C"

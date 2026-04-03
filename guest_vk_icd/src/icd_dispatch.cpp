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
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) { next = next->pNext; }
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

// --- Stub for unimplemented functions ---
static void VKAPI_CALL icd_stub() {
    // Intentionally empty — called for unimplemented Vulkan functions
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

    // Swapchain
    ENTRY(vkCreateSwapchainKHR),
    ENTRY(vkDestroySwapchainKHR),
    ENTRY(vkGetSwapchainImagesKHR),
    ENTRY(vkAcquireNextImageKHR),
    ENTRY(vkQueuePresentKHR),
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

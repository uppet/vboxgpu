// Hardcoded physical device properties.
// DXVK queries these during init to decide feature levels and code paths.
// For MVP we hardcode values matching a typical discrete GPU.
// Later these will be queried from Host's real GPU.

#include "icd_dispatch.h"
#include <cstring>

void IcdState::initDefaults() {
    // --- Physical device properties ---
    memset(&physDeviceProps, 0, sizeof(physDeviceProps));
    physDeviceProps.apiVersion = VK_API_VERSION_1_3;
    physDeviceProps.driverVersion = 1;
    physDeviceProps.vendorID = 0x10DE; // NVIDIA
    physDeviceProps.deviceID = 0x2191; // GTX 1660 Ti
    physDeviceProps.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    strncpy(physDeviceProps.deviceName, "VBox GPU Bridge (Remote)", VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

    auto& limits = physDeviceProps.limits;
    limits.maxImageDimension1D = 16384;
    limits.maxImageDimension2D = 16384;
    limits.maxImageDimension3D = 2048;
    limits.maxImageDimensionCube = 16384;
    limits.maxImageArrayLayers = 2048;
    limits.maxTexelBufferElements = 128 * 1024 * 1024;
    limits.maxUniformBufferRange = 65536;
    limits.maxStorageBufferRange = 2u * 1024 * 1024 * 1024 - 1;
    limits.maxPushConstantsSize = 256;
    limits.maxMemoryAllocationCount = 4096;
    limits.maxSamplerAllocationCount = 4000;
    limits.maxBoundDescriptorSets = 8;
    limits.maxPerStageDescriptorSamplers = 16;
    limits.maxPerStageDescriptorUniformBuffers = 15;
    limits.maxPerStageDescriptorStorageBuffers = 16;
    limits.maxPerStageDescriptorSampledImages = 128;
    limits.maxPerStageDescriptorStorageImages = 8;
    limits.maxDescriptorSetSamplers = 80;
    limits.maxDescriptorSetUniformBuffers = 90;
    limits.maxDescriptorSetStorageBuffers = 96;
    limits.maxDescriptorSetSampledImages = 256;
    limits.maxDescriptorSetStorageImages = 40;
    limits.maxVertexInputAttributes = 32;
    limits.maxVertexInputBindings = 32;
    limits.maxVertexInputAttributeOffset = 2047;
    limits.maxVertexInputBindingStride = 2048;
    limits.maxVertexOutputComponents = 128;
    limits.maxFragmentInputComponents = 128;
    limits.maxFragmentOutputAttachments = 8;
    limits.maxComputeSharedMemorySize = 49152;
    limits.maxComputeWorkGroupCount[0] = 65535;
    limits.maxComputeWorkGroupCount[1] = 65535;
    limits.maxComputeWorkGroupCount[2] = 65535;
    limits.maxComputeWorkGroupInvocations = 1024;
    limits.maxComputeWorkGroupSize[0] = 1024;
    limits.maxComputeWorkGroupSize[1] = 1024;
    limits.maxComputeWorkGroupSize[2] = 64;
    limits.maxViewports = 16;
    limits.maxViewportDimensions[0] = 16384;
    limits.maxViewportDimensions[1] = 16384;
    limits.maxFramebufferWidth = 16384;
    limits.maxFramebufferHeight = 16384;
    limits.maxFramebufferLayers = 2048;
    limits.maxColorAttachments = 8;
    limits.maxClipDistances = 8;
    limits.maxCullDistances = 8;
    limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
    limits.framebufferDepthSampleCounts = limits.framebufferColorSampleCounts;
    limits.framebufferStencilSampleCounts = limits.framebufferColorSampleCounts;
    limits.timestampComputeAndGraphics = VK_TRUE;
    limits.timestampPeriod = 1.0f;
    limits.pointSizeRange[0] = 1.0f;
    limits.pointSizeRange[1] = 64.0f;
    limits.lineWidthRange[0] = 1.0f;
    limits.lineWidthRange[1] = 8.0f;
    limits.minMemoryMapAlignment = 64;
    limits.minTexelBufferOffsetAlignment = 16;
    limits.minUniformBufferOffsetAlignment = 256;
    limits.minStorageBufferOffsetAlignment = 16;
    limits.nonCoherentAtomSize = 64;

    // --- Features: enable everything (we're a proxy, Host GPU does the real work) ---
    memset(&physDeviceFeatures, VK_TRUE, sizeof(physDeviceFeatures));

    // --- Memory properties ---
    memset(&memProps, 0, sizeof(memProps));
    memProps.memoryTypeCount = 2;
    // Type 0: device local
    memProps.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memProps.memoryTypes[0].heapIndex = 0;
    // Type 1: host visible + coherent
    memProps.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memProps.memoryTypes[1].heapIndex = 1;
    memProps.memoryHeapCount = 2;
    memProps.memoryHeaps[0].size = 4ull * 1024 * 1024 * 1024; // 4 GB VRAM
    memProps.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memProps.memoryHeaps[1].size = 8ull * 1024 * 1024 * 1024; // 8 GB system
    memProps.memoryHeaps[1].flags = 0;

    // --- Queue families ---
    VkQueueFamilyProperties gfxQueue{};
    gfxQueue.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    gfxQueue.queueCount = 1;
    gfxQueue.timestampValidBits = 64;
    gfxQueue.minImageTransferGranularity = { 1, 1, 1 };
    queueFamilies.push_back(gfxQueue);

    // --- Device extensions (minimum for DXVK) ---
    auto addExt = [this](const char* name) {
        VkExtensionProperties ext{};
        strncpy(ext.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE);
        ext.specVersion = 1;
        deviceExtensions.push_back(ext);
    };
    addExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    addExt(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    addExt(VK_KHR_MAINTENANCE2_EXTENSION_NAME);
    addExt(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
    addExt(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    addExt(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    addExt(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    addExt(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    addExt(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    addExt(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    addExt(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    // --- Surface formats ---
    surfaceFormats.push_back({ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
    surfaceFormats.push_back({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
}

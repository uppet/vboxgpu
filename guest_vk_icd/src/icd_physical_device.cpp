// Hardcoded physical device properties.
// DXVK queries these during init to decide feature levels and code paths.
// For MVP we hardcode values matching a typical discrete GPU.
// Later these will be queried from Host's real GPU.

#include "icd_dispatch.h"
#include <cstring>

void IcdState::initDefaults() {
    // --- Physical device properties ---
    memset(&physDeviceProps, 0, sizeof(physDeviceProps));
    // Use Vulkan 1.2 for compatibility with DXVK 2.3.x (doesn't require 1.3)
    physDeviceProps.apiVersion = VK_API_VERSION_1_2;
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

    // --- Features: enable everything ---
    // Set every VkBool32 field to VK_TRUE (=1), not 0x01010101 from memset
    VkBool32* featureBools = reinterpret_cast<VkBool32*>(&physDeviceFeatures);
    size_t numFeatures = sizeof(physDeviceFeatures) / sizeof(VkBool32);
    for (size_t i = 0; i < numFeatures; i++) featureBools[i] = VK_TRUE;

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
    // Report all extensions DXVK may request, EXCEPT ones that cause
    // DXVK to skip vkCreateShaderModule (shader_module_identifier).
    const char* extNames[] = {
        // KHR core
        "VK_KHR_swapchain", "VK_KHR_maintenance1", "VK_KHR_maintenance2",
        "VK_KHR_maintenance3", "VK_KHR_maintenance4", "VK_KHR_maintenance5",
        "VK_KHR_maintenance6", "VK_KHR_maintenance7",
        "VK_KHR_create_renderpass2", "VK_KHR_image_format_list",
        "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_driver_properties",
        "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32",
        "VK_KHR_load_store_op_none", "VK_KHR_present_id", "VK_KHR_present_wait",
        "VK_KHR_swapchain_mutable_format", "VK_KHR_win32_keyed_mutex",
        "VK_KHR_buffer_device_address", "VK_KHR_dynamic_rendering",
        "VK_KHR_depth_stencil_resolve", "VK_KHR_timeline_semaphore",
        "VK_KHR_descriptor_update_template", "VK_KHR_shader_draw_parameters",
        "VK_KHR_draw_indirect_count", "VK_KHR_uniform_buffer_standard_layout",
        "VK_KHR_vulkan_memory_model", "VK_KHR_synchronization2",
        "VK_KHR_copy_commands2", "VK_KHR_format_feature_flags2",
        "VK_KHR_shader_integer_dot_product", "VK_KHR_shader_non_semantic_info",
        "VK_KHR_zero_initialize_workgroup_memory",
        // EXT
        "VK_EXT_transform_feedback", "VK_EXT_vertex_attribute_divisor",
        "VK_EXT_robustness2", "VK_EXT_attachment_feedback_loop_layout",
        "VK_EXT_conservative_rasterization", "VK_EXT_custom_border_color",
        "VK_EXT_depth_clip_enable", "VK_EXT_depth_bias_control",
        "VK_EXT_extended_dynamic_state3",
        "VK_EXT_shader_stencil_export", "VK_EXT_swapchain_colorspace",
        "VK_EXT_swapchain_maintenance1", "VK_EXT_descriptor_indexing",
        "VK_EXT_host_query_reset", "VK_EXT_sampler_filter_minmax",
        "VK_EXT_scalar_block_layout", "VK_EXT_separate_stencil_usage",
        "VK_EXT_shader_demote_to_helper_invocation",
        "VK_EXT_shader_viewport_index_layer", "VK_EXT_subgroup_size_control",
        "VK_EXT_inline_uniform_block", "VK_EXT_pipeline_creation_cache_control",
        "VK_EXT_private_data", "VK_EXT_image_robustness",
        "VK_EXT_4444_formats", "VK_EXT_texel_buffer_alignment",
        "VK_EXT_ycbcr_2plane_444_formats", "VK_EXT_extended_dynamic_state",
        "VK_EXT_extended_dynamic_state2", "VK_EXT_shader_atomic_float",
        // Additional DXVK-referenced extensions
        "VK_EXT_fragment_shader_interlock",
        "VK_EXT_border_color_swizzle",
        "VK_EXT_sample_locations",
        "VK_EXT_multi_draw",
        "VK_EXT_non_seamless_cube_map",
        "VK_EXT_line_rasterization",
        "VK_EXT_pageable_device_local_memory",
        "VK_EXT_memory_budget", "VK_EXT_memory_priority",
        "VK_EXT_mesh_shader",
        "VK_EXT_hdr_metadata",
        "VK_EXT_full_screen_exclusive",
        "VK_EXT_descriptor_buffer",
        "VK_AMD_shader_fragment_mask",
        "VK_KHR_shader_float_controls2",
        "VK_KHR_shader_subgroup_uniform_control_flow",
        "VK_EXT_graphics_pipeline_library", "VK_KHR_pipeline_library",
        "VK_EXT_shader_module_identifier",
        // NV
        "VK_NV_descriptor_pool_overallocation", "VK_NV_low_latency2",
        "VK_NV_raw_access_chains", "VK_NVX_binary_import", "VK_NVX_image_view_handle",
    };
    for (const char* name : extNames)
        addExt(name);

    // --- Surface formats ---
    surfaceFormats.push_back({ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
    surfaceFormats.push_back({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
}

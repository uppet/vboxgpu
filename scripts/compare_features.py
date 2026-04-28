#!/usr/bin/env python3
"""Compare ICD claimed features vs host actually-enabled features.
Root cause analysis for Ultrakill black screen."""

import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ── ICD side: what DXVK sees (from icd_physical_device.cpp) ──
# All VkPhysicalDeviceFeatures booleans set to VK_TRUE
ICD_CORE_FEATURES_ALL_TRUE = True

# ── Host side: what device actually enables (from vk_bootstrap.cpp:148-192) ──
HOST_CORE_FEATURES = {
    'shaderClipDistance': True,
    'shaderCullDistance': True,
    # Everything else: VK_FALSE (zero-initialized)
}

HOST_VK11_FEATURES = {
    'shaderDrawParameters': True,
}

HOST_VK12_FEATURES = {
    'bufferDeviceAddress': True,
    'vulkanMemoryModel': True,
    'vulkanMemoryModelDeviceScope': True,
    'descriptorIndexing': True,
    'runtimeDescriptorArray': True,
    'timelineSemaphore': True,
    'scalarBlockLayout': True,
    'uniformBufferStandardLayout': True,
}

HOST_VK13_FEATURES = {
    'dynamicRendering': True,
    'synchronization2': True,
    # Many more VK13 features NOT enabled
}

HOST_EXTENSIONS = {
    'VK_KHR_swapchain',
    'VK_KHR_push_descriptor',
}

# ── VkPhysicalDeviceFeatures complete list ──
CORE_FEATURES = [
    'robustBufferAccess', 'fullDrawIndexUint32', 'imageCubeArray',
    'independentBlend', 'geometryShader', 'tessellationShader',
    'sampleRateShading', 'dualSrcBlend', 'logicOp',
    'multiDrawIndirect', 'drawIndirectFirstInstance', 'depthClamp',
    'depthBiasClamp', 'fillModeNonSolid', 'depthBounds',
    'wideLines', 'largePoints', 'alphaToOne',
    'multiViewport', 'samplerAnisotropy', 'textureCompressionETC2',
    'textureCompressionASTC_LDR', 'textureCompressionBC',
    'occlusionQueryPrecise', 'pipelineStatisticsQuery',
    'vertexPipelineStoresAndAtomics', 'fragmentStoresAndAtomics',
    'shaderTessellationAndGeometryPointSize', 'shaderImageGatherExtended',
    'shaderStorageImageExtendedFormats', 'shaderStorageImageMultisample',
    'shaderStorageImageReadWithoutFormat', 'shaderStorageImageWriteWithoutFormat',
    'shaderUniformBufferArrayDynamicIndexing', 'shaderSampledImageArrayDynamicIndexing',
    'shaderStorageBufferArrayDynamicIndexing', 'shaderStorageImageArrayDynamicIndexing',
    'shaderClipDistance', 'shaderCullDistance', 'shaderFloat64',
    'shaderInt64', 'shaderInt16', 'shaderResourceResidency',
    'shaderResourceMinLod', 'sparseBinding', 'sparseResidencyBuffer',
    'sparseResidencyImage2D', 'sparseResidencyImage3D',
    'sparseResidency2Samples', 'sparseResidency4Samples',
    'sparseResidency8Samples', 'sparseResidency16Samples',
    'sparseResidencyAliased', 'variableMultisampleRate',
    'inheritedQueries',
]

# ── VkPhysicalDeviceVulkan13Features that DXVK needs ──
VK13_FEATURES_DXVK_NEEDS = [
    'dynamicRendering', 'synchronization2',
    'pipelineCreationCacheControl',
    'descriptorBindingInlineUniformBlockUpdateAfterBind',
    'inlineUniformBlock',
    'shaderDemoteToHelperInvocation',
    'shaderTerminateInvocation',
    'subgroupSizeControl', 'computeFullSubgroups',
    'samplerYcbcrConversion',
    'textureCompressionASTC_HDR',
    'shaderIntegerDotProduct',
    'extendedDynamicState',          # ← CRITICAL for Ultrakill
    'extendedDynamicState2',         # ← CRITICAL for Ultrakill
    'extendedDynamicState3',         # ← might need
    'maintenance4',
]

# ── Validation error → missing feature mapping ──
VALIDATION_TO_FEATURE = {
    'VUID-vkCmdPipelineBarrier-dstStageMask-04091': 'geometryShader (barrier stage)',
    'VUID-vkCmdPipelineBarrier-dstStageMask-04090': 'tessellationShader (barrier stage)',
    'VUID-vkCmdDraw-None-04877': 'extendedDynamicState (depthBiasEnable)',
    'VUID-vkCmdDraw-pStrides-04884': 'extendedDynamicState (vertexInputBindingStride)',
    'VUID-VkPipelineRasterizationStateCreateInfo-depthClampEnable-00782': 'depthClamp',
    'VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-00747': 'extendedDynamicState (dynamic state not supported)',
    'VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-00748': 'extendedDynamicState2 (dynamic state not supported)',
    'VUID-VkShaderModuleCreateInfo-pCode-01091': 'SPIR-V capability not enabled (geometry/tessellation shader)',
}

# ── Validation error counts (from analysis) ──
VALIDATION_COUNTS = {
    'VUID-vkCmdPipelineBarrier-dstStageMask-04091': 5754,
    'UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout': 4104,
    'VUID-vkCmdPipelineBarrier-dstStageMask-04090': 2877,
    'VUID-vkCmdDraw-None-02699': 2640,
    'VUID-vkCmdDrawIndexed-None-02699': 2355,
    'UNASSIGNED-CoreValidation-DrawState-QueueForwardProgress': 1981,
    'VUID-VkWriteDescriptorSet-descriptorType-02997': 1980,
    'UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkBuffer': 1539,
    'VUID-vkCmdBindPipeline-pipeline-06197': 1268,
    'VUID-vkResetFences-pFences-01123': 661,
    'VUID-vkCmdDraw-None-04877': 660,
    'VUID-vkCmdDraw-pStrides-04884': 660,
    'VUID-VkImageMemoryBarrier-oldLayout-01197': 660,
    'VUID-vkPresentInfoKHR-pImageIndices-01296': 660,
    'VUID-vkCmdBindPipeline-pipeline-06196': 464,
    'VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-00747': 12,
    'VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-00748': 12,
    'VUID-VkShaderModuleCreateInfo-pCode-01091': 12,
    'VUID-vkDestroyDevice-device-00378': 12,
    'VUID-VkImageCreateInfo-format-parameter': 11,
    'VUID-VkPipelineRasterizationStateCreateInfo-depthClampEnable-00782': 11,
    'VUID-VkImageViewCreateInfo-image-01762': 6,
    'VUID-VkImageViewCreateInfo-format-parameter': 4,
}

# ══════════════════════════════════════════════════════════════
print("=" * 60)
print("ROOT CAUSE: ICD claims ALL features, Host enables ALMOST NONE")
print("=" * 60)

# Core features mismatch
missing_core = [f for f in CORE_FEATURES if f not in HOST_CORE_FEATURES]
print(f"\nCore VkPhysicalDeviceFeatures:")
print(f"  ICD claims:   ALL {len(CORE_FEATURES)} features = VK_TRUE")
print(f"  Host enables: {len(HOST_CORE_FEATURES)} features")
print(f"  MISSING:      {len(missing_core)} features")

# Show which missing features are directly implicated by validation errors
print(f"\n  Features directly causing validation errors:")
critical_missing = {
    'geometryShader': '5754 barrier errors + 12 pipeline errors',
    'tessellationShader': '2877 barrier errors',
    'depthClamp': '11 pipeline creation errors',
}
for feat, impact in critical_missing.items():
    print(f"    {feat}: {impact}")

# VK13 features
missing_vk13 = [f for f in VK13_FEATURES_DXVK_NEEDS if f not in HOST_VK13_FEATURES]
print(f"\nVkPhysicalDeviceVulkan13Features:")
print(f"  Host enables: {', '.join(HOST_VK13_FEATURES.keys())}")
print(f"  MISSING features DXVK likely needs:")
for f in missing_vk13:
    print(f"    {f}")

# Validation error → missing feature mapping
print(f"\n{'=' * 60}")
print("VALIDATION ERROR → ROOT CAUSE MAPPING")
print(f"{'=' * 60}")
for vid, feat in VALIDATION_TO_FEATURE.items():
    count = VALIDATION_COUNTS.get(vid, '?')
    print(f"  [{count:>5}x] {vid}")
    print(f"         → Missing: {feat}")

# Impact analysis
print(f"\n{'=' * 60}")
print("IMPACT CHAIN")
print(f"{'=' * 60}")
print("""
  ICD tells DXVK: "I support geometryShader, tessellationShader,
                    depthClamp, extendedDynamicState, ALL features"

  DXVK generates:  Pipeline barriers with GEOMETRY/TESSELLATION stages
                    Pipelines using depthClamp
                    Dynamic states (depthBiasEnable, vertexBindingStride)
                    SPIR-V shaders with Geometry capabilities

  Host device has:  NONE of these features enabled

  Result:
    Pipeline creation FAILS (or creates invalid pipelines)
    → Command buffers become invalid
    → All draw calls fail / produce no output
    → Descriptor updates may be silently ignored
    → Rendering output: ALL BLACK
""")

# Fix suggestion
print(f"{'=' * 60}")
print("FIX: vk_bootstrap.cpp needs to enable ALL host-supported features")
print(f"{'=' * 60}")
print("""
  Instead of hand-picking features, query and enable everything:

    VkPhysicalDeviceFeatures2 supported{};
    vkGetPhysicalDeviceFeatures2(physDevice, &supported);
    // Then pass supported as enabled features

  This ensures host device matches what ICD claims to DXVK.
""")

#include "vk_bootstrap.h"
#include <cstring>
#include <algorithm>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan] %s\n", pCallbackData->pMessage);
        fflush(stderr);
    }
    return VK_FALSE;
}

static bool checkValidationLayerSupport() {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (auto& l : layers) {
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            return true;
    }
    return false;
}

void createInstance(VulkanContext& ctx) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VBox GPU Bridge Host";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "vbox-gpu-bridge";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    // Disable validation layer for host server — the proxy ICD sends commands
    // that may reference objects in unusual ways, causing false validation errors.
    // Set env var VBOX_VALIDATION=1 to re-enable for debugging (expect perf hit).
    bool enableValidation = (getenv("VBOX_VALIDATION") != nullptr);

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (enableValidation) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }

    if (vkCreateInstance(&createInfo, nullptr, &ctx.instance) != VK_SUCCESS)
        throw std::runtime_error("Failed to create Vulkan instance");

    if (enableValidation) {
        auto vkCreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT");
        if (vkCreateDebugUtilsMessengerEXT) {
            VkDebugUtilsMessengerCreateInfoEXT dbgInfo{};
            dbgInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbgInfo.pfnUserCallback = debugCallback;
            vkCreateDebugUtilsMessengerEXT(ctx.instance, &dbgInfo, nullptr, &ctx.debugMessenger);
        }
    }
}

void createSurface(VulkanContext& ctx, HWND hwnd, HINSTANCE hInstance) {
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hwnd = hwnd;
    surfaceInfo.hinstance = hInstance;
    if (vkCreateWin32SurfaceKHR(ctx.instance, &surfaceInfo, nullptr, &ctx.surface) != VK_SUCCESS)
        throw std::runtime_error("Failed to create Win32 surface");
}

void pickPhysicalDevice(VulkanContext& ctx) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, nullptr);
    if (deviceCount == 0)
        throw std::runtime_error("No Vulkan-capable GPU found");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, devices.data());

    for (auto& dev : devices) {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, families.data());

        bool foundGraphics = false, foundPresent = false;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                ctx.graphicsFamily = i;
                foundGraphics = true;
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, ctx.surface, &presentSupport);
            if (presentSupport) {
                ctx.presentFamily = i;
                foundPresent = true;
            }
            if (foundGraphics && foundPresent) break;
        }

        if (foundGraphics && foundPresent) {
            ctx.physicalDevice = dev;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            OutputDebugStringA("[VBoxGPU] Using GPU: ");
            OutputDebugStringA(props.deviceName);
            OutputDebugStringA("\n");
            return;
        }
    }
    throw std::runtime_error("No suitable GPU found");
}

void createLogicalDevice(VulkanContext& ctx) {
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;

    uint32_t uniqueFamilies[] = { ctx.graphicsFamily, ctx.presentFamily };
    uint32_t uniqueCount = (ctx.graphicsFamily == ctx.presentFamily) ? 1 : 2;

    for (uint32_t i = 0; i < uniqueCount; i++) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = uniqueFamilies[i];
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(queueInfo);
    }

    // Query ALL features supported by the physical device and enable them all.
    // Previously we hand-picked a small subset, which caused pipeline creation
    // failures for features DXVK uses (depthClamp, geometryShader, etc.).
    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceVulkan11Features vk11Features{};
    vk11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

    VkPhysicalDeviceVulkan12Features vk12Features{};
    vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceVulkan13Features vk13Features{};
    vk13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    // Extended dynamic state features (EDS1/EDS2) — promoted to 1.3 core but some
    // validation layers still check these extension structs explicitly.
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsFeatures{};
    edsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2Features{};
    eds2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;

    // VK_EXT_robustness2: enables nullDescriptor (null image/buffer views in descriptor writes)
    // Required for DXVK's sparse descriptor arrays where unbound slots are written as NULL.
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features{};
    robustness2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;

    // Check if VK_EXT_robustness2 is available before adding to feature chain / ext list.
    bool hasRobustness2 = false;
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &extCount, exts.data());
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) {
                hasRobustness2 = true;
                break;
            }
        }
    }

    // Chain for query: deviceFeatures2 → vk11 → vk12 → vk13 → eds → eds2 [→ robustness2]
    deviceFeatures2.pNext = &vk11Features;
    vk11Features.pNext = &vk12Features;
    vk12Features.pNext = &vk13Features;
    vk13Features.pNext = &edsFeatures;
    edsFeatures.pNext = &eds2Features;
    if (hasRobustness2)
        eds2Features.pNext = &robustness2Features;

    // Query: fills VK_TRUE for every feature the GPU actually supports
    vkGetPhysicalDeviceFeatures2(ctx.physicalDevice, &deviceFeatures2);
    // Pass the same struct chain to vkCreateDevice — enables everything supported.

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };
    if (hasRobustness2)
        deviceExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &deviceFeatures2;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.pEnabledFeatures = nullptr; // using pNext chain instead
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(ctx.physicalDevice, &createInfo, nullptr, &ctx.device) != VK_SUCCESS)
        throw std::runtime_error("Failed to create logical device");

    vkGetDeviceQueue(ctx.device, ctx.graphicsFamily, 0, &ctx.graphicsQueue);
    vkGetDeviceQueue(ctx.device, ctx.presentFamily, 0, &ctx.presentQueue);
}

static SwapchainDetails querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());

    return details;
}

void createSwapchain(VulkanContext& ctx, uint32_t width, uint32_t height) {
    auto details = querySwapchainSupport(ctx.physicalDevice, ctx.surface);

    // Pick format: prefer B8G8R8A8_SRGB
    VkSurfaceFormatKHR surfaceFormat = details.formats[0];
    for (auto& f : details.formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }

    // Pick present mode: prefer MAILBOX, fallback FIFO
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto& m : details.presentModes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = m;
            break;
        }
    }

    // Pick extent
    VkExtent2D extent;
    if (details.capabilities.currentExtent.width != UINT32_MAX) {
        extent = details.capabilities.currentExtent;
    } else {
        extent.width = std::clamp(width, details.capabilities.minImageExtent.width, details.capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, details.capabilities.minImageExtent.height, details.capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount)
        imageCount = details.capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = ctx.surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = surfaceFormat.format;
    swapInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (ctx.graphicsFamily != ctx.presentFamily) {
        uint32_t families[] = { ctx.graphicsFamily, ctx.presentFamily };
        swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapInfo.queueFamilyIndexCount = 2;
        swapInfo.pQueueFamilyIndices = families;
    } else {
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    swapInfo.preTransform = details.capabilities.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(ctx.device, &swapInfo, nullptr, &ctx.swapchain) != VK_SUCCESS)
        throw std::runtime_error("Failed to create swap chain");

    ctx.swapchainFormat = surfaceFormat.format;
    ctx.swapchainExtent = extent;

    vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &imageCount, nullptr);
    ctx.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &imageCount, ctx.swapchainImages.data());
}

void createImageViews(VulkanContext& ctx) {
    ctx.swapchainImageViews.resize(ctx.swapchainImages.size());
    for (size_t i = 0; i < ctx.swapchainImages.size(); i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ctx.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ctx.swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx.device, &viewInfo, nullptr, &ctx.swapchainImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create image view");
    }
}

void cleanupVulkan(VulkanContext& ctx) {
    for (auto iv : ctx.swapchainImageViews)
        vkDestroyImageView(ctx.device, iv, nullptr);
    if (ctx.swapchain) vkDestroySwapchainKHR(ctx.device, ctx.swapchain, nullptr);
    if (ctx.device) vkDestroyDevice(ctx.device, nullptr);
    if (ctx.surface) vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr);
    if (ctx.debugMessenger) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) func(ctx.instance, ctx.debugMessenger, nullptr);
    }
    if (ctx.instance) vkDestroyInstance(ctx.instance, nullptr);
}

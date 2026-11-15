#include "vulkan_wayland_renderer.h"
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "egl_renderer.h"
#include "labwc.h"
#include "ahwb_pool.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PLASMA_WAYLAND_HAS_VULKAN
#define PLASMA_WAYLAND_HAS_VULKAN 0
#endif

#define VKW_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlasmaWaylandVk", __VA_ARGS__)
#define VKW_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PlasmaWaylandVk", __VA_ARGS__)
#define VKW_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlasmaWaylandVk", __VA_ARGS__)

#if PLASMA_WAYLAND_HAS_VULKAN
#include "wayland_vulkan_shaders.hpp"
#include <sys/stat.h>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XBGR8888 0x34324258u
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1ULL)
#endif

namespace {

static bool hasExt(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data()) != VK_SUCCESS) return false;
    for (const auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static bool hasInstanceExt(const char* name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) != VK_SUCCESS) return false;
    for (const auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

struct QuadPush {
    float rect[4];
    float extent[2];
    float uvScale[2];
    float opacity;
    float padding;
};
static_assert(sizeof(QuadPush) == 40);

struct ImageRecord {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    AHardwareBuffer* ahb = nullptr;
    wl_resource* bufferResource = nullptr;
    uint64_t contentGeneration = 0;
    uint64_t bufferGeneration = 0;
    uint64_t lastUsedFrame = 0;
    int width = 0;
    int height = 0;
    bool external = false;
    bool yInvert = false;
    bool opaqueShm = false;
    bool keyed = false;
    dev_t keyDev = 0;
    ino_t keyIno = 0;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingPtr = nullptr;
    VkDeviceSize stagingSize = 0;
    bool stagingCoherent = true;
};

}

struct VulkanWaylandOutput::Impl {
    ANativeWindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::string gpuName;

    bool ahbImport = false;
    bool dmaBufImport = false;
    bool foreignQueue = false;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR swapPre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR swapSeen = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    std::unordered_map<Surface*, ImageRecord> images;
    std::map<std::pair<dev_t, ino_t>, ImageRecord> dmabufCache;
    static constexpr size_t kDmabufCacheMax = 8;
    uint64_t frameNumber = 0;
    bool resizePending = false;
    bool initialized = false;
    GpuTracker* gpuTracker = nullptr;
    GpuTimeline timeline;
    bool timelineRegistered = false;

    static bool probeTimeline(void* ctx) {
        auto* self = static_cast<Impl*>(ctx);
        return self->device && self->inFlight &&
               vkGetFenceStatus(self->device, self->inFlight) == VK_SUCCESS;
    }

    void registerTimeline() {
        if (!gpuTracker || timelineRegistered) return;
        timeline.ctx = this;
        timeline.probe = &Impl::probeTimeline;
        gpuTracker->timelines.push_back(&timeline);
        timelineRegistered = true;
    }

    void unregisterTimeline() {
        if (!gpuTracker || !timelineRegistered) return;
        auto& v = gpuTracker->timelines;
        v.erase(std::remove(v.begin(), v.end(), &timeline), v.end());
        timelineRegistered = false;
    }

    bool check(VkResult r, const char* op) {
        if (r == VK_SUCCESS) return true;
        VKW_LOGW("%s: VkResult=%d", op, static_cast<int>(r));
        return false;
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted,
                        VkMemoryPropertyFlags* actual = nullptr) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & wanted) == wanted) {
                if (actual) *actual = mp.memoryTypes[i].propertyFlags;
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool init(ANativeWindow* nativeWindow, int width, int height) {
        if (!nativeWindow) return false;
        window = nativeWindow;
        ANativeWindow_acquire(window);
        if (!createInstanceAndSurface() || !selectDevice() || !createDevice() ||
            !createSwapchain() || !createDescriptors() || !createPipeline() ||
            !createCommands()) {
            teardown();
            return false;
        }
        (void)width; (void)height;
        initialized = true;
        VKW_LOGI("Vulkan Wayland output ativo: %s (%ux%u, AHB=%d DMA-BUF=%d)",
                 gpuName.c_str(), extent.width, extent.height, ahbImport, dmaBufImport);
        return true;
    }

    bool createInstanceAndSurface() {
        if (!hasInstanceExt(VK_KHR_SURFACE_EXTENSION_NAME) ||
            !hasInstanceExt(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) return false;
        const char* exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "PlasmaDroid Wayland";
        app.applicationVersion = 1;
        app.pEngineName = "PlasmaDroid Vulkan scene renderer";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = exts;
        if (!check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance")) return false;
        VkAndroidSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        sci.window = window;
        return check(vkCreateAndroidSurfaceKHR(instance, &sci, nullptr, &surface),
                     "vkCreateAndroidSurfaceKHR");
    }

    bool selectDevice() {
        uint32_t n = 0;
        if (!check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "enumerate GPUs") || n == 0) return false;
        std::vector<VkPhysicalDevice> devices(n);
        if (!check(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "enumerate GPU list")) return false;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(candidate, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                VKW_LOGW("ignoring CPU Vulkan device: %s", p.deviceName);
                continue;
            }
            if (!hasExt(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qn, queues.data());
            for (uint32_t q = 0; q < qn; ++q) {
                if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 present = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, surface, &present) != VK_SUCCESS || !present) continue;
                if (VK_VERSION_MAJOR(p.apiVersion) < 1 ||
                    (VK_VERSION_MAJOR(p.apiVersion) == 1 && VK_VERSION_MINOR(p.apiVersion) < 1)) continue;
                physical = candidate;
                queueFamily = q;
                gpuName = p.deviceName;
                return true;
            }
        }
        return false;
    }

    bool createDevice() {
        std::vector<const char*> exts;
        auto addExtension = [&exts](const char* name) {
            for (const char* existing : exts)
                if (std::strcmp(existing, name) == 0) return;
            exts.push_back(name);
        };
        addExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
        ahbImport = hasExt(physical, "VK_ANDROID_external_memory_android_hardware_buffer") &&
                    hasExt(physical, "VK_EXT_queue_family_foreign");
        if (ahbImport) {
            addExtension("VK_ANDROID_external_memory_android_hardware_buffer");
            addExtension("VK_EXT_queue_family_foreign");
        }
#endif
#if defined(VK_EXT_external_memory_dma_buf) && defined(VK_EXT_image_drm_format_modifier)
        dmaBufImport = hasExt(physical, "VK_EXT_external_memory_dma_buf") &&
                       hasExt(physical, "VK_EXT_image_drm_format_modifier") &&
                       hasExt(physical, "VK_KHR_external_memory_fd") &&
                       hasExt(physical, "VK_EXT_queue_family_foreign");
        if (dmaBufImport) {
            addExtension("VK_EXT_external_memory_dma_buf");
            addExtension("VK_EXT_image_drm_format_modifier");
            addExtension("VK_KHR_external_memory_fd");
            addExtension("VK_EXT_queue_family_foreign");
        }
#endif
#ifdef VK_EXT_queue_family_foreign
        foreignQueue = hasExt(physical, "VK_EXT_queue_family_foreign") &&
                       (ahbImport || dmaBufImport);
#endif
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        ci.pEnabledFeatures = &features;
        if (!check(vkCreateDevice(physical, &ci, nullptr, &device), "vkCreateDevice")) return false;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!queue) return false;
        if (ahbImport) {
            getAhbProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
            if (!getAhbProperties) ahbImport = false;
        }
        if (dmaBufImport) {
            getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
                vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
            if (!getMemoryFdProperties) dmaBufImport = false;
        }
        return true;
    }

    bool supportsAhbImageFormat(VkFormat format) {
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
        VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
        externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkPhysicalDeviceImageFormatInfo2 imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        imageInfo.pNext = &externalInfo;
        imageInfo.format = format;
        imageInfo.type = VK_IMAGE_TYPE_2D;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties externalProperties{};
        externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        properties.pNext = &externalProperties;
        if (vkGetPhysicalDeviceImageFormatProperties2(physical, &imageInfo, &properties) != VK_SUCCESS)
            return false;
        const auto& memory = externalProperties.externalMemoryProperties;
        return (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
               (memory.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) != 0;
#else
        (void)format;
        return false;
#endif
    }

    bool supportsDmaBufModifier(VkFormat format, uint64_t modifier) {
#if defined(VK_EXT_external_memory_dma_buf) && defined(VK_EXT_image_drm_format_modifier)
        VkDrmFormatModifierPropertiesListEXT modifierList{};
        modifierList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 formatProperties{};
        formatProperties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        formatProperties.pNext = &modifierList;
        vkGetPhysicalDeviceFormatProperties2(physical, format, &formatProperties);
        if (modifierList.drmFormatModifierCount == 0) return false;
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(modifierList.drmFormatModifierCount);
        modifierList.pDrmFormatModifierProperties = modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(physical, format, &formatProperties);
        auto found = std::find_if(modifiers.begin(), modifiers.end(), [modifier](const auto& item) {
            return item.drmFormatModifier == modifier && item.drmFormatModifierPlaneCount == 1 &&
                   (item.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        });
        if (found == modifiers.end()) return false;

        VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
        externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{};
        modifierInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modifierInfo.pNext = &externalInfo;
        modifierInfo.drmFormatModifier = modifier;
        modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceImageFormatInfo2 imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        imageInfo.pNext = &modifierInfo;
        imageInfo.format = format;
        imageInfo.type = VK_IMAGE_TYPE_2D;
        imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties externalProperties{};
        externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        properties.pNext = &externalProperties;
        if (vkGetPhysicalDeviceImageFormatProperties2(physical, &imageInfo, &properties) != VK_SUCCESS)
            return false;
        const auto& memory = externalProperties.externalMemoryProperties;
        return (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
               (memory.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
#else
        (void)format; (void)modifier;
        return false;
#endif
    }

    bool createSwapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities")) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            VKW_LOGW("Android surface does not support color-attachment swapchain images");
            return false;
        }
        uint32_t count = 0;
        if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "surface formats count") || !count) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "surface formats")) return false;
        VkSurfaceFormatKHR chosen = formats.front();
        if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
            chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
            chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        } else {
            auto preferred = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
                return f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (preferred == formats.end()) {
                preferred = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
                    return f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
            }
            if (preferred != formats.end()) chosen = *preferred;
        }
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        else {
            const int nativeWidth = ANativeWindow_getWidth(window);
            const int nativeHeight = ANativeWindow_getHeight(window);
            if (nativeWidth <= 0 || nativeHeight <= 0) return false;
            extent.width = std::clamp<uint32_t>(static_cast<uint32_t>(nativeWidth), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(static_cast<uint32_t>(nativeHeight), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        VkSurfaceTransformFlagBitsKHR preTransform = caps.currentTransform;
        {
            const char* pt = getenv("PLASMA_VK_PRETRANSFORM");
            const bool keepNative = pt && std::string(pt) == "native";
            if (!keepNative && preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
                (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
                if (caps.currentExtent.width != UINT32_MAX && transformSwapsAxes(caps.currentTransform))
                    std::swap(extent.width, extent.height);
                preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            }
        }
        swapPre = preTransform;
        swapSeen = caps.currentTransform;
        if (extent.width == 0 || extent.height == 0) return false;
        uint32_t imageCount = std::max<uint32_t>(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
        VkPresentModeKHR plasmaPresent = VK_PRESENT_MODE_FIFO_KHR;
        {
            const char* force = getenv("PLASMA_VK_PRESENT");
            uint32_t pmCount = 0;
            if (!(force && std::string(force) == "fifo") &&
                vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &pmCount, nullptr) == VK_SUCCESS && pmCount) {
                std::vector<VkPresentModeKHR> pms(pmCount);
                if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &pmCount, pms.data()) == VK_SUCCESS &&
                    std::find(pms.begin(), pms.end(), VK_PRESENT_MODE_MAILBOX_KHR) != pms.end())
                    plasmaPresent = VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        const VkCompositeAlphaFlagBitsKHR alphas[] = { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR };
        for (auto a : alphas) if (caps.supportedCompositeAlpha & a) { alpha = a; break; }
        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface;
        ci.minImageCount = imageCount;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = preTransform;
        ci.compositeAlpha = alpha;
        ci.presentMode = plasmaPresent;
        ci.clipped = VK_TRUE;
        if (!check(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain), "create swapchain")) return false;
        swapFormat = chosen.format;
        if (!check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr), "swapchain images count")) return false;
        swapImages.resize(imageCount);
        if (!check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapImages.data()), "swapchain images")) return false;
        swapViews.resize(imageCount, VK_NULL_HANDLE);
        framebuffers.resize(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swapImages[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapFormat;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (!check(vkCreateImageView(device, &vi, nullptr, &swapViews[i]), "swap image view")) return false;
        }
        VkAttachmentDescription attachment{};
        attachment.format = swapFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ri.attachmentCount = 1;
        ri.pAttachments = &attachment;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = 1;
        ri.pDependencies = &dep;
        if (!check(vkCreateRenderPass(device, &ri, nullptr, &renderPass), "create render pass")) return false;
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = renderPass;
            fi.attachmentCount = 1;
            fi.pAttachments = &swapViews[i];
            fi.width = extent.width;
            fi.height = extent.height;
            fi.layers = 1;
            if (!check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffers[i]), "create framebuffer")) return false;
        }
        if (!check(vkCreateSemaphore(device, &si, nullptr, &imageAvailable), "create image semaphore")) return false;
        if (!check(vkCreateSemaphore(device, &si, nullptr, &renderFinished), "create render semaphore")) return false;
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateFence(device, &fi, nullptr, &inFlight), "create frame fence")) return false;
        return true;
    }

    bool createDescriptors() {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        if (!check(vkCreateSampler(device, &si, nullptr, &sampler), "create sampler")) return false;
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device, &li, nullptr, &descriptorLayout), "descriptor layout")) return false;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 512;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        return check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptorPool), "descriptor pool");
    }

    VkShaderModule makeShader(const uint32_t* data, size_t bytes) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = bytes;
        ci.pCode = data;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
        return module;
    }

    bool createPipeline() {
        VkShaderModule vert = makeShader(plasma_spv::kWaylandQuadVert, plasma_spv::kWaylandQuadVertSize);
        VkShaderModule frag = makeShader(plasma_spv::kWaylandQuadFrag, plasma_spv::kWaylandQuadFragSize);
        if (!vert || !frag) {
            if (vert) vkDestroyShaderModule(device, vert, nullptr);
            if (frag) vkDestroyShaderModule(device, frag, nullptr);
            return false;
        }
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(QuadPush);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &descriptorLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        bool ok = check(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout), "pipeline layout");
        if (ok) {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ba{};
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
            ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &ba;
            VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;
            VkGraphicsPipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            ci.stageCount = 2;
            ci.pStages = stages;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &dyn;
            ci.layout = pipelineLayout;
            ci.renderPass = renderPass;
            ci.subpass = 0;
            ok = check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), "graphics pipeline");
        }
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return ok;
    }

    bool createCommands() {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queueFamily;
        if (!check(vkCreateCommandPool(device, &pi, nullptr, &commandPool), "command pool")) return false;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        return check(vkAllocateCommandBuffers(device, &ai, &commandBuffer), "command buffer");
    }

    void destroyImage(ImageRecord& r) {
        if (!device) return;
        if (r.descriptor && descriptorPool) vkFreeDescriptorSets(device, descriptorPool, 1, &r.descriptor);
        if (r.view) vkDestroyImageView(device, r.view, nullptr);
        if (r.image) vkDestroyImage(device, r.image, nullptr);
        if (r.memory) vkFreeMemory(device, r.memory, nullptr);
        if (r.ahb) AHardwareBuffer_release(r.ahb);
        if (r.stagingPtr) vkUnmapMemory(device, r.stagingMem);
        if (r.stagingBuf) vkDestroyBuffer(device, r.stagingBuf, nullptr);
        if (r.stagingMem) vkFreeMemory(device, r.stagingMem, nullptr);
        r = {};
    }

    void parkRecord(ImageRecord& r) {
        if (!r.keyed) { vkDeviceWaitIdle(device); destroyImage(r); return; }
        const auto key = std::make_pair(r.keyDev, r.keyIno);
        if (dmabufCache.count(key)) { vkDeviceWaitIdle(device); destroyImage(r); return; }
        if (dmabufCache.size() >= kDmabufCacheMax) {
            auto oldest = dmabufCache.begin();
            for (auto it = dmabufCache.begin(); it != dmabufCache.end(); ++it)
                if (it->second.lastUsedFrame < oldest->second.lastUsedFrame) oldest = it;
            vkDeviceWaitIdle(device);
            destroyImage(oldest->second);
            dmabufCache.erase(oldest);
        }
        dmabufCache.emplace(key, r);
        r = {};
    }

    bool createViewAndDescriptor(ImageRecord& r) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = r.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = r.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (r.opaqueShm) vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        if (!check(vkCreateImageView(device, &vi, nullptr, &r.view), "client image view")) return false;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &descriptorLayout;
        if (!check(vkAllocateDescriptorSets(device, &ai, &r.descriptor), "client descriptor set")) return false;
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sampler;
        imageInfo.imageView = r.view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = r.descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return true;
    }

    bool createShmImage(ImageRecord& r, int width, int height, VkFormat format) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &ii, nullptr, &r.image), "create SHM image")) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, r.image, &req);
        uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = memoryType(req.memoryTypeBits, 0);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(device, &ai, nullptr, &r.memory), "allocate SHM image")) return false;
        if (!check(vkBindImageMemory(device, r.image, r.memory, 0), "bind SHM image")) return false;
        r.width = width; r.height = height; r.format = format; r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return createViewAndDescriptor(r);
    }

    bool oneShot(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &ai, &cmd), "allocate one-shot command")) return false;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        bool ok = vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
        if (ok) { record(cmd); ok = vkEndCommandBuffer(cmd) == VK_SUCCESS; }
        if (ok) {
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            ok = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
        }
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
        return ok;
    }

    struct PendingUpload { Surface* key; VkRect2D region; };
    std::vector<PendingUpload> pendingUploads;

    bool ensureStaging(ImageRecord& r, VkDeviceSize bytes) {
        if (r.stagingBuf && r.stagingSize >= bytes) return true;
        if (r.stagingPtr) { vkUnmapMemory(device, r.stagingMem); r.stagingPtr = nullptr; }
        if (r.stagingBuf) { vkDestroyBuffer(device, r.stagingBuf, nullptr); r.stagingBuf = VK_NULL_HANDLE; }
        if (r.stagingMem) { vkFreeMemory(device, r.stagingMem, nullptr); r.stagingMem = VK_NULL_HANDLE; }
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device, &bi, nullptr, &r.stagingBuf), "create SHM staging")) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, r.stagingBuf, &req);
        VkMemoryPropertyFlags actual = 0;
        uint32_t mt = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &actual);
        if (mt == UINT32_MAX) mt = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &actual);
        if (mt == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mt;
        if (!check(vkAllocateMemory(device, &mai, nullptr, &r.stagingMem), "allocate SHM staging")) return false;
        if (!check(vkBindBufferMemory(device, r.stagingBuf, r.stagingMem, 0), "bind SHM staging")) return false;
        if (!check(vkMapMemory(device, r.stagingMem, 0, VK_WHOLE_SIZE, 0, &r.stagingPtr), "map SHM staging")) return false;
        r.stagingSize = bytes;
        r.stagingCoherent = (actual & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return true;
    }

    bool uploadShm(ImageRecord& r, Surface* s) {
        wl_shm_buffer* shm = s && s->current_buffer ? wl_shm_buffer_get(s->current_buffer) : nullptr;
        if (!shm) return false;
        const int width = wl_shm_buffer_get_width(shm);
        const int height = wl_shm_buffer_get_height(shm);
        const int stride = wl_shm_buffer_get_stride(shm);
        const uint32_t fmt = wl_shm_buffer_get_format(shm);
        VkFormat vkfmt = VK_FORMAT_UNDEFINED;
        bool opaque = false;
        switch (fmt) {
            case WL_SHM_FORMAT_ARGB8888: vkfmt = VK_FORMAT_B8G8R8A8_UNORM; break;
            case WL_SHM_FORMAT_XRGB8888: vkfmt = VK_FORMAT_B8G8R8A8_UNORM; opaque = true; break;
            case WL_SHM_FORMAT_ABGR8888: vkfmt = VK_FORMAT_R8G8B8A8_UNORM; break;
            case WL_SHM_FORMAT_XBGR8888: vkfmt = VK_FORMAT_R8G8B8A8_UNORM; opaque = true; break;
#ifdef WL_SHM_FORMAT_RGBA8888
            case WL_SHM_FORMAT_RGBA8888: vkfmt = VK_FORMAT_R8G8B8A8_UNORM; break;
            case WL_SHM_FORMAT_RGBX8888: vkfmt = VK_FORMAT_R8G8B8A8_UNORM; opaque = true; break;
#endif
            default: return false;
        }
        if (width <= 0 || height <= 0 || stride < width * 4 || width > 16384 || height > 16384) return false;
        const bool recreated = !r.image || r.width != width || r.height != height || r.format != vkfmt ||
                               r.opaqueShm != opaque;
        if (recreated) {
            if (r.image) { vkDeviceWaitIdle(device); destroyImage(r); }
            r.opaqueShm = opaque;
            if (!createShmImage(r, width, height, vkfmt)) return false;
        }

        int ux = 0, uy = 0, uw = width, uh = height;
        if (!recreated && !s->damage_surface.empty() && s->current.buffer_scale <= 1 &&
            s->current.buffer_transform == 0 && s->current.src_w <= 0 && s->current.dst_w <= 0) {
            int x1 = width, y1 = height, x2 = 0, y2 = 0;
            for (const auto& d : s->damage_surface) {
                if (d.empty()) continue;
                x1 = std::min(x1, std::max(0, d.x)); y1 = std::min(y1, std::max(0, d.y));
                x2 = std::max(x2, std::min(width, d.x + d.w)); y2 = std::max(y2, std::min(height, d.y + d.h));
            }
            if (x2 > x1 && y2 > y1) { ux = x1; uy = y1; uw = x2 - x1; uh = y2 - y1; }
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
        if (!ensureStaging(r, bytes)) return false;
        wl_shm_buffer_begin_access(shm);
        const auto* src = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
        if (!src) { wl_shm_buffer_end_access(shm); return false; }
        auto* dstBase = static_cast<uint8_t*>(r.stagingPtr);
        for (int y = uy; y < uy + uh; ++y) {
            uint8_t* dst = dstBase + (static_cast<size_t>(y) * width + ux) * 4;
            std::memcpy(dst, src + static_cast<size_t>(y) * stride + static_cast<size_t>(ux) * 4, static_cast<size_t>(uw) * 4);
        }
        wl_shm_buffer_end_access(shm);
        if (!r.stagingCoherent) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = r.stagingMem;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            if (!check(vkFlushMappedMemoryRanges(device, 1, &range), "flush SHM staging")) return false;
        }
        pendingUploads.push_back({s, VkRect2D{{ux, uy}, {static_cast<uint32_t>(uw), static_cast<uint32_t>(uh)}}});
        r.bufferResource = s->current_buffer;
        r.contentGeneration = s->content_generation;
        r.bufferGeneration = s->buffer_generation;
        r.width = width; r.height = height;
        return true;
    }

    void recordUploads(VkCommandBuffer cmd) {
        for (const auto& up : pendingUploads) {
            auto it = images.find(up.key);
            if (it == images.end()) continue;
            ImageRecord& r = it->second;
            if (!r.image || !r.stagingBuf) continue;
            const VkImageLayout oldLayout = r.layout;
            VkImageMemoryBarrier toDst{};
            toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toDst.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT : 0;
            toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.oldLayout = oldLayout;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image = r.image;
            toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
            VkBufferImageCopy copy{};
            copy.bufferOffset = (static_cast<VkDeviceSize>(up.region.offset.y) * r.width + up.region.offset.x) * 4;
            copy.bufferRowLength = static_cast<uint32_t>(r.width);
            copy.bufferImageHeight = static_cast<uint32_t>(r.height);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {up.region.offset.x, up.region.offset.y, 0};
            copy.imageExtent = {up.region.extent.width, up.region.extent.height, 1};
            vkCmdCopyBufferToImage(cmd, r.stagingBuf, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            VkImageMemoryBarrier toRead{};
            toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.image = r.image;
            toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
            r.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        pendingUploads.clear();
    }

    bool importAhb(ImageRecord& r, Surface* s, AHardwareBuffer* ahb) {
#if defined(VK_ANDROID_external_memory_android_hardware_buffer)
        if (!ahbImport || !getAhbProperties || !ahb || !s) return false;
        VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{};
        formatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
        VkAndroidHardwareBufferPropertiesANDROID props{};
        props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        props.pNext = &formatProps;
        if (!check(getAhbProperties(device, ahb, &props), "AHB properties")) return false;
        if (formatProps.externalFormat != 0 || formatProps.format == VK_FORMAT_UNDEFINED) return false;
        if (!(formatProps.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !supportsAhbImageFormat(formatProps.format)) return false;
        VkExternalMemoryImageCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.pNext = &ext;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = formatProps.format;
        AHardwareBuffer_Desc bufferDesc{};
        AHardwareBuffer_describe(ahb, &bufferDesc);
        if (bufferDesc.width == 0 || bufferDesc.height == 0) return false;
        ii.extent = {bufferDesc.width, bufferDesc.height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &ii, nullptr, &r.image), "create AHB image")) return false;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = r.image;
        VkImportAndroidHardwareBufferInfoANDROID importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        importInfo.pNext = &dedicated;
        importInfo.buffer = ahb;
        uint32_t type = UINT32_MAX;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, r.image, &req);
        const uint32_t bits = props.memoryTypeBits & req.memoryTypeBits;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if (bits & (1u << i)) { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &importInfo;
        ai.allocationSize = props.allocationSize;
        ai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(device, &ai, nullptr, &r.memory), "import AHB memory")) return false;
        if (!check(vkBindImageMemory(device, r.image, r.memory, 0), "bind AHB image")) return false;
        r.format = formatProps.format;
        r.width = static_cast<int>(bufferDesc.width);
        r.height = static_cast<int>(bufferDesc.height);
        r.external = true;
        r.layout = VK_IMAGE_LAYOUT_GENERAL;
        r.bufferResource = s->current_buffer;
        r.contentGeneration = s->content_generation;
        r.bufferGeneration = s->buffer_generation;
        r.yInvert = (s->dmabuf.flags & 1u) != 0;
        if (!createViewAndDescriptor(r)) return false;
        r.ahb = ahb;
        return true;
#else
        (void)r; (void)s; (void)ahb;
        return false;
#endif
    }

    VkFormat drmFormat(uint32_t fourcc) const {
        switch (fourcc) {
            case DRM_FORMAT_ARGB8888:
            case DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
            case DRM_FORMAT_ABGR8888:
            case DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    bool importDmaBuf(ImageRecord& r, Surface* s) {
#if defined(VK_EXT_external_memory_dma_buf) && defined(VK_EXT_image_drm_format_modifier)
        if (!dmaBufImport || !foreignQueue || !s || s->dmabuf.fds.size() != 1 ||
            s->dmabuf.offsets.size() != 1 || s->dmabuf.strides.size() != 1 ||
            s->dmabuf.modifier == DRM_FORMAT_MOD_INVALID) return false;
        const VkFormat format = drmFormat(s->dmabuf.fourcc);
        if (format == VK_FORMAT_UNDEFINED || s->dmabuf.width <= 0 || s->dmabuf.height <= 0 ||
            !supportsDmaBufModifier(format, s->dmabuf.modifier)) return false;
        const int srcFd = s->dmabuf.fds[0];
        if (srcFd < 0) return false;
        const int fd = dup(srcFd);
        if (fd < 0) return false;
        VkSubresourceLayout plane{};
        plane.offset = s->dmabuf.offsets[0];
        plane.size = 0;
        plane.rowPitch = s->dmabuf.strides[0];
        plane.arrayPitch = 0;
        plane.depthPitch = 0;
        VkExternalMemoryImageCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{};
        modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modifier.pNext = &ext;
        modifier.drmFormatModifier = s->dmabuf.modifier;
        modifier.drmFormatModifierPlaneCount = 1;
        modifier.pPlaneLayouts = &plane;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.pNext = &modifier;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {static_cast<uint32_t>(s->dmabuf.width), static_cast<uint32_t>(s->dmabuf.height), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &ii, nullptr, &r.image), "create DMA-BUF image")) { close(fd); return false; }
        VkMemoryFdPropertiesKHR fdProps{};
        fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (!check(getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps), "DMA-BUF fd properties")) {
            close(fd); return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, r.image, &req);
        uint32_t type = memoryType(req.memoryTypeBits & fdProps.memoryTypeBits, 0);
        if (type == UINT32_MAX) { close(fd); return false; }
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = r.image;
        VkImportMemoryFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import.fd = fd;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &import;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(device, &ai, nullptr, &r.memory), "import DMA-BUF memory")) { close(fd); return false; }
        if (!check(vkBindImageMemory(device, r.image, r.memory, 0), "bind DMA-BUF image")) return false;
        r.format = format;
        r.width = s->dmabuf.width;
        r.height = s->dmabuf.height;
        r.external = true;
        r.layout = VK_IMAGE_LAYOUT_GENERAL;
        r.bufferResource = s->current_buffer;
        r.contentGeneration = s->content_generation;
        r.bufferGeneration = s->buffer_generation;
        r.yInvert = (s->dmabuf.flags & 1u) != 0;
        return createViewAndDescriptor(r);
#else
        (void)r; (void)s;
        return false;
#endif
    }

    bool ensureImage(Surface* s, ImageRecord*& out) {
        if (!s || !s->current_buffer) return false;
        ImageRecord& r = images[s];
        const bool sameBuffer = r.image && r.bufferResource == s->current_buffer;
        if (sameBuffer && s->buffer_kind == BufferKind::Shm && r.contentGeneration == s->content_generation) {
            r.lastUsedFrame = frameNumber;
            out = &r;
            return true;
        }
        const bool newExternalBinding = s->buffer_kind == BufferKind::Dmabuf &&
            r.bufferGeneration != s->buffer_generation;
        if (!sameBuffer || newExternalBinding) {
            bool keyValid = false;
            std::pair<dev_t, ino_t> key{};
            if (s->buffer_kind == BufferKind::Dmabuf && !s->dmabuf.fds.empty()) {
                struct stat fst{};
                if (fstat(s->dmabuf.fds[0], &fst) == 0) {
                    key = {fst.st_dev, fst.st_ino};
                    keyValid = true;
                }
            }
            if (keyValid) {
                auto adopt = [&]() {
                    r.bufferResource = s->current_buffer;
                    r.bufferGeneration = s->buffer_generation;
                    r.contentGeneration = s->content_generation;
                    r.lastUsedFrame = frameNumber;
                    out = &r;
                    return true;
                };
                if (r.image && r.keyed && r.keyDev == key.first && r.keyIno == key.second)
                    return adopt();
                auto hit = dmabufCache.find(key);
                if (hit != dmabufCache.end()) {
                    ImageRecord cached = hit->second;
                    dmabufCache.erase(hit);
                    if (r.image) parkRecord(r);
                    r = cached;
                    return adopt();
                }
            }
            if (r.image && (s->buffer_kind != BufferKind::Shm || r.external || r.ahb || r.keyed)) parkRecord(r);
            bool imported = false;
            if (s->buffer_kind == BufferKind::Shm) imported = uploadShm(r, s);
            else if (s->buffer_kind == BufferKind::Dmabuf) {
                AHardwareBuffer* ahb = !s->dmabuf.fds.empty()
                    ? ahwb_acquire_from_fd(s->dmabuf.fds[0]) : nullptr;
                if (ahb) {
                    imported = importAhb(r, s, ahb);
                    if (!imported) AHardwareBuffer_release(ahb);
                }
                if (!imported && r.image) destroyImage(r);
                if (!imported) imported = importDmaBuf(r, s);
            }
            if (imported && keyValid) { r.keyed = true; r.keyDev = key.first; r.keyIno = key.second; }
            if (!imported) { if (r.image) destroyImage(r); return false; }
        } else if (s->buffer_kind == BufferKind::Shm && r.contentGeneration != s->content_generation) {
            if (!uploadShm(r, s)) return false;
        }
        r.lastUsedFrame = frameNumber;
        out = &r;
        return true;
    }

    void barrierExternal(VkCommandBuffer cmd, ImageRecord& r, bool acquire) {
        if (!r.external) return;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0;
        b.oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (foreignQueue) {
            b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : queueFamily;
            b.dstQueueFamilyIndex = acquire ? queueFamily : VK_QUEUE_FAMILY_FOREIGN_EXT;
        } else b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
        r.layout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    }

    static bool transformSwapsAxes(VkSurfaceTransformFlagBitsKHR t) {
        return (t & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
                     VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
                     VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR)) != 0;
    }

    bool swapchainStale() {
        VkSurfaceCapabilitiesKHR c{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &c) != VK_SUCCESS) return true;
        if (c.currentTransform != swapSeen) return true;
        if (c.currentExtent.width != UINT32_MAX) {
            VkExtent2D e = c.currentExtent;
            if (swapPre == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && transformSwapsAxes(c.currentTransform)) std::swap(e.width, e.height);
            if (e.width != extent.width || e.height != extent.height) return true;
        }
        return swapPre == c.currentTransform;
    }

    bool recreateSwapchain() {
        if (!device) return false;
        if (vkDeviceWaitIdle(device) != VK_SUCCESS) {
            initialized = false;
            return false;
        }
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
        for (auto fb : framebuffers) if (fb) vkDestroyFramebuffer(device, fb, nullptr);
        if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
        for (auto view : swapViews) if (view) vkDestroyImageView(device, view, nullptr);
        framebuffers.clear(); swapViews.clear(); swapImages.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        if (imageAvailable) vkDestroySemaphore(device, imageAvailable, nullptr);
        if (renderFinished) vkDestroySemaphore(device, renderFinished, nullptr);
        if (inFlight) vkDestroyFence(device, inFlight, nullptr);
        imageAvailable = renderFinished = VK_NULL_HANDLE; inFlight = VK_NULL_HANDLE;
        return createSwapchain() && createPipeline();
    }

    bool render(const std::vector<RenderItem>& items, int64_t ) {
        if (!initialized || !device || !swapchain) return false;
        if (resizePending) {
            resizePending = false;
            if (!recreateSwapchain()) return false;
        }
        std::vector<std::pair<const RenderItem*, ImageRecord*>> drawItems;
        drawItems.reserve(items.size());
        pendingUploads.clear();
        struct UploadGuard {
            Impl* self;
            ~UploadGuard() {
                for (const auto& up : self->pendingUploads) {
                    auto it = self->images.find(up.key);
                    if (it != self->images.end()) { it->second.contentGeneration = ~0ull; it->second.bufferResource = nullptr; }
                }
                self->pendingUploads.clear();
            }
        } uploadGuard{this};
        ++frameNumber;
        registerTimeline();
        if (!check(vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX), "wait frame fence")) {
            initialized = false;
            return false;
        }
        timeline.done = timeline.submitted;
        for (const auto& item : items) {
            if (!item.surface || item.w <= 0 || item.h <= 0 || !item.surface->current_buffer) continue;
            ImageRecord* image = nullptr;
            if (!ensureImage(item.surface, image) || !image || !image->descriptor) return false;
            drawItems.emplace_back(&item, image);
        }
        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                  imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resizePending = false;
            if (!recreateSwapchain()) return false;
            acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             imageAvailable, VK_NULL_HANDLE, &imageIndex);
        }
        if (acquire == VK_SUBOPTIMAL_KHR) {
            if (swapchainStale()) resizePending = true;
        } else if (acquire != VK_SUCCESS) {
            if (acquire == VK_ERROR_DEVICE_LOST) initialized = false;
            return false;
        }
        if (!check(vkResetCommandBuffer(commandBuffer, 0), "reset frame command")) return false;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commandBuffer, &bi), "begin frame command")) return false;
        recordUploads(commandBuffer);
        for (auto& draw : drawItems) barrierExternal(commandBuffer, *draw.second, true);
        VkClearValue clear{};
        clear.color = {{0.045f, 0.052f, 0.065f, 1.0f}};
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = renderPass;
        rp.framebuffer = framebuffers[imageIndex];
        rp.renderArea = {{0, 0}, extent};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.x = 0.f; viewport.y = 0.f;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.f; viewport.maxDepth = 1.f;
        VkRect2D fullScissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (auto& draw : drawItems) {
            const RenderItem& item = *draw.first;
            ImageRecord& image = *draw.second;
            const int left = std::max(0, item.x);
            const int top = std::max(0, item.y);
            const int right = std::min(static_cast<int>(extent.width), item.x + item.w);
            const int bottom = std::min(static_cast<int>(extent.height), item.y + item.h);
            if (right <= left || bottom <= top) continue;
            VkRect2D scissor{};
            scissor.offset = {left, top};
            scissor.extent = {static_cast<uint32_t>(right-left), static_cast<uint32_t>(bottom-top)};
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    0, 1, &image.descriptor, 0, nullptr);
            QuadPush push{};
            push.rect[0] = static_cast<float>(item.x);
            push.rect[1] = static_cast<float>(item.y);
            push.rect[2] = static_cast<float>(item.w);
            push.rect[3] = static_cast<float>(item.h);
            push.extent[0] = static_cast<float>(extent.width);
            push.extent[1] = static_cast<float>(extent.height);
            push.uvScale[0] = 1.f;
            push.uvScale[1] = image.yInvert ? 1.f : 0.f;
            push.opacity = 1.f;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            vkCmdDraw(commandBuffer, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(commandBuffer);
        for (auto& draw : drawItems) barrierExternal(commandBuffer, *draw.second, false);
        if (!check(vkEndCommandBuffer(commandBuffer), "end frame command")) return false;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (!check(vkResetFences(device, 1, &inFlight), "reset frame fence")) return false;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &commandBuffer;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished;
        if (!check(vkQueueSubmit(queue, 1, &si, inFlight), "submit Wayland frame")) {
            initialized = false;
            return false;
        }
        if (gpuTracker) timeline.submitted = ++gpuTracker->serial;
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) resizePending = true;
        else if (pr == VK_SUBOPTIMAL_KHR && swapchainStale()) resizePending = true;
        if (pr == VK_ERROR_DEVICE_LOST) { initialized = false; return false; }
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (resizePending) {
            resizePending = false;
            if (!recreateSwapchain()) return false;
        }
        if ((frameNumber % 180) == 0) {
            for (auto it = dmabufCache.begin(); it != dmabufCache.end();) {
                if (it->second.lastUsedFrame + 180 < frameNumber) {
                    destroyImage(it->second);
                    it = dmabufCache.erase(it);
                } else ++it;
            }
            for (auto it = images.begin(); it != images.end();) {
                if (it->second.lastUsedFrame + 180 < frameNumber) {
                    destroyImage(it->second);
                    it = images.erase(it);
                } else ++it;
            }
        }
        return true;
    }

    void resize(int width, int height) {
        (void)width; (void)height;
        resizePending = true;
    }

    void forgetSurface(Surface* surface) {
        if (!surface || !device) return;
        auto it = images.find(surface);
        if (it == images.end()) return;
        vkDeviceWaitIdle(device);
        destroyImage(it->second);
        images.erase(it);
    }

    void teardown() {
        unregisterTimeline();
        initialized = false;
        if (device) vkDeviceWaitIdle(device);
        for (auto& pair : images) destroyImage(pair.second);
        images.clear();
        for (auto& pair : dmabufCache) destroyImage(pair.second);
        dmabufCache.clear();
        if (device) {
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            for (auto fb : framebuffers) if (fb) vkDestroyFramebuffer(device, fb, nullptr);
            if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
            for (auto view : swapViews) if (view) vkDestroyImageView(device, view, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (imageAvailable) vkDestroySemaphore(device, imageAvailable, nullptr);
            if (renderFinished) vkDestroySemaphore(device, renderFinished, nullptr);
            if (inFlight) vkDestroyFence(device, inFlight, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface && instance) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE; instance = VK_NULL_HANDLE; surface = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE; pipeline = VK_NULL_HANDLE; renderPass = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE; descriptorPool = VK_NULL_HANDLE; descriptorLayout = VK_NULL_HANDLE;
        sampler = VK_NULL_HANDLE; commandPool = VK_NULL_HANDLE; commandBuffer = VK_NULL_HANDLE;
        imageAvailable = renderFinished = VK_NULL_HANDLE; inFlight = VK_NULL_HANDLE;
        framebuffers.clear(); swapViews.clear(); swapImages.clear();
        if (window) { ANativeWindow_release(window); window = nullptr; }
    }
};

VulkanWaylandOutput::VulkanWaylandOutput() : impl_(std::make_unique<Impl>()) {}
VulkanWaylandOutput::~VulkanWaylandOutput() { teardown(); }
bool VulkanWaylandOutput::init(ANativeWindow* w, int width, int height) { return impl_ && impl_->init(w, width, height); }
bool VulkanWaylandOutput::render(const std::vector<RenderItem>& items, int64_t ns) { return impl_ && impl_->render(items, ns); }
void VulkanWaylandOutput::resize(int w, int h) { if (impl_) impl_->resize(w, h); }
void VulkanWaylandOutput::forgetSurface(Surface* s) { if (impl_) impl_->forgetSurface(s); }
void VulkanWaylandOutput::teardown() { if (impl_) impl_->teardown(); }
void VulkanWaylandOutput::setGpuTracker(GpuTracker* t) { if (impl_) impl_->gpuTracker = t; }
bool VulkanWaylandOutput::active() const { return impl_ && impl_->initialized; }
const char* VulkanWaylandOutput::deviceName() const { return impl_ ? impl_->gpuName.c_str() : ""; }

#else

struct VulkanWaylandOutput::Impl { };
VulkanWaylandOutput::VulkanWaylandOutput() = default;
VulkanWaylandOutput::~VulkanWaylandOutput() = default;
bool VulkanWaylandOutput::init(ANativeWindow*, int, int) { return false; }
bool VulkanWaylandOutput::render(const std::vector<RenderItem>&, int64_t) { return false; }
void VulkanWaylandOutput::resize(int, int) {}
void VulkanWaylandOutput::forgetSurface(Surface*) {}
void VulkanWaylandOutput::teardown() {}
void VulkanWaylandOutput::setGpuTracker(GpuTracker*) {}
bool VulkanWaylandOutput::active() const { return false; }
const char* VulkanWaylandOutput::deviceName() const { return ""; }

#endif

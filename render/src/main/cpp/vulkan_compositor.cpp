/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

#define VK_USE_PLATFORM_ANDROID_KHR
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if PLASMA_HAS_VULKAN_COMPOSITOR
#include "vulkan_shaders.hpp"
#endif

#define VKLOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlasmaVulkan", __VA_ARGS__)
#define VKLOGW(...) __android_log_print(ANDROID_LOG_WARN, "PlasmaVulkan", __VA_ARGS__)
#define VKLOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlasmaVulkan", __VA_ARGS__)

#if PLASMA_HAS_VULKAN_COMPOSITOR
namespace {

struct alignas(16) WallpaperPush {
    float resTimeMotion[4];
    float stylePad[4];
    float colors[4][4];
};
struct alignas(16) PresentPush { float resFx[4]; };
static_assert(sizeof(WallpaperPush) == 96);
static_assert(sizeof(PresentPush) == 16);
static_assert(sizeof(float) * 12 == 48);

struct Config {
    int style = 0;
    std::array<float, 16> colors {
        0.015f, 0.039f, 0.141f, 1.f,
        0.051f, 0.169f, 0.420f, 1.f,
        0.102f, 0.522f, 0.969f, 1.f,
        0.549f, 0.380f, 0.980f, 1.f
    };
    bool animated = true;
    int fpsCap = 60;
    bool blur = true;
    float blurStrength = 0.6f;
    float vignette = 0.35f;
    float dim = 0.0f;
};

static bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data()) != VK_SUCCESS) return false;
    for (const auto& prop : props) if (strcmp(prop.extensionName, name) == 0) return true;
    return false;
}

class VulkanCompositor {
public:
    explicit VulkanCompositor(ANativeWindow* nativeWindow) : window(nativeWindow) {}
    ~VulkanCompositor() { shutdown(); }

    bool initialize() {
        if (!window) return fail("ANativeWindow ausente");
        if (!createInstanceAndSurface()) return false;
        if (!selectPhysicalDevice()) return false;
        if (!createDevice()) return false;
        if (!createRenderResources()) return false;
        if (!createCommands()) return false;
        running.store(true);
        renderThread = std::thread(&VulkanCompositor::run, this);
        VKLOGI("Compositor Vulkan iniciado: %s", deviceName.c_str());
        return true;
    }

    void configure(const Config& next) {
        std::lock_guard<std::mutex> guard(stateMutex);
        if (config.style != next.style || config.colors != next.colors || config.animated != next.animated) wallDirty = true;
        config = next;
        dirty = true;
        wake.notify_all();
    }

    void setScene(const std::vector<float>& next) {
        std::lock_guard<std::mutex> guard(stateMutex);
        if (scene == next) return;
        scene = next;
        ++sceneGeneration;
        dirty = true;
        wake.notify_all();
    }

    void resize(int, int) {
        resizePending.store(true);
        std::lock_guard<std::mutex> guard(stateMutex);
        dirty = true;
        wake.notify_all();
    }

    std::string name() const { return deviceName; }
    bool isRunning() const { return running.load(); }

    void shutdown() {
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            running.store(false);
        }
        wake.notify_all();
        if (renderThread.joinable()) renderThread.join();
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        destroyRenderResources();
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        if (window) { ANativeWindow_release(window); window = nullptr; }
    }

private:
    ANativeWindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::string deviceName;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkExtent2D baseExtent{};
    static constexpr float kWallpaperScale = 0.5f;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> swapFramebuffers;
    VkRenderPass swapRenderPass = VK_NULL_HANDLE;
    VkRenderPass baseRenderPass = VK_NULL_HANDLE;
    VkImage baseImage = VK_NULL_HANDLE;
    VkDeviceMemory baseMemory = VK_NULL_HANDLE;
    VkImageView baseView = VK_NULL_HANDLE;
    VkFramebuffer baseFramebuffer = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout wallpaperLayout = VK_NULL_HANDLE;
    VkPipelineLayout sampleLayout = VK_NULL_HANDLE;
    VkPipeline wallpaperPipeline = VK_NULL_HANDLE;
    VkPipeline presentPipeline = VK_NULL_HANDLE;
    VkPipeline glassPipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    std::vector<VkSemaphore> renderFinishedByImage;
    VkFence inFlight = VK_NULL_HANDLE;
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
    void* mappedInstances = nullptr;
    VkDeviceSize instanceCapacity = 0;
    uint32_t instanceCount = 0;
    std::vector<float> instanceData;
    uint64_t sceneGeneration = 1;
    uint64_t uploadedSceneGeneration = 0;
    VkExtent2D uploadedInstanceExtent{};
    bool uploadedBlurEnabled = true;
    std::atomic<bool> running{false};
    std::atomic<bool> resizePending{false};
    std::thread renderThread;
    mutable std::mutex stateMutex;
    std::condition_variable wake;
    Config config;
    std::vector<float> scene;
    bool dirty = true;
    bool wallDirty = true;
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    bool fail(const char* reason) {
        VKLOGE("%s", reason);
        return false;
    }

    bool check(VkResult result, const char* operation) {
        if (result == VK_SUCCESS) return true;
        VKLOGE("%s falhou: VkResult=%d", operation, static_cast<int>(result));
        return false;
    }

    bool createInstanceAndSurface() {
        const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "KDE Plasma Android Desktop";
        app.applicationVersion = 1;
        app.pEngineName = "PlasmaDroid Vulkan Compositor";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = extensions;
        if (!check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance")) return false;
        VkAndroidSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        sci.window = window;
        return check(vkCreateAndroidSurfaceKHR(instance, &sci, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
    }

    bool selectPhysicalDevice() {
        uint32_t count = 0;
        if (!check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)")) return false;
        if (count == 0) return fail("nenhum dispositivo Vulkan");
        std::vector<VkPhysicalDevice> devices(count);
        if (!check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices(list)")) return false;
        for (VkPhysicalDevice candidate : devices) {
            if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, queues.data());
            for (uint32_t q = 0; q < qCount; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, surface, &present);
                if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    VkPhysicalDeviceProperties props{};
                    vkGetPhysicalDeviceProperties(candidate, &props);
                    if (VK_VERSION_MAJOR(props.apiVersion) < 1 ||
                        (VK_VERSION_MAJOR(props.apiVersion) == 1 && VK_VERSION_MINOR(props.apiVersion) < 1)) {
                        continue;
                    }
                    physical = candidate;
                    queueFamily = q;
                    deviceName = props.deviceName;
                    return true;
                }
            }
        }
        return fail("sem fila Vulkan que suporte graphics + present para esta Surface");
    }

    bool createDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = extensions;
        ci.pEnabledFeatures = &features;
        if (!check(vkCreateDevice(physical, &ci, nullptr, &device), "vkCreateDevice")) return false;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        return queue != VK_NULL_HANDLE || fail("vkGetDeviceQueue retornou null");
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & properties) == properties) return i;
        return UINT32_MAX;
    }

    bool createImage(uint32_t width, uint32_t height) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = { width, height, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &ii, nullptr, &baseImage), "vkCreateImage(base)")) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, baseImage, &req);
        uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return fail("sem tipo de memoria device-local para imagem Vulkan");
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(device, &ai, nullptr, &baseMemory), "vkAllocateMemory(base)")) return false;
        if (!check(vkBindImageMemory(device, baseImage, baseMemory, 0), "vkBindImageMemory(base)")) return false;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = baseImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (!check(vkCreateImageView(device, &vi, nullptr, &baseView), "vkCreateImageView(base)")) return false;
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = baseRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &baseView;
        fi.width = width;
        fi.height = height;
        fi.layers = 1;
        return check(vkCreateFramebuffer(device, &fi, nullptr, &baseFramebuffer), "vkCreateFramebuffer(base)");
    }

    bool createBaseImage() {
        baseExtent.width = std::max(1u, static_cast<uint32_t>(extent.width * kWallpaperScale));
        baseExtent.height = std::max(1u, static_cast<uint32_t>(extent.height * kWallpaperScale));
        return createImage(baseExtent.width, baseExtent.height);
    }

    bool createBaseRenderPass() {
        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ri.attachmentCount = 1;
        ri.pAttachments = &attachment;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = static_cast<uint32_t>(deps.size());
        ri.pDependencies = deps.data();
        return check(vkCreateRenderPass(device, &ri, nullptr, &baseRenderPass), "vkCreateRenderPass(base)");
    }

    bool createSwapchainObjects() {
        VkSurfaceCapabilitiesKHR caps{};
        if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return false;
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
            return fail("Surface Vulkan nao suporta COLOR_ATTACHMENT");
        uint32_t formatCount = 0;
        if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)")) return false;
        if (!formatCount) return fail("Surface Vulkan sem formatos");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR(list)")) return false;
        VkSurfaceFormatKHR chosen = formats.front();
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
        }
        if (chosen.format == VK_FORMAT_UNDEFINED) {
            chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
            chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        }
        swapFormat = chosen.format;
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        else {
            uint32_t width = static_cast<uint32_t>(std::max(1, ANativeWindow_getWidth(window)));
            uint32_t height = static_cast<uint32_t>(std::max(1, ANativeWindow_getHeight(window)));
            extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        const VkCompositeAlphaFlagBitsKHR alphaOptions[] = { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR };
        for (auto option : alphaOptions) if (caps.supportedCompositeAlpha & option) { alpha = option; break; }
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
        // O compositor trabalha em coordenadas LÓGICAS (cena do Compose, gl_FragCoord, shaders sem rotação).
        // Com preTransform = currentTransform (ROTATE_90/270 em paisagem num painel retrato) o Android espera a
        // imagem em orientação nativa (extent trocado); sem pré-rotação a camada saía girada 90° e esticada
        // (vidros/sombras "fantasma" em lugar errado, corte no meio e composição por GPU lenta).
        // IDENTITY + extent lógico = o sistema aplica a rotação corretamente.
        if ((caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) == 0)
            return fail("Surface Vulkan sem suporte a preTransform IDENTITY");
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha = alpha;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        if (!check(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR")) return false;
        if (!check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR(count)")) return false;
        if (imageCount == 0) return fail("swapchain criada sem imagens");
        swapImages.resize(imageCount);
        if (!check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapImages.data()), "vkGetSwapchainImagesKHR(list)")) return false;
        renderFinishedByImage.resize(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo presentSemaphoreInfo{};
        presentSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < imageCount; ++i) {
            if (!check(vkCreateSemaphore(device, &presentSemaphoreInfo, nullptr, &renderFinishedByImage[i]), "vkCreateSemaphore(present-image)")) return false;
        }
        swapViews.resize(imageCount);
        swapFramebuffers.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swapImages[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapFormat;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (!check(vkCreateImageView(device, &vi, nullptr, &swapViews[i]), "vkCreateImageView(swap)")) return false;
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
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
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
        if (!check(vkCreateRenderPass(device, &ri, nullptr, &swapRenderPass), "vkCreateRenderPass(swap)")) return false;
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = swapRenderPass;
            fi.attachmentCount = 1;
            fi.pAttachments = &swapViews[i];
            fi.width = extent.width;
            fi.height = extent.height;
            fi.layers = 1;
            if (!check(vkCreateFramebuffer(device, &fi, nullptr, &swapFramebuffers[i]), "vkCreateFramebuffer(swap)")) return false;
        }
        return true;
    }

    bool createDescriptors() {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        si.maxLod = 0.0f;
        if (!check(vkCreateSampler(device, &si, nullptr, &sampler), "vkCreateSampler")) return false;
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device, &li, nullptr, &descriptorLayout), "vkCreateDescriptorSetLayout")) return false;
        return allocateDescriptorSet();
    }

    bool allocateDescriptorSet() {
        VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (!check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptorPool), "vkCreateDescriptorPool")) return false;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &descriptorLayout;
        if (!check(vkAllocateDescriptorSets(device, &ai, &descriptorSet), "vkAllocateDescriptorSets")) return false;
        return updateDescriptor();
    }

    bool updateDescriptor() {
        VkDescriptorImageInfo image{};
        image.sampler = sampler;
        image.imageView = baseView;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return true;
    }

    VkShaderModule shader(const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule result = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &ci, nullptr, &result) != VK_SUCCESS) return VK_NULL_HANDLE;
        return result;
    }

    VkPipelineLayout makeLayout(bool sampled, uint32_t pushSize) {
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.pushConstantRangeCount = 1;
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = pushSize;
        li.pPushConstantRanges = &range;
        li.setLayoutCount = sampled ? 1u : 0u;
        li.pSetLayouts = sampled ? &descriptorLayout : nullptr;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device, &li, nullptr, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
        return layout;
    }

    VkPipeline makePipeline(VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vert, VkShaderModule frag,
                            VkPrimitiveTopology topology, bool blend, bool instanceInput = false) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription instanceBinding{};
        VkVertexInputAttributeDescription instanceAttributes[3]{};
        if (instanceInput) {
            instanceBinding.binding = 0;
            instanceBinding.stride = sizeof(float) * 12;
            instanceBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            for (uint32_t i = 0; i < 3; ++i) {
                instanceAttributes[i].location = i;
                instanceAttributes[i].binding = 0;
                instanceAttributes[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
                instanceAttributes[i].offset = sizeof(float) * 4 * i;
            }
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &instanceBinding;
            vi.vertexAttributeDescriptionCount = 3;
            vi.pVertexAttributeDescriptions = instanceAttributes;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = topology;
        ia.primitiveRestartEnable = VK_FALSE;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.depthClampEnable = VK_FALSE;
        rs.rasterizerDiscardEnable = VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        ba.blendEnable = blend ? VK_TRUE : VK_FALSE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
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
        ci.layout = layout;
        ci.renderPass = pass;
        ci.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) return VK_NULL_HANDLE;
        return pipeline;
    }

    bool createPipelines() {
        VkShaderModule fullVert = shader(plasma_spv::kFullscreenVert, plasma_spv::kFullscreenVertSize);
        VkShaderModule wallFrag = shader(plasma_spv::kWallpaperFrag, plasma_spv::kWallpaperFragSize);
        VkShaderModule presentFrag = shader(plasma_spv::kPresentFrag, plasma_spv::kPresentFragSize);
        VkShaderModule glassVert = shader(plasma_spv::kGlassVert, plasma_spv::kGlassVertSize);
        VkShaderModule glassFrag = shader(plasma_spv::kGlassFrag, plasma_spv::kGlassFragSize);
        if (!fullVert || !wallFrag || !presentFrag || !glassVert || !glassFrag) return fail("falha ao criar modulos SPIR-V");
        wallpaperLayout = makeLayout(false, sizeof(WallpaperPush));
        sampleLayout = makeLayout(true, sizeof(PresentPush));
        if (!wallpaperLayout || !sampleLayout) return fail("falha em pipeline layout Vulkan");
        wallpaperPipeline = makePipeline(baseRenderPass, wallpaperLayout, fullVert, wallFrag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false);
        presentPipeline = makePipeline(swapRenderPass, sampleLayout, fullVert, presentFrag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false);
        glassPipeline = makePipeline(swapRenderPass, sampleLayout, glassVert, glassFrag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true, true);
        vkDestroyShaderModule(device, fullVert, nullptr);
        vkDestroyShaderModule(device, wallFrag, nullptr);
        vkDestroyShaderModule(device, presentFrag, nullptr);
        vkDestroyShaderModule(device, glassVert, nullptr);
        vkDestroyShaderModule(device, glassFrag, nullptr);
        if (!wallpaperPipeline || !presentPipeline || !glassPipeline) return fail("falha ao criar pipeline gráfico Vulkan");
        return true;
    }

    bool createInstanceBuffer(VkDeviceSize requestedBytes) {
        requestedBytes = std::max<VkDeviceSize>(requestedBytes, sizeof(float) * 12 * 128);
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = requestedBytes;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device, &bi, nullptr, &instanceBuffer), "vkCreateBuffer(instances)")) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, instanceBuffer, &req);
        uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) return fail("sem memoria host-visible/coherent para instancias Vulkan");
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(device, &ai, nullptr, &instanceMemory), "vkAllocateMemory(instances)")) return false;
        if (!check(vkBindBufferMemory(device, instanceBuffer, instanceMemory, 0), "vkBindBufferMemory(instances)")) return false;
        if (!check(vkMapMemory(device, instanceMemory, 0, req.size, 0, &mappedInstances), "vkMapMemory(instances)")) return false;
        instanceCapacity = requestedBytes;
        return true;
    }

    void destroyInstanceBuffer() {
        if (device == VK_NULL_HANDLE) return;
        if (mappedInstances && instanceMemory) vkUnmapMemory(device, instanceMemory);
        if (instanceBuffer) vkDestroyBuffer(device, instanceBuffer, nullptr);
        if (instanceMemory) vkFreeMemory(device, instanceMemory, nullptr);
        mappedInstances = nullptr;
        instanceBuffer = VK_NULL_HANDLE;
        instanceMemory = VK_NULL_HANDLE;
        instanceCapacity = 0;
        instanceCount = 0;
    }

    bool updateInstances(const std::vector<float>& windows, bool blurEnabled, uint64_t generation) {
        if (uploadedSceneGeneration == generation && uploadedBlurEnabled == blurEnabled &&
            uploadedInstanceExtent.width == extent.width && uploadedInstanceExtent.height == extent.height) return true;
        if (windows.size() > (64ull * 1024ull * 1024ull) / sizeof(float))
            return fail("cena de vidro excede 64 MiB de dados");
        instanceData.clear();
        instanceData.reserve(windows.size());
        instanceCount = 0;
        for (size_t i = 0; i + 11 < windows.size(); i += 12) {
            const float x = windows[i], y = windows[i + 1], w = windows[i + 2], h = windows[i + 3];
            if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h)) ||
                w <= 0.f || h <= 0.f || x + w <= 0.f || y + h <= 0.f || x >= extent.width || y >= extent.height) continue;
            const size_t start = instanceData.size();
            instanceData.insert(instanceData.end(), windows.begin() + static_cast<std::ptrdiff_t>(i),
                                windows.begin() + static_cast<std::ptrdiff_t>(i + 12));
            if (!blurEnabled) instanceData[start + 7] = 0.f;
            ++instanceCount;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(instanceData.size() * sizeof(float));
        if (bytes == 0) {
            uploadedSceneGeneration = generation;
            uploadedBlurEnabled = blurEnabled;
            uploadedInstanceExtent = extent;
            return true;
        }
        if (instanceBuffer == VK_NULL_HANDLE || bytes > instanceCapacity) {
            VkDeviceSize cap = std::max<VkDeviceSize>(instanceCapacity, sizeof(float) * 12 * 128);
            while (cap < bytes && cap < (64ull * 1024ull * 1024ull)) cap = std::min<VkDeviceSize>(cap * 2, 64ull * 1024ull * 1024ull);
            if (cap < bytes) return fail("cena de vidro excede 64 MiB de instancias");
            destroyInstanceBuffer();
            if (!createInstanceBuffer(cap)) return false;
        }
        std::memcpy(mappedInstances, instanceData.data(), static_cast<size_t>(bytes));
        uploadedSceneGeneration = generation;
        uploadedBlurEnabled = blurEnabled;
        uploadedInstanceExtent = extent;
        return true;
    }

    bool createCommands() {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queueFamily;
        if (!check(vkCreateCommandPool(device, &pi, nullptr, &commandPool), "vkCreateCommandPool")) return false;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &ai, &commandBuffer), "vkAllocateCommandBuffers")) return false;
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!check(vkCreateSemaphore(device, &si, nullptr, &imageAvailable), "vkCreateSemaphore(acquire)")) return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return check(vkCreateFence(device, &fi, nullptr, &inFlight), "vkCreateFence");
    }

    bool createRenderResources() {
        if (!createSwapchainObjects()) return false;
        if (!createBaseRenderPass()) return false;
        if (!createBaseImage()) return false;
        if (!createDescriptors()) return false;
        if (!createPipelines()) return false;
        return true;
    }

    void setViewport(VkCommandBuffer cmd, VkExtent2D ext) {
        VkViewport viewport{};
        viewport.x = 0.f; viewport.y = 0.f;
        viewport.width = static_cast<float>(ext.width);
        viewport.height = static_cast<float>(ext.height);
        viewport.minDepth = 0.f; viewport.maxDepth = 1.f;
        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = ext;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void renderWallpaper(VkCommandBuffer cmd, const Config& cfg, float time) {
        VkClearValue clear{};
        clear.color = {{ 0.f, 0.f, 0.f, 1.f }};
        VkRenderPassBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = baseRenderPass;
        bi.framebuffer = baseFramebuffer;
        bi.renderArea = { {0, 0}, baseExtent };
        bi.clearValueCount = 1;
        bi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        setViewport(cmd, baseExtent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wallpaperPipeline);
        WallpaperPush push{};
        push.resTimeMotion[0] = static_cast<float>(baseExtent.width);
        push.resTimeMotion[1] = static_cast<float>(baseExtent.height);
        push.resTimeMotion[2] = time;
        push.resTimeMotion[3] = cfg.animated ? 1.0f : 0.0f;
        push.stylePad[0] = static_cast<float>(cfg.style);
        memcpy(push.colors, cfg.colors.data(), sizeof(push.colors));
        vkCmdPushConstants(cmd, wallpaperLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    bool drawFrame(const Config& cfg, const std::vector<float>& windows, uint64_t windowsGeneration, bool redrawWall, float time) {
        if (!check(vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
            running.store(false);
            return false;
        }
        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device, swapchain, 250000000ull, imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { resizePending.store(true); return false; }
        const bool acquiredSuboptimal = acquire == VK_SUBOPTIMAL_KHR;
        if (acquire == VK_TIMEOUT || acquire == VK_NOT_READY) return false;
        if (acquire != VK_SUCCESS && !acquiredSuboptimal) {
            running.store(false);
            return check(acquire, "vkAcquireNextImageKHR");
        }
        if (!check(vkResetCommandBuffer(commandBuffer, 0), "vkResetCommandBuffer")) {
            running.store(false);
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commandBuffer, &begin), "vkBeginCommandBuffer")) {
            running.store(false);
            return false;
        }
        if (redrawWall) renderWallpaper(commandBuffer, cfg, time);
        VkClearValue clear{};
        clear.color = {{ 0.f, 0.f, 0.f, 1.f }};
        VkRenderPassBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = swapRenderPass;
        bi.framebuffer = swapFramebuffers[imageIndex];
        bi.renderArea = { {0, 0}, extent };
        bi.clearValueCount = 1;
        bi.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &bi, VK_SUBPASS_CONTENTS_INLINE);
        setViewport(commandBuffer, extent);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sampleLayout, 0, 1, &descriptorSet, 0, nullptr);
        PresentPush present{};
        present.resFx[0] = static_cast<float>(extent.width);
        present.resFx[1] = static_cast<float>(extent.height);
        present.resFx[2] = cfg.vignette;
        present.resFx[3] = cfg.dim;
        vkCmdPushConstants(commandBuffer, sampleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(present), &present);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

        if (cfg.blur || !windows.empty()) {
            if (!updateInstances(windows, cfg.blur, windowsGeneration)) {
                running.store(false);
                return false;
            }
            if (instanceCount > 0) {
                PresentPush glassGlobal{};
                glassGlobal.resFx[0] = static_cast<float>(extent.width);
                glassGlobal.resFx[1] = static_cast<float>(extent.height);
                glassGlobal.resFx[2] = 1.5f + cfg.blurStrength * 7.0f;
                vkCmdPushConstants(commandBuffer, sampleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(glassGlobal), &glassGlobal);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, glassPipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sampleLayout, 0, 1, &descriptorSet, 0, nullptr);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);
                vkCmdDraw(commandBuffer, 4, instanceCount, 0, 0);
            }
        }
        vkCmdEndRenderPass(commandBuffer);
        if (!check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer")) {
            running.store(false);
            return false;
        }
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinishedByImage[imageIndex];
        if (!check(vkResetFences(device, 1, &inFlight), "vkResetFences")) {
            running.store(false);
            return false;
        }
        if (!check(vkQueueSubmit(queue, 1, &submit, inFlight), "vkQueueSubmit")) {
            running.store(false);
            return false;
        }
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinishedByImage[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;
        VkResult result = vkQueuePresentKHR(queue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            resizePending.store(true);
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            running.store(false);
            return check(result, "vkQueuePresentKHR");
        }
        if ((result == VK_SUBOPTIMAL_KHR || acquiredSuboptimal) && surfaceExtentChanged()) resizePending.store(true);
        return true;
    }

    // Com preTransform IDENTITY numa tela rotacionada o Android devolve VK_SUBOPTIMAL_KHR em TODO present.
    // Só vale recriar a swapchain se o tamanho lógico da Surface realmente mudou.
    bool surfaceExtentChanged() {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS) return false;
        if (caps.currentExtent.width == UINT32_MAX) return false;
        return caps.currentExtent.width != extent.width || caps.currentExtent.height != extent.height;
    }

    void destroySwapchain() {
        if (device == VK_NULL_HANDLE) return;
        for (auto fb : swapFramebuffers) if (fb) vkDestroyFramebuffer(device, fb, nullptr);
        swapFramebuffers.clear();
        for (auto view : swapViews) if (view) vkDestroyImageView(device, view, nullptr);
        swapViews.clear(); swapImages.clear();
        for (auto semaphore : renderFinishedByImage) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        renderFinishedByImage.clear();
        if (swapRenderPass) vkDestroyRenderPass(device, swapRenderPass, nullptr);
        swapRenderPass = VK_NULL_HANDLE;
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    void destroyBaseTarget() {
        if (device == VK_NULL_HANDLE) return;
        if (baseFramebuffer) vkDestroyFramebuffer(device, baseFramebuffer, nullptr);
        if (baseView) vkDestroyImageView(device, baseView, nullptr);
        if (baseImage) vkDestroyImage(device, baseImage, nullptr);
        if (baseMemory) vkFreeMemory(device, baseMemory, nullptr);
        baseFramebuffer = VK_NULL_HANDLE; baseView = VK_NULL_HANDLE;
        baseImage = VK_NULL_HANDLE; baseMemory = VK_NULL_HANDLE;
    }

    bool recreateSwapchain() {
        if (device == VK_NULL_HANDLE) return false;
        vkDeviceWaitIdle(device);
        if (presentPipeline) vkDestroyPipeline(device, presentPipeline, nullptr);
        if (glassPipeline) vkDestroyPipeline(device, glassPipeline, nullptr);
        presentPipeline = VK_NULL_HANDLE; glassPipeline = VK_NULL_HANDLE;
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE; descriptorSet = VK_NULL_HANDLE;
        destroyBaseTarget();
        destroySwapchain();
        if (!createSwapchainObjects()) return false;
        if (!createBaseImage()) return false;
        if (!allocateDescriptorSet()) return false;
        if (!createPipelinesForSwapchain()) return false;
        { std::lock_guard<std::mutex> guard(stateMutex); wallDirty = true; dirty = true; }
        resizePending.store(false);
        return true;
    }

    bool createPipelinesForSwapchain() {
        VkShaderModule fullVert = shader(plasma_spv::kFullscreenVert, plasma_spv::kFullscreenVertSize);
        VkShaderModule presentFrag = shader(plasma_spv::kPresentFrag, plasma_spv::kPresentFragSize);
        VkShaderModule glassVert = shader(plasma_spv::kGlassVert, plasma_spv::kGlassVertSize);
        VkShaderModule glassFrag = shader(plasma_spv::kGlassFrag, plasma_spv::kGlassFragSize);
        if (!fullVert || !presentFrag || !glassVert || !glassFrag) return false;
        presentPipeline = makePipeline(swapRenderPass, sampleLayout, fullVert, presentFrag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false);
        glassPipeline = makePipeline(swapRenderPass, sampleLayout, glassVert, glassFrag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true, true);
        vkDestroyShaderModule(device, fullVert, nullptr);
        vkDestroyShaderModule(device, presentFrag, nullptr);
        vkDestroyShaderModule(device, glassVert, nullptr);
        vkDestroyShaderModule(device, glassFrag, nullptr);
        return presentPipeline && glassPipeline;
    }

    void destroyRenderResources() {
        if (device == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(device);
        destroyInstanceBuffer();
        if (wallpaperPipeline) vkDestroyPipeline(device, wallpaperPipeline, nullptr);
        if (presentPipeline) vkDestroyPipeline(device, presentPipeline, nullptr);
        if (glassPipeline) vkDestroyPipeline(device, glassPipeline, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE; descriptorSet = VK_NULL_HANDLE;
        destroyBaseTarget();
        destroySwapchain();
        if (wallpaperLayout) vkDestroyPipelineLayout(device, wallpaperLayout, nullptr);
        if (sampleLayout) vkDestroyPipelineLayout(device, sampleLayout, nullptr);
        if (baseRenderPass) vkDestroyRenderPass(device, baseRenderPass, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (inFlight) vkDestroyFence(device, inFlight, nullptr);
        if (imageAvailable) vkDestroySemaphore(device, imageAvailable, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        wallpaperPipeline = presentPipeline = glassPipeline = VK_NULL_HANDLE;
        wallpaperLayout = sampleLayout = VK_NULL_HANDLE;
        baseRenderPass = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE; descriptorLayout = VK_NULL_HANDLE; sampler = VK_NULL_HANDLE;
        inFlight = VK_NULL_HANDLE; imageAvailable = VK_NULL_HANDLE; commandPool = VK_NULL_HANDLE;
    }

    void run() {
        using Clock = std::chrono::steady_clock;
        int consecutiveResizeFailures = 0;
        std::vector<float> cachedWindows;
        uint64_t localSceneGen = 0;
        Clock::time_point lastWall{};  // epoch: o primeiro frame sempre desenha o papel de parede
        while (running.load()) {
            Config cfg;
            uint64_t windowsGeneration = 0;
            bool redrawWall = false;
            {
                std::unique_lock<std::mutex> lock(stateMutex);
                // Acorda na hora quando a cena (janelas/popups) muda; o papel de parede animado é redesenhado
                // só no ritmo do fpsCap. Antes o loop dormia o orçamento inteiro do frame e a cena de vidro
                // chegava com atraso (fantasma ao arrastar janelas).
                for (;;) {
                    if (!running.load()) break;
                    const auto now = Clock::now();
                    const auto budget = std::chrono::microseconds(1000000 / std::clamp(config.fpsCap, 12, 60));
                    const bool wallDue = config.animated && (now - lastWall) >= budget;
                    if (dirty || wallDirty || wallDue || resizePending.load()) break;
                    if (config.animated) wake.wait_until(lock, lastWall + budget);
                    else wake.wait(lock);
                }
                if (!running.load()) break;
                const auto now = Clock::now();
                cfg = config;
                const auto budget = std::chrono::microseconds(1000000 / std::clamp(cfg.fpsCap, 12, 60));
                if (sceneGeneration != localSceneGen) { cachedWindows = scene; localSceneGen = sceneGeneration; }
                windowsGeneration = localSceneGen;
                redrawWall = wallDirty || resizePending.load() || (cfg.animated && (now - lastWall) >= budget);
                dirty = false;
                wallDirty = false;
            }
            if (resizePending.load()) {
                if (!recreateSwapchain()) {
                    if (++consecutiveResizeFailures >= 5) {
                        VKLOGE("swapchain nao foi recriada apos cinco tentativas; encerrando Vulkan para fallback");
                        running.store(false);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                consecutiveResizeFailures = 0;
                redrawWall = true;
            }
            const auto frameStart = Clock::now();
            const float elapsed = std::chrono::duration<float>(frameStart - startTime).count();
            if (redrawWall) lastWall = frameStart;
            if (!drawFrame(cfg, cachedWindows, windowsGeneration, redrawWall, elapsed)) {
                if (!resizePending.load()) VKLOGW("um frame Vulkan foi descartado; o próximo ciclo tentará novamente");
            }
        }
    }
};

}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_render_NativeVulkan_hasCompositor(JNIEnv*, jclass) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_plasma_render_NativeVulkan_create(JNIEnv* env, jclass, jobject javaSurface) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    ANativeWindow* window = ANativeWindow_fromSurface(env, javaSurface);
    if (!window) { VKLOGE("ANativeWindow_fromSurface falhou"); return 0; }
    auto* renderer = new VulkanCompositor(window);
    if (!renderer->initialize()) { delete renderer; return 0; }
    return reinterpret_cast<jlong>(renderer);
#else
    (void)env; (void)javaSurface;
    return 0;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_render_NativeVulkan_configure(JNIEnv* env, jclass, jlong handle, jint style,
                                                       jfloatArray palette, jboolean animated, jint fps,
                                                       jboolean blur, jfloat blurStrength, jfloat vignette, jfloat dim) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    if (!renderer) return;
    Config cfg;
    cfg.style = std::clamp(style, 0, 4);
    cfg.animated = animated == JNI_TRUE;
    cfg.fpsCap = std::clamp(fps, 12, 60);
    cfg.blur = blur == JNI_TRUE;
    cfg.blurStrength = std::clamp(blurStrength, 0.f, 1.f);
    cfg.vignette = std::clamp(vignette, 0.f, 1.f);
    cfg.dim = std::clamp(dim, 0.f, 1.f);
    if (palette && env->GetArrayLength(palette) >= 12) {
        jfloat values[12];
        env->GetFloatArrayRegion(palette, 0, 12, values);
        if (!env->ExceptionCheck()) for (int i = 0; i < 4; ++i) {
            cfg.colors[i * 4] = values[i * 3];
            cfg.colors[i * 4 + 1] = values[i * 3 + 1];
            cfg.colors[i * 4 + 2] = values[i * 3 + 2];
            cfg.colors[i * 4 + 3] = 1.f;
        }
    }
    renderer->configure(cfg);
#else
    (void)env; (void)handle; (void)style; (void)palette; (void)animated; (void)fps; (void)blur; (void)blurStrength; (void)vignette; (void)dim;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_render_NativeVulkan_setScene(JNIEnv* env, jclass, jlong handle, jfloatArray values) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    if (!renderer) return;
    std::vector<float> scene;
    if (values) {
        const jsize count = env->GetArrayLength(values);
        scene.resize(static_cast<size_t>(count));
        if (count) env->GetFloatArrayRegion(values, 0, count, scene.data());
    }
    if (!env->ExceptionCheck()) renderer->setScene(scene);
#else
    (void)env; (void)handle; (void)values;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_render_NativeVulkan_resize(JNIEnv*, jclass, jlong handle, jint width, jint height) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    if (renderer && width > 0 && height > 0) renderer->resize(width, height);
#else
    (void)handle; (void)width; (void)height;
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_plasma_render_NativeVulkan_deviceName(JNIEnv* env, jclass, jlong handle) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    const std::string name = renderer ? renderer->name() : "";
    return env->NewStringUTF(name.c_str());
#else
    return env->NewStringUTF("");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_render_NativeVulkan_isRunning(JNIEnv*, jclass, jlong handle) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    return renderer && renderer->isRunning() ? JNI_TRUE : JNI_FALSE;
#else
    (void)handle;
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_render_NativeVulkan_destroy(JNIEnv*, jclass, jlong handle) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    auto* renderer = reinterpret_cast<VulkanCompositor*>(handle);
    delete renderer;
#else
    (void)handle;
#endif
}

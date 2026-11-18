
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlasmaRender", __VA_ARGS__)

static bool hasExt(const char* list, const char* name) { return list && strstr(list, name) != nullptr; }
static const char* glStr(GLenum e) { auto s = reinterpret_cast<const char*>(glGetString(e)); return s ? s : ""; }

static std::string probeVulkan(bool selfTest) {
    std::ostringstream o;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "KDE Plasma Android Desktop";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) { o << "vk.available=0\nvk.error=vkCreateInstance:" << (int)r << "\n"; return o.str(); }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (n == 0) { o << "vk.available=0\nvk.error=nenhum dispositivo fisico\n"; vkDestroyInstance(inst, nullptr); return o.str(); }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice pd = devs[0];
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    o << "vk.available=1\nvk.device=" << p.deviceName
      << "\nvk.apiMajor=" << VK_VERSION_MAJOR(p.apiVersion) << "\nvk.apiMinor=" << VK_VERSION_MINOR(p.apiVersion)
      << "\nvk.driverVersion=" << p.driverVersion << "\nvk.vendorId=" << p.vendorID << "\n";

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, exts.data());
    auto has = [&](const char* nm) { for (auto& e : exts) if (!strcmp(e.extensionName, nm)) return true; return false; };
    o << "vk.extCount=" << en
      << "\nvk.ext.swapchain=" << has("VK_KHR_swapchain")
      << "\nvk.ext.ahb=" << has("VK_ANDROID_external_memory_android_hardware_buffer")
      << "\nvk.ext.externalMemoryFd=" << has("VK_KHR_external_memory_fd")
      << "\nvk.ext.foreignQueue=" << has("VK_EXT_queue_family_foreign")
      << "\nvk.ext.ycbcr=" << has("VK_KHR_sampler_ycbcr_conversion") << "\n";

    if (selfTest) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        int gq = -1;
        for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gq = (int)i; break; }
        if (gq < 0) {
            o << "vk.selftest=fail:sem fila grafica\n";
        } else {
            float prio = 1.f;
            VkDeviceQueueCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qi.queueFamilyIndex = (uint32_t)gq; qi.queueCount = 1; qi.pQueuePriorities = &prio;
            VkDeviceCreateInfo dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
            VkDevice dev = VK_NULL_HANDLE;
            VkResult dr = vkCreateDevice(pd, &dci, nullptr, &dev);
            if (dr != VK_SUCCESS) {
                o << "vk.selftest=fail:vkCreateDevice " << (int)dr << "\n";
            } else {
                VkQueue q; vkGetDeviceQueue(dev, (uint32_t)gq, 0, &q);
                bool ok = false;
                VkCommandPoolCreateInfo pci{};
                pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.queueFamilyIndex = (uint32_t)gq;
                VkCommandPool pool = VK_NULL_HANDLE;
                if (vkCreateCommandPool(dev, &pci, nullptr, &pool) == VK_SUCCESS) {
                    VkCommandBufferAllocateInfo ai{};
                    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                    VkCommandBuffer cb;
                    if (vkAllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS) {
                        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                        if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS && vkEndCommandBuffer(cb) == VK_SUCCESS) {
                            VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                            VkFence f = VK_NULL_HANDLE;
                            if (vkCreateFence(dev, &fi, nullptr, &f) == VK_SUCCESS) {
                                VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                                ok = vkQueueSubmit(q, 1, &si, f) == VK_SUCCESS &&
                                     vkWaitForFences(dev, 1, &f, VK_TRUE, 1000000000ull) == VK_SUCCESS;
                                vkDestroyFence(dev, f, nullptr);
                            }
                        }
                    }
                    vkDestroyCommandPool(dev, pool, nullptr);
                }
                vkDeviceWaitIdle(dev);
                vkDestroyDevice(dev, nullptr);
                o << "vk.selftest=" << (ok ? "ok" : "fail:submissao") << "\n";
            }
        }
    }
    vkDestroyInstance(inst, nullptr);
    return o.str();
}

static std::string probeGles(bool selfTest) {
    std::ostringstream o;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { o << "gles.available=0\ngles.error=sem display EGL\n"; return o.str(); }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { o << "gles.available=0\ngles.error=eglInitialize falhou\n"; return o.str(); }
    const char* eglExt = eglQueryString(dpy, EGL_EXTENSIONS);
    o << "egl.version=" << maj << "." << min
      << "\negl.ext.nativeFence=" << hasExt(eglExt, "EGL_ANDROID_native_fence_sync")
      << "\negl.ext.imageNativeBuffer=" << hasExt(eglExt, "EGL_ANDROID_image_native_buffer")
      << "\negl.ext.imageBase=" << hasExt(eglExt, "EGL_KHR_image_base")
      << "\negl.ext.dmaBuf=" << hasExt(eglExt, "EGL_EXT_image_dma_buf_import") << "\n";

    EGLConfig cfg; EGLint nc = 0;
    auto choose = [&](EGLint renderable) {
        EGLint a[] = {EGL_RENDERABLE_TYPE, renderable, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        return eglChooseConfig(dpy, a, &cfg, 1, &nc) && nc > 0;
    };
    int es = 3;
    EGLContext ctx = EGL_NO_CONTEXT;
    if (choose(EGL_OPENGL_ES3_BIT)) {
        EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    }
    if (ctx == EGL_NO_CONTEXT) {
        es = 2;
        if (choose(EGL_OPENGL_ES2_BIT)) {
            EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
            ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
        }
    }
    if (ctx == EGL_NO_CONTEXT) { o << "gles.available=0\ngles.error=sem contexto ES2/ES3\n"; eglTerminate(dpy); return o.str(); }
    EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        o << "gles.available=0\ngles.error=eglMakeCurrent falhou\n";
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        eglDestroyContext(dpy, ctx); eglTerminate(dpy);
        return o.str();
    }
    const char* glExt = glStr(GL_EXTENSIONS);
    o << "gles.available=1\ngles.contextVersion=" << es
      << "\ngles.version=" << glStr(GL_VERSION) << "\ngles.renderer=" << glStr(GL_RENDERER)
      << "\ngles.vendor=" << glStr(GL_VENDOR)
      << "\ngles.ext.eglImageExternal=" << hasExt(glExt, "GL_OES_EGL_image_external")
      << "\ngles.ext.eglImage=" << hasExt(glExt, "GL_OES_EGL_image") << "\n";
    if (selfTest) {
        glClearColor(0.2f, 0.4f, 0.6f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        GLenum err = glGetError();
        bool ok = err == GL_NO_ERROR && px[2] >= 148 && px[2] <= 158;
        o << "gles.selftest=" << (ok ? "ok" : "fail:pixel/erro") << "\n";
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx); eglTerminate(dpy);
    return o.str();
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_plasma_render_NativeProbe_probeVulkan(JNIEnv* env, jclass, jboolean t) {
    return env->NewStringUTF(probeVulkan(t).c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_plasma_render_NativeProbe_probeGles(JNIEnv* env, jclass, jboolean t) {
    return env->NewStringUTF(probeGles(t).c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_render_NativeProbe_hasVulkanCompositor(JNIEnv*, jclass) {
#if PLASMA_HAS_VULKAN_COMPOSITOR
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

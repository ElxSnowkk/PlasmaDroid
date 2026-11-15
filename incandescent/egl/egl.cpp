// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - egl/egl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "egl.h"
#include "context.h"
#include "../config/settings.h"
#include "../gl/FSR1/FSR1.h"
#include "../gl/mali_tuning.h"
#include "../gl/log.h"
#include "../gl/inc.h"
#include "../gles/loader.h"
#include "../glx/lookup.h"
#include "loader.h"
#include "trace.h"
#include <EGL/eglext.h>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>
#include <memory>
#include <ska/flat_hash_map.hpp>
#include <utility>
#include <vector>

#define DEBUG 0

namespace {
    EGLDisplay realDpy(EGLDisplay dpy);
}

namespace {

    constexpr EGLint kBackendDesktopGlClientVersion = 3;
    constexpr EGLint kBackendDesktopGlRenderableBit = EGL_OPENGL_ES3_BIT;
    constexpr EGLint kVirtualDesktopProfileMask = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;

    constexpr EGLint kVirtualCompatProfileMask = EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
    constexpr size_t kMaxEglAttributePairs = 128;

    thread_local EGLenum frontend_api = EGL_OPENGL_ES_API;
    thread_local EGLint frontend_error = EGL_SUCCESS;

    std::mutex context_lifecycle_mutex;

    void setFrontendError(EGLint error) {
        ETRACE("virtual error queued: %s", inc_egl_error_name(error));
        frontend_error = error;
    }

    const char* eglAttributeName(EGLint attribute) {
        switch (attribute) {
        case EGL_NONE:
            return "NONE";
        case EGL_RED_SIZE:
            return "RED_SIZE";
        case EGL_GREEN_SIZE:
            return "GREEN_SIZE";
        case EGL_BLUE_SIZE:
            return "BLUE_SIZE";
        case EGL_ALPHA_SIZE:
            return "ALPHA_SIZE";
        case EGL_DEPTH_SIZE:
            return "DEPTH_SIZE";
        case EGL_STENCIL_SIZE:
            return "STENCIL_SIZE";
        case EGL_SAMPLES:
            return "SAMPLES";
        case EGL_SAMPLE_BUFFERS:
            return "SAMPLE_BUFFERS";
        case EGL_SURFACE_TYPE:
            return "SURFACE_TYPE";
        case EGL_RENDERABLE_TYPE:
            return "RENDERABLE_TYPE";
        case EGL_CONFORMANT:
            return "CONFORMANT";
        case EGL_CONFIG_ID:
            return "CONFIG_ID";
        case EGL_CONTEXT_CLIENT_VERSION:
            return "CONTEXT_MAJOR_VERSION";
        case EGL_CONTEXT_MINOR_VERSION:
            return "CONTEXT_MINOR_VERSION";
        case EGL_CONTEXT_FLAGS_KHR:
            return "CONTEXT_FLAGS";
        case EGL_CONTEXT_OPENGL_PROFILE_MASK:
            return "PROFILE_MASK";
        case EGL_CONTEXT_OPENGL_DEBUG:
            return "OPENGL_DEBUG";
        case EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE:
            return "FORWARD_COMPATIBLE";
        case EGL_CONTEXT_OPENGL_ROBUST_ACCESS:
            return "ROBUST_ACCESS";
        case EGL_CONTEXT_OPENGL_NO_ERROR_KHR:
            return "NO_ERROR";
        case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY:
            return "RESET_NOTIFICATION_STRATEGY";
        case EGL_WIDTH:
            return "WIDTH";
        case EGL_HEIGHT:
            return "HEIGHT";
        default:
            return nullptr;
        }
    }

    std::string describeAttributes(const EGLint* attrib_list) {
        if (!attrib_list) return "(none)";
        std::string out;
        for (size_t pair = 0; pair < kMaxEglAttributePairs; ++pair) {
            const EGLint attribute = attrib_list[pair * 2];
            if (attribute == EGL_NONE) break;
            const EGLint value = attrib_list[pair * 2 + 1];
            if (!out.empty()) out += ", ";
            const char* name = eglAttributeName(attribute);
            char buffer[64];
            if (name) {
                snprintf(buffer, sizeof(buffer), "%s=0x%x", name, value);
            } else {
                snprintf(buffer, sizeof(buffer), "0x%x=0x%x", attribute, value);
            }
            out += buffer;
        }
        return out.empty() ? "(empty)" : out;
    }

    std::string describeAttributes(const std::vector<EGLint>& attributes) {
        return attributes.empty() ? "(empty)" : describeAttributes(attributes.data());
    }

    bool copyAttributeList(const EGLint* attrib_list, std::vector<EGLint>* attributes) {
        attributes->clear();
        if (!attrib_list) return true;

        for (size_t pair = 0; pair < kMaxEglAttributePairs; ++pair) {
            const EGLint attribute = attrib_list[pair * 2];
            attributes->push_back(attribute);
            if (attribute == EGL_NONE) return true;
            attributes->push_back(attrib_list[pair * 2 + 1]);
        }

        return false;
    }

    bool containsOpenGLBit(EGLint value) {
        return (value & EGL_OPENGL_BIT) != 0;
    }

    EGLint backendRenderableMask(EGLint frontend_mask) {
        if (containsOpenGLBit(frontend_mask)) {
            frontend_mask &= ~EGL_OPENGL_BIT;
            frontend_mask |= kBackendDesktopGlRenderableBit;
        }
        return frontend_mask;
    }

    EGLint frontendRenderableMask(EGLint backend_mask) {
        if ((backend_mask & kBackendDesktopGlRenderableBit) != 0) {
            backend_mask |= EGL_OPENGL_BIT;
        }
        return backend_mask;
    }

    bool makeBackendConfigAttributes(const EGLint* attrib_list, std::vector<EGLint>* backend_attributes) {
        if (!copyAttributeList(attrib_list, backend_attributes)) return false;
        if (backend_attributes->empty()) backend_attributes->push_back(EGL_NONE);

        bool has_renderable_type = false;
        bool requests_desktop_gl = frontend_api == EGL_OPENGL_API;
        for (size_t i = 0; i + 1 < backend_attributes->size(); i += 2) {
            const EGLint attribute = (*backend_attributes)[i];
            if (attribute == EGL_NONE) break;

            EGLint& value = (*backend_attributes)[i + 1];
            if (attribute == EGL_RENDERABLE_TYPE) {
                has_renderable_type = true;
                requests_desktop_gl = requests_desktop_gl || containsOpenGLBit(value);
                value = backendRenderableMask(value);
            } else if (attribute == EGL_CONFORMANT) {
                requests_desktop_gl = requests_desktop_gl || containsOpenGLBit(value);
                value = backendRenderableMask(value);
            }
        }

        if (!has_renderable_type && requests_desktop_gl) {
            backend_attributes->insert(backend_attributes->end() - 1,
                                       {EGL_RENDERABLE_TYPE, kBackendDesktopGlRenderableBit});
        } else if (has_renderable_type && requests_desktop_gl) {
            for (size_t i = 0; i + 1 < backend_attributes->size(); i += 2) {
                if ((*backend_attributes)[i] == EGL_NONE) break;
                if ((*backend_attributes)[i] == EGL_RENDERABLE_TYPE) {
                    (*backend_attributes)[i + 1] |= kBackendDesktopGlRenderableBit;
                }
            }
        }
        return true;
    }

    bool isDesktopOnlyContextAttribute(EGLint attribute) {
        switch (attribute) {
        case EGL_CONTEXT_MINOR_VERSION:
        case EGL_CONTEXT_FLAGS_KHR:
        case EGL_CONTEXT_OPENGL_PROFILE_MASK:
        case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY:
        case EGL_CONTEXT_OPENGL_DEBUG:
        case EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE:
        case EGL_CONTEXT_OPENGL_ROBUST_ACCESS:
        case EGL_CONTEXT_OPENGL_NO_ERROR_KHR:
        case EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT:
        case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT:
            return true;
        default:
            return false;
        }
    }

    bool supportsDesktopContextAttribute(EGLint attribute, EGLint value) {
        switch (attribute) {
        case EGL_CONTEXT_FLAGS_KHR:
            return (value & ~(EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR | EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR |
                              EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)) == 0;
        case EGL_CONTEXT_OPENGL_PROFILE_MASK:
            return value != 0 && (value & ~(kVirtualDesktopProfileMask | kVirtualCompatProfileMask)) == 0;
        case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY:
        case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT:
            return value == EGL_NO_RESET_NOTIFICATION;
        case EGL_CONTEXT_OPENGL_DEBUG:
        case EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE:
        case EGL_CONTEXT_OPENGL_NO_ERROR_KHR:
            return true;
        case EGL_CONTEXT_OPENGL_ROBUST_ACCESS:
        case EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT:
            return true;
        default:
            return false;
        }
    }

    EGLint defaultDesktopMajorVersion() {
        return global_settings.custom_gl_version.isEmpty() ? DEFAULT_GL_VERSION / 10
                                                           : global_settings.custom_gl_version.Major;
    }

    EGLint defaultDesktopMinorVersion() {
        return global_settings.custom_gl_version.isEmpty() ? DEFAULT_GL_VERSION % 10
                                                           : global_settings.custom_gl_version.Minor;
    }

    bool makeBackendContextAttributes(const EGLint* attrib_list, std::vector<EGLint>* backend_attributes,
                                      EGLint* frontend_major, EGLint* frontend_minor, EGLint* frontend_flags,
                                      EGLint* error) {
        *frontend_flags = 0;
        *error = EGL_SUCCESS;
        if (!copyAttributeList(attrib_list, backend_attributes)) {
            *error = EGL_BAD_ATTRIBUTE;
            return false;
        }

        EGLint requested_major = defaultDesktopMajorVersion();
        EGLint requested_minor = defaultDesktopMinorVersion();
        bool saw_major_version = false;
        bool saw_minor_version = false;
        std::vector<EGLint> rewritten;
        rewritten.reserve(backend_attributes->size() + 3);

        for (size_t i = 0; i + 1 < backend_attributes->size(); i += 2) {
            const EGLint attribute = (*backend_attributes)[i];
            if (attribute == EGL_NONE) break;
            const EGLint value = (*backend_attributes)[i + 1];

            if (attribute == EGL_CONTEXT_CLIENT_VERSION) {
                if (saw_major_version) {
                    *error = EGL_BAD_ATTRIBUTE;
                    return false;
                }
                requested_major = value;
                saw_major_version = true;
                continue;
            }
            if (attribute == EGL_CONTEXT_MINOR_VERSION) {
                if (saw_minor_version) {
                    *error = EGL_BAD_ATTRIBUTE;
                    return false;
                }
                requested_minor = value;
                saw_minor_version = true;
                continue;
            }

            if (attribute == EGL_CONTEXT_FLAGS_KHR) {
                if (value & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR) *frontend_flags |= GL_CONTEXT_FLAG_DEBUG_BIT;
                if (value & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
                    *frontend_flags |= GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT;
                if (value & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)
                    *frontend_flags |= GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT;
            } else if (attribute == EGL_CONTEXT_OPENGL_DEBUG && value == EGL_TRUE) {
                *frontend_flags |= GL_CONTEXT_FLAG_DEBUG_BIT;
            } else if (attribute == EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE && value == EGL_TRUE) {
                *frontend_flags |= GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT;
            } else if ((attribute == EGL_CONTEXT_OPENGL_ROBUST_ACCESS ||
                        attribute == EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT) &&
                       value == EGL_TRUE) {
                *frontend_flags |= GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT;
            }

            if (isDesktopOnlyContextAttribute(attribute)) {
                if (!supportsDesktopContextAttribute(attribute, value)) {
                    *error = EGL_BAD_MATCH;
                    return false;
                }
                continue;
            }

            rewritten.push_back(attribute);
            rewritten.push_back(value);
        }

        if (saw_major_version && !saw_minor_version) requested_minor = 0;

        const EGLint configured = defaultDesktopMajorVersion() * 100 + defaultDesktopMinorVersion() * 10;
        const EGLint requested = requested_major * 100 + requested_minor * 10;
        if (requested > configured) {
            LOG_W_FORCE("eglCreateContext: %d.%d was requested but customGLVersion is %d.%d", requested_major,
                        requested_minor, defaultDesktopMajorVersion(), defaultDesktopMinorVersion())
            *error = EGL_BAD_MATCH;
            return false;
        }

        if (requested < 320) {

            *frontend_major = defaultDesktopMajorVersion();
            *frontend_minor = defaultDesktopMinorVersion();
        } else {
            *frontend_major = requested_major;
            *frontend_minor = requested_minor;
        }

        rewritten.push_back(EGL_CONTEXT_CLIENT_VERSION);
        rewritten.push_back(kBackendDesktopGlClientVersion);
        rewritten.push_back(EGL_NONE);
        *backend_attributes = std::move(rewritten);
        return true;
    }

    using SwapWithDamageFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint);

    SwapWithDamageFn resolveSwapWithDamage(const char* name) {
        if (egl != nullptr) {
            if (auto* fn = (SwapWithDamageFn)proc_address(egl, name)) {
                ETRACE("%s resolved via dlsym", name);
                return fn;
            }
        }
        LOAD_EGL(eglGetProcAddress)
        if (egl_eglGetProcAddress == nullptr) {
            ETRACE("%s: backend has neither the symbol nor eglGetProcAddress", name);
            return nullptr;
        }
        auto* fn = (SwapWithDamageFn)egl_eglGetProcAddress(name);
        ETRACE("%s resolved via backend eglGetProcAddress -> %p", name, (void*)fn);
        return fn;
    }

    EGLBoolean presentSurface(EGLDisplay dpy, EGLSurface surface) {
        LOAD_EGL(eglSwapBuffers)
        inc_probe_swap();
        if (global_settings.fsr1_setting == FSR1_Quality_Preset::Disabled) {
            mali_before_swap();
            return egl_eglSwapBuffers(realDpy(dpy), surface);
        }
        ApplyFSR();
        mali_before_swap();
        const EGLBoolean result = egl_eglSwapBuffers(realDpy(dpy), surface);
        if (result != EGL_TRUE) {

            static unsigned s_swapFail = 0;
            if ((s_swapFail++ % 300u) == 0) {
                LOAD_EGL(eglGetError)
                LOG_W("eglSwapBuffers falhou (#%u): eglGetError=0x%x", s_swapFail, static_cast<unsigned>(egl_eglGetError()));
            }
        }
        CheckResolutionChange(dpy, surface);
        return result;
    }

    EGLBoolean presentSurfaceWithDamage(EGLDisplay dpy, EGLSurface surface, EGLint* rects, EGLint n_rects,
                                        SwapWithDamageFn backend) {
        const bool fsr_on = global_settings.fsr1_setting != FSR1_Quality_Preset::Disabled;
        if (backend != nullptr && !fsr_on) {
            inc_probe_swap();
            return backend(dpy, surface, rects, n_rects);
        }

        static bool logged = false;
        if (!logged) {
            logged = true;
            ETRACE("damage-swap(dpy=%p) falling back to a full swap (backend=%d, fsr_on=%d)", dpy, backend != nullptr,
                   fsr_on);
        }
        return presentSurface(dpy, surface);
    }

    bool hasExtension(const std::string& extensions, const char* extension) {
        const std::string needle(extension);
        size_t offset = 0;
        while ((offset = extensions.find(needle, offset)) != std::string::npos) {
            const bool starts_at_boundary = offset == 0 || extensions[offset - 1] == ' ';
            const size_t end = offset + needle.size();
            const bool ends_at_boundary = end == extensions.size() || extensions[end] == ' ';
            if (starts_at_boundary && ends_at_boundary) return true;
            offset = end;
        }
        return false;
    }

    std::mutex ext_strings_mutex;
    ska::flat_hash_map<EGLDisplay, std::unique_ptr<std::string>> ext_strings;

    bool isForeignPlatform(EGLenum platform) {
        switch (platform) {
        case EGL_PLATFORM_WAYLAND_KHR:
        case EGL_PLATFORM_SURFACELESS_MESA:
            return true;
        default:
            return false;
        }
    }

    std::mutex client_ext_mutex;
    std::string client_ext_cache;

    const char* clientExtensionString(const char* backend_extensions) {
        std::lock_guard<std::mutex> lock(client_ext_mutex);
        if (!client_ext_cache.empty()) return client_ext_cache.c_str();
        std::string built = backend_extensions ? backend_extensions : "";
        auto add = [&built](const char* name) {
            if (hasExtension(built, name)) return;
            if (!built.empty()) built += ' ';
            built += name;
        };
        add("EGL_EXT_client_extensions");
        add("EGL_EXT_platform_base");
        add("EGL_KHR_platform_wayland");
        add("EGL_EXT_platform_wayland");
        add("EGL_MESA_platform_surfaceless");
        client_ext_cache = std::move(built);
        return client_ext_cache.c_str();
    }

    const char* frontendExtensionString(EGLDisplay dpy, const char* backend_extensions) {
        std::lock_guard<std::mutex> lock(ext_strings_mutex);
        auto it = ext_strings.find(dpy);
        if (it != ext_strings.end()) return it->second->c_str();

        std::string built = backend_extensions ? backend_extensions : "";
        auto add = [&built](const char* name) {
            if (hasExtension(built, name)) return;
            if (!built.empty()) built += ' ';
            built += name;
        };

        add("EGL_KHR_create_context");

        add("EGL_KHR_get_all_proc_addresses");
        add("EGL_KHR_client_get_all_proc_addresses");

        auto res = ext_strings.emplace(dpy, std::make_unique<std::string>(std::move(built)));
        return res.first->second->c_str();
    }

    std::mutex g_alias_mutex;
    ska::flat_hash_map<EGLDisplay, EGLDisplay> g_aliases;
    std::atomic<int> g_alias_count{0};

    void addDisplayAlias(EGLDisplay alias, EGLDisplay real) {
        std::lock_guard<std::mutex> lock(g_alias_mutex);
        g_aliases[alias] = real;
        g_alias_count.store((int)g_aliases.size(), std::memory_order_release);
    }

    EGLDisplay realDpy(EGLDisplay dpy) {
        if (g_alias_count.load(std::memory_order_acquire) == 0) return dpy;
        std::lock_guard<std::mutex> lock(g_alias_mutex);
        const auto it = g_aliases.find(dpy);
        return it == g_aliases.end() ? dpy : it->second;
    }

}

extern "C"
{
#define EGL_API __attribute__((visibility("default")))
    EGL_API EGLint eglGetError(void) {
        LOG_D("eglGetError");
        LOAD_EGL(eglGetError)

        if (frontend_error != EGL_SUCCESS) {
            const EGLint error = frontend_error;
            frontend_error = EGL_SUCCESS;

            const EGLint discarded = egl_eglGetError();
            ETRACE("eglGetError -> %s (virtual; backend had %s)", inc_egl_error_name(error),
                   inc_egl_error_name(discarded));
            return error;
        }
        const EGLint error = egl_eglGetError();
        if (error != EGL_SUCCESS) ETRACE("eglGetError -> %s", inc_egl_error_name(error));
        return error;
    }

    EGL_API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
        LOG_D("eglGetDisplay, display_id: %p", display_id);
        LOAD_EGL(eglGetDisplay)

        const EGLNativeDisplayType backend_id = EGL_DEFAULT_DISPLAY;
        const EGLDisplay dpy = egl_eglGetDisplay(backend_id);
        ETRACE("eglGetDisplay(%p) -> %p (backend recebeu o display padrão)", display_id, dpy);
        PGL_LOG("eglGetDisplay(%p) -> %p (display padrao do backend)", display_id, dpy);
        return dpy;
    }

    EGL_API EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        LOG_D("eglInitialize, dpy: %p, major: %p, minor: %p", dpy, major, minor);
        LOAD_EGL(eglInitialize)
        LOAD_EGL(eglGetDisplay)
        LOAD_EGL(eglGetError)
        EGLint maj = 0, min = 0;
        EGLBoolean result = egl_eglInitialize(realDpy(dpy), &maj, &min);
        if (result != EGL_TRUE && dpy != EGL_NO_DISPLAY) {
            const EGLint err = egl_eglGetError();
            const EGLDisplay def = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);
            PGL_LOG("eglInitialize(%p) falhou no backend: EGL_%s (0x%x); display padrao do backend = %p", dpy,
                    inc_egl_error_name(err), (unsigned)err, def);
            if (def != EGL_NO_DISPLAY) {

                result = egl_eglInitialize(def, &maj, &min);
                if (result == EGL_TRUE) {
                    if (def != dpy) {
                        addDisplayAlias(dpy, def);
                        PGL_LOG("display %p agora e apelido do display padrao %p", dpy, def);
                    }
                } else if (def == dpy && inc_egl_known_version(&maj, &min)) {
                    PGL_LOG("backend recusou reinicializar; usando a versao ja conhecida %d.%d", maj, min);
                    result = EGL_TRUE;
                } else {
                    const EGLint err2 = egl_eglGetError();
                    PGL_LOG("segunda tentativa no display padrao tambem falhou: EGL_%s (0x%x)", inc_egl_error_name(err2),
                            (unsigned)err2);
                }
            }
        }
        if (result == EGL_TRUE) {
            if (major) *major = maj;
            if (minor) *minor = min;
            inc_egl_remember_version(maj, min);
            ETRACE("eglInitialize(%p) -> EGL %d.%d", dpy, maj, min);
            inc_display_initialised(dpy, false);
        } else {

            ETRACE("eglInitialize(%p) FAILED", dpy);
        }
        return result;
    }

    EGL_API EGLBoolean eglTerminate(EGLDisplay dpy) {
        LOG_D("eglTerminate, dpy: %p", dpy);
        LOAD_EGL(eglTerminate)

        if (!inc_display_release(dpy, false)) {
            ETRACE("eglTerminate(%p): other holders remain, not terminating", dpy);
            inc_context_forget_display(dpy);
            return EGL_TRUE;
        }
        if (!inc_egl_real_terminate()) {

            ETRACE("eglTerminate(%p): last holder, keeping the backend display alive (INC_EGL_REAL_TERMINATE=1 to end it)", dpy);
            inc_context_forget_display(dpy);
            return EGL_TRUE;
        }
        ETRACE("eglTerminate(%p): last holder, terminating for real", dpy);
        const EGLBoolean result = egl_eglTerminate(realDpy(dpy));
        if (result == EGL_TRUE) {
            inc_context_forget_display(dpy);

            std::lock_guard<std::mutex> lock(ext_strings_mutex);
            ext_strings.erase(dpy);
        }
        return result;
    }

    EGL_API const char* eglQueryString(EGLDisplay dpy, EGLint name) {
        LOG_D("eglQueryString, dpy: %p, name: %d", dpy, name);
        LOAD_EGL(eglQueryString)
        const char* result = egl_eglQueryString(realDpy(dpy), name);

        if (dpy == EGL_NO_DISPLAY && name == EGL_EXTENSIONS) return clientExtensionString(result);
        if (!result) return nullptr;

        if (name == EGL_CLIENT_APIS) return "OpenGL OpenGL_ES";
        if (name == EGL_EXTENSIONS) return frontendExtensionString(dpy, result);
        return result;
    }

    EGL_API EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
        LOG_D("eglGetConfigs, dpy: %p, configs: %p, config_size: %d, num_config: %p", dpy, configs, config_size,
              num_config);
        LOAD_EGL(eglGetConfigs)
        return egl_eglGetConfigs(realDpy(dpy), configs, config_size, num_config);
    }

    EGL_API EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs,
                                       EGLint config_size, EGLint* num_config) {
        LOG_D("eglChooseConfig, dpy: %p, attrib_list: %p, configs: %p, config_size: "
              "%d, num_config: %p",
              dpy, attrib_list, configs, config_size, num_config);
        LOAD_EGL(eglChooseConfig)
        std::vector<EGLint> backend_attributes;
        if (!makeBackendConfigAttributes(attrib_list, &backend_attributes)) {
            setFrontendError(EGL_BAD_ATTRIBUTE);
            return EGL_FALSE;
        }
        return egl_eglChooseConfig(realDpy(dpy), backend_attributes.data(), configs, config_size, num_config);
    }

    EGL_API EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
        LOG_D("eglGetConfigAttrib, dpy: %p, config: %p, attribute: %d, value: %p", dpy, config, attribute, value);
        LOAD_EGL(eglGetConfigAttrib)
        const EGLBoolean result = egl_eglGetConfigAttrib(realDpy(dpy), config, attribute, value);
        if (result == EGL_TRUE && value && (attribute == EGL_RENDERABLE_TYPE || attribute == EGL_CONFORMANT)) {
            *value = frontendRenderableMask(*value);
        }
        return result;
    }

    EGL_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                              const EGLint* attrib_list) {
        LOG_D("eglCreateWindowSurface, dpy: %p, config: %p, win: %p, attrib_list: %p", dpy, config, win, attrib_list);
        LOAD_EGL(eglCreateWindowSurface)
        return egl_eglCreateWindowSurface(realDpy(dpy), config, win, attrib_list);
    }

    EGL_API EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
        LOG_D("eglCreatePbufferSurface, dpy: %p, config: %p, attrib_list: %p", dpy, config, attrib_list);
        LOAD_EGL(eglCreatePbufferSurface)
        return egl_eglCreatePbufferSurface(realDpy(dpy), config, attrib_list);
    }

    EGL_API EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                              const EGLint* attrib_list) {
        LOG_D("eglCreatePixmapSurface, dpy: %p, config: %p, pixmap: %p, attrib_list: "
              "%p",
              dpy, config, pixmap, attrib_list);
        LOAD_EGL(eglCreatePixmapSurface)
        return egl_eglCreatePixmapSurface(realDpy(dpy), config, pixmap, attrib_list);
    }

    EGL_API EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
        LOG_D("eglDestroySurface, dpy: %p, surface: %p", dpy, surface);
        LOAD_EGL(eglDestroySurface)
        return egl_eglDestroySurface(realDpy(dpy), surface);
    }

    EGL_API EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint* value) {
        LOG_D("eglQuerySurface, dpy: %p, surface: %p, attribute: %d, value: %p", dpy, surface, attribute, value);
        LOAD_EGL(eglQuerySurface)
        return egl_eglQuerySurface(realDpy(dpy), surface, attribute, value);
    }

    EGL_API EGLBoolean eglBindAPI(EGLenum api) {
        LOG_D("eglBindAPI, api: %d", api);
        LOAD_EGL(eglBindAPI)
        const EGLenum backend_api = api == EGL_OPENGL_API ? EGL_OPENGL_ES_API : api;
        const EGLBoolean result = egl_eglBindAPI(backend_api);
        ETRACE("eglBindAPI(%s) -> backend %s: %s", inc_egl_api_name(api), inc_egl_api_name(backend_api),
               result == EGL_TRUE ? "ok" : "FAILED");
        if (result == EGL_TRUE) frontend_api = api;
        return result;
    }

    EGL_API EGLenum eglQueryAPI(void) {
        LOG_D("eglQueryAPI");
        return frontend_api;
    }

    EGL_API EGLBoolean eglWaitClient(void) {
        LOG_D("eglWaitClient");
        LOAD_EGL(eglWaitClient)
        return egl_eglWaitClient();
    }

    EGL_API EGLBoolean eglReleaseThread(void) {
        LOG_D("eglReleaseThread");
        LOAD_EGL(eglReleaseThread)
        const EGLBoolean result = egl_eglReleaseThread();
        ETRACE("eglReleaseThread -> %s", result == EGL_TRUE ? "ok" : "FAILED");
        if (result == EGL_TRUE) {

            inc_context_make_current(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            frontend_api = EGL_OPENGL_ES_API;
            frontend_error = EGL_SUCCESS;
        }
        return result;
    }

    EGL_API EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                                                        EGLConfig config, const EGLint* attrib_list) {
        LOG_D("eglCreatePbufferFromClientBuffer, dpy: %p, buftype: %d, buffer: %p, "
              "config: %p, attrib_list: %p",
              dpy, buftype, buffer, config, attrib_list);
        LOAD_EGL(eglCreatePbufferFromClientBuffer)
        return egl_eglCreatePbufferFromClientBuffer(realDpy(dpy), buftype, buffer, config, attrib_list);
    }

    EGL_API EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
        LOG_D("eglSurfaceAttrib, dpy: %p, surface: %p, attribute: %d, value: %d", dpy, surface, attribute, value);
        LOAD_EGL(eglSurfaceAttrib)
        return egl_eglSurfaceAttrib(realDpy(dpy), surface, attribute, value);
    }

    EGL_API EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        LOG_D("eglBindTexImage, dpy: %p, surface: %p, buffer: %d", dpy, surface, buffer);
        LOAD_EGL(eglBindTexImage)
        return egl_eglBindTexImage(realDpy(dpy), surface, buffer);
    }

    EGL_API EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        LOG_D("eglReleaseTexImage, dpy: %p, surface: %p, buffer: %d", dpy, surface, buffer);
        LOAD_EGL(eglReleaseTexImage)
        return egl_eglReleaseTexImage(realDpy(dpy), surface, buffer);
    }

    EGL_API EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
        LOG_D("eglSwapInterval, dpy: %p, interval: %d", dpy, interval);
        LOAD_EGL(eglSwapInterval)
        return egl_eglSwapInterval(realDpy(dpy), interval);
    }

    EGL_API EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                        const EGLint* attrib_list) {
        LOG_D("eglCreateContext, dpy: %p, config: %p, share_context: %p, "
              "attrib_list: %p",
              dpy, config, share_context, attrib_list);
        LOAD_EGL(eglCreateContext)
        if (frontend_api != EGL_OPENGL_API) {

            std::lock_guard<std::mutex> lifecycle(context_lifecycle_mutex);
            EGLContext es_context = egl_eglCreateContext(realDpy(dpy), config, share_context, attrib_list);
            ETRACE("eglCreateContext(ES, dpy=%p, share=%p) -> %p [%s]", dpy, share_context, es_context,
                   describeAttributes(attrib_list).c_str());
            if (es_context != EGL_NO_CONTEXT) {
                MGContext* record = inc_context_create(dpy, es_context, share_context, EGL_OPENGL_ES_API,
                                                      g_gles_caps.major, g_gles_caps.minor, 0, 0);
                ETRACE("  -> MGContext %llu", record ? record->id : 0ULL);
            }
            return es_context;
        }

        std::vector<EGLint> backend_attributes;
        EGLint frontend_major = 0;
        EGLint frontend_minor = 0;
        EGLint context_error = EGL_SUCCESS;
        EGLint frontend_flags = 0;
        if (!makeBackendContextAttributes(attrib_list, &backend_attributes, &frontend_major, &frontend_minor,
                                          &frontend_flags, &context_error)) {
            ETRACE("eglCreateContext(desktop, dpy=%p, share=%p) rejected before reaching the backend: %s [%s]", dpy,
                   share_context, inc_egl_error_name(context_error), describeAttributes(attrib_list).c_str());
            setFrontendError(context_error);
            return EGL_NO_CONTEXT;
        }

        LOAD_EGL(eglBindAPI)
        if (egl_eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            ETRACE("eglCreateContext(desktop, dpy=%p): backend eglBindAPI(ES) failed", dpy);
            return EGL_NO_CONTEXT;
        }

        std::lock_guard<std::mutex> lifecycle(context_lifecycle_mutex);
        EGLContext context = egl_eglCreateContext(realDpy(dpy), config, share_context, backend_attributes.data());
        ETRACE("eglCreateContext(desktop, dpy=%p, share=%p) -> %p, granted %d.%d [backend attrs: %s]", dpy,
               share_context, context, frontend_major, frontend_minor, describeAttributes(backend_attributes).c_str());
        if (context != EGL_NO_CONTEXT) {
            MGContext* record = inc_context_create(dpy, context, share_context, EGL_OPENGL_API, frontend_major,
                                                  frontend_minor, kVirtualDesktopProfileMask, frontend_flags);
            ETRACE("  -> MGContext %llu", record ? record->id : 0ULL);
        }
        return context;
    }

    EGL_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
        LOG_D("eglDestroyContext, dpy: %p, ctx: %p", dpy, ctx);
        LOAD_EGL(eglDestroyContext)
        std::lock_guard<std::mutex> lifecycle(context_lifecycle_mutex);
        MGContext* before = inc_context_find(ctx);
        const EGLBoolean result = egl_eglDestroyContext(realDpy(dpy), ctx);
        ETRACE("eglDestroyContext(dpy=%p, ctx=%p, MGContext=%llu) -> %s", dpy, ctx,
               before ? before->id : 0ULL, result == EGL_TRUE ? "ok" : "FAILED");
        if (result == EGL_TRUE) {

            inc_context_destroy(ctx);
        }
        return result;
    }

    EGL_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        LOG_D("eglMakeCurrent, dpy: %p, draw: %p, read: %p, ctx: %p", dpy, draw, read, ctx);
        LOAD_EGL(eglMakeCurrent)
        MGContext* before = inc_context_find(ctx);
        const EGLBoolean result = egl_eglMakeCurrent(realDpy(dpy), draw, read, ctx);
        ETRACE("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p, MGContext=%llu) -> %s", dpy, draw, read, ctx,
               before ? before->id : 0ULL, result == EGL_TRUE ? "ok" : "FAILED");

        if (result == EGL_TRUE) inc_context_make_current(dpy, draw, read, ctx);
        return result;
    }

    EGL_API EGLContext eglGetCurrentContext(void) {
        LOG_D("eglGetCurrentContext");
        LOAD_EGL(eglGetCurrentContext)
        return egl_eglGetCurrentContext();
    }

    EGL_API EGLSurface eglGetCurrentSurface(EGLint readdraw) {
        LOG_D("eglGetCurrentSurface, readdraw: %d", readdraw);
        LOAD_EGL(eglGetCurrentSurface)
        return egl_eglGetCurrentSurface(readdraw);
    }

    EGL_API EGLDisplay eglGetCurrentDisplay(void) {
        LOG_D("eglGetCurrentDisplay");
        LOAD_EGL(eglGetCurrentDisplay)
        return egl_eglGetCurrentDisplay();
    }

    EGL_API EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
        LOG_D("eglQueryContext, dpy: %p, ctx: %p, attribute: %d, value: %p", dpy, ctx, attribute, value);
        LOAD_EGL(eglQueryContext)
        const EGLBoolean result = egl_eglQueryContext(realDpy(dpy), ctx, attribute, value);
        if (result != EGL_TRUE || !value) return result;

        ETRACE("eglQueryContext(ctx=%p, attr=0x%x) backend value=0x%x", ctx, attribute, *value);

        const MGContext* record = inc_context_find(ctx);
        if (record != nullptr && record->client_type == EGL_OPENGL_API) {
            if (attribute == EGL_CONTEXT_CLIENT_TYPE) {
                *value = EGL_OPENGL_API;
                return EGL_TRUE;
            }
            if (attribute == EGL_CONTEXT_CLIENT_VERSION) {
                *value = record->granted_major;
                return EGL_TRUE;
            }
        }
        return result;
    }

    EGL_API EGLBoolean eglWaitGL(void) {
        LOG_D("eglWaitGL");
        LOAD_EGL(eglWaitGL)
        return egl_eglWaitGL();
    }

    EGL_API EGLBoolean eglWaitNative(EGLint engine) {
        LOG_D("eglWaitNative, engine: %d", engine);
        LOAD_EGL(eglWaitNative)
        return egl_eglWaitNative(engine);
    }

    EGL_API EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
        LOG_D("eglSwapBuffers, dpy: %p, surface: %p", dpy, surface);
        return presentSurface(dpy, surface);
    }

    EGL_API EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, EGLint* rects, EGLint n_rects) {
        LOG_D("eglSwapBuffersWithDamageKHR, dpy: %p, surface: %p, n_rects: %d", dpy, surface, n_rects);
        static const SwapWithDamageFn backend = resolveSwapWithDamage("eglSwapBuffersWithDamageKHR");
        return presentSurfaceWithDamage(dpy, surface, rects, n_rects, backend);
    }

    EGL_API EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface, EGLint* rects, EGLint n_rects) {
        LOG_D("eglSwapBuffersWithDamageEXT, dpy: %p, surface: %p, n_rects: %d", dpy, surface, n_rects);
        static const SwapWithDamageFn backend = resolveSwapWithDamage("eglSwapBuffersWithDamageEXT");
        return presentSurfaceWithDamage(dpy, surface, rects, n_rects, backend);
    }

    EGL_API EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
        LOG_D("eglCopyBuffers, dpy: %p, surface: %p, target: %p", dpy, surface, target);
        LOAD_EGL(eglCopyBuffers)
        return egl_eglCopyBuffers(realDpy(dpy), surface, target);
    }

    EGL_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list) {
        LOG_D("eglGetPlatformDisplay, platform: %d, native_display: %p, attrib_list: "
              "%p",
              platform, native_display, attrib_list);
        if (isForeignPlatform(platform)) {

            LOAD_EGL(eglGetDisplay)
            const EGLDisplay dpy = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);
            ETRACE("eglGetPlatformDisplay(0x%x, %p) -> %p (display padrão do backend)", platform, native_display, dpy);
            PGL_LOG("eglGetPlatformDisplay(plataforma 0x%x, nativo %p) -> %p (display padrao do backend)", platform,
                    native_display, dpy);
            return dpy;
        }
        LOAD_EGL_OR(eglGetPlatformDisplay, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_DISPLAY)
        return egl_eglGetPlatformDisplay(platform, native_display, attrib_list);
    }

    EGL_API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                      const EGLAttrib* attrib_list) {
        LOG_D("eglCreatePlatformWindowSurface, dpy: %p, config: %p, native_window: %p, attrib_list: %p", dpy, config,
              native_window, attrib_list);
        LOAD_EGL_OR(eglCreatePlatformWindowSurface, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_SURFACE)
        return egl_eglCreatePlatformWindowSurface(realDpy(dpy), config, native_window, attrib_list);
    }

    EGL_API EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                      const EGLAttrib* attrib_list) {
        LOG_D("eglCreatePlatformPixmapSurface, dpy: %p, config: %p, native_pixmap: %p, attrib_list: %p", dpy, config,
              native_pixmap, attrib_list);
        LOAD_EGL_OR(eglCreatePlatformPixmapSurface, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_SURFACE)
        return egl_eglCreatePlatformPixmapSurface(realDpy(dpy), config, native_pixmap, attrib_list);
    }

    EGL_API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void* native_display, const EGLint* attrib_list) {
        LOG_D("eglGetPlatformDisplayEXT, platform: %d, native_display: %p, attrib_list: %p", platform, native_display,
              attrib_list);
        if (isForeignPlatform(platform)) {
            LOAD_EGL(eglGetDisplay)
            const EGLDisplay dpy = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);
            ETRACE("eglGetPlatformDisplayEXT(0x%x, %p) -> %p (display padrão do backend)", platform, native_display, dpy);
            PGL_LOG("eglGetPlatformDisplayEXT(plataforma 0x%x, nativo %p) -> %p (display padrao do backend)", platform,
                    native_display, dpy);
            return dpy;
        }
        LOAD_EGL_OR(eglGetPlatformDisplayEXT, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_DISPLAY)
        return egl_eglGetPlatformDisplayEXT(platform, native_display, attrib_list);
    }

    EGL_API EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                         const EGLint* attrib_list) {
        LOG_D("eglCreatePlatformWindowSurfaceEXT, dpy: %p, config: %p, native_window: %p, attrib_list: %p", dpy, config,
              native_window, attrib_list);
        LOAD_EGL_OR(eglCreatePlatformWindowSurfaceEXT, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_SURFACE)
        return egl_eglCreatePlatformWindowSurfaceEXT(realDpy(dpy), config, native_window, attrib_list);
    }

    EGL_API EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                         const EGLint* attrib_list) {
        LOG_D("eglCreatePlatformPixmapSurfaceEXT, dpy: %p, config: %p, native_pixmap: %p, attrib_list: %p", dpy, config,
              native_pixmap, attrib_list);
        LOAD_EGL_OR(eglCreatePlatformPixmapSurfaceEXT, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_SURFACE)
        return egl_eglCreatePlatformPixmapSurfaceEXT(realDpy(dpy), config, native_pixmap, attrib_list);
    }

    EGL_API EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list) {
        LOG_D("eglCreateSync, dpy: %p, type: %d", dpy, type);
        LOAD_EGL_OR(eglCreateSync, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_SYNC)
        return egl_eglCreateSync(realDpy(dpy), type, attrib_list);
    }

    EGL_API EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync) {
        LOG_D("eglDestroySync, dpy: %p, sync: %p", dpy, sync);
        LOAD_EGL_OR(eglDestroySync, setFrontendError(EGL_BAD_PARAMETER), EGL_FALSE)
        return egl_eglDestroySync(realDpy(dpy), sync);
    }

    EGL_API EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
        LOG_D("eglClientWaitSync, dpy: %p, sync: %p", dpy, sync);
        LOAD_EGL_OR(eglClientWaitSync, setFrontendError(EGL_BAD_PARAMETER), EGL_FALSE)
        return egl_eglClientWaitSync(realDpy(dpy), sync, flags, timeout);
    }

    EGL_API EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
        LOG_D("eglGetSyncAttrib, dpy: %p, sync: %p, attribute: %d", dpy, sync, attribute);
        LOAD_EGL_OR(eglGetSyncAttrib, setFrontendError(EGL_BAD_PARAMETER), EGL_FALSE)
        return egl_eglGetSyncAttrib(realDpy(dpy), sync, attribute, value);
    }

    EGL_API EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
        LOG_D("eglWaitSync, dpy: %p, sync: %p", dpy, sync);
        LOAD_EGL_OR(eglWaitSync, setFrontendError(EGL_BAD_PARAMETER), EGL_FALSE)
        return egl_eglWaitSync(realDpy(dpy), sync, flags);
    }

    EGL_API EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                    const EGLAttrib* attrib_list) {
        LOG_D("eglCreateImage, dpy: %p, ctx: %p, target: %d", dpy, ctx, target);
        LOAD_EGL_OR(eglCreateImage, setFrontendError(EGL_BAD_PARAMETER), EGL_NO_IMAGE)
        return egl_eglCreateImage(realDpy(dpy), ctx, target, buffer, attrib_list);
    }

    EGL_API EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image) {
        LOG_D("eglDestroyImage, dpy: %p, image: %p", dpy, image);
        LOAD_EGL_OR(eglDestroyImage, setFrontendError(EGL_BAD_PARAMETER), EGL_FALSE)
        return egl_eglDestroyImage(realDpy(dpy), image);
    }

    EGL_API EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char* procname) {
        if (procname == nullptr) return nullptr;

        struct egl_entry_t {
            const char* name;
            void* fn;
        };
        static const egl_entry_t k_egl_entries[] = {
            {"eglBindAPI", (void*)eglBindAPI},
            {"eglBindTexImage", (void*)eglBindTexImage},
            {"eglChooseConfig", (void*)eglChooseConfig},
            {"eglClientWaitSync", (void*)eglClientWaitSync},
            {"eglCopyBuffers", (void*)eglCopyBuffers},
            {"eglCreateContext", (void*)eglCreateContext},
            {"eglCreateImage", (void*)eglCreateImage},
            {"eglCreatePbufferFromClientBuffer", (void*)eglCreatePbufferFromClientBuffer},
            {"eglCreatePbufferSurface", (void*)eglCreatePbufferSurface},
            {"eglCreatePixmapSurface", (void*)eglCreatePixmapSurface},
            {"eglCreatePlatformPixmapSurface", (void*)eglCreatePlatformPixmapSurface},
            {"eglCreatePlatformPixmapSurfaceEXT", (void*)eglCreatePlatformPixmapSurfaceEXT},
            {"eglCreatePlatformWindowSurface", (void*)eglCreatePlatformWindowSurface},
            {"eglCreatePlatformWindowSurfaceEXT", (void*)eglCreatePlatformWindowSurfaceEXT},
            {"eglCreateSync", (void*)eglCreateSync},
            {"eglCreateWindowSurface", (void*)eglCreateWindowSurface},
            {"eglDestroyContext", (void*)eglDestroyContext},
            {"eglDestroyImage", (void*)eglDestroyImage},
            {"eglDestroySurface", (void*)eglDestroySurface},
            {"eglDestroySync", (void*)eglDestroySync},
            {"eglGetConfigAttrib", (void*)eglGetConfigAttrib},
            {"eglGetConfigs", (void*)eglGetConfigs},
            {"eglGetCurrentContext", (void*)eglGetCurrentContext},
            {"eglGetCurrentDisplay", (void*)eglGetCurrentDisplay},
            {"eglGetCurrentSurface", (void*)eglGetCurrentSurface},
            {"eglGetDisplay", (void*)eglGetDisplay},
            {"eglGetError", (void*)eglGetError},
            {"eglGetPlatformDisplay", (void*)eglGetPlatformDisplay},
            {"eglGetPlatformDisplayEXT", (void*)eglGetPlatformDisplayEXT},
            {"eglGetProcAddress", (void*)eglGetProcAddress},
            {"eglGetSyncAttrib", (void*)eglGetSyncAttrib},
            {"eglInitialize", (void*)eglInitialize},
            {"eglMakeCurrent", (void*)eglMakeCurrent},
            {"eglQueryAPI", (void*)eglQueryAPI},
            {"eglQueryContext", (void*)eglQueryContext},
            {"eglQueryString", (void*)eglQueryString},
            {"eglQuerySurface", (void*)eglQuerySurface},
            {"eglReleaseTexImage", (void*)eglReleaseTexImage},
            {"eglReleaseThread", (void*)eglReleaseThread},
            {"eglSurfaceAttrib", (void*)eglSurfaceAttrib},
            {"eglSwapBuffers", (void*)eglSwapBuffers},
            {"eglSwapBuffersWithDamageEXT", (void*)eglSwapBuffersWithDamageEXT},
            {"eglSwapBuffersWithDamageKHR", (void*)eglSwapBuffersWithDamageKHR},
            {"eglSwapInterval", (void*)eglSwapInterval},
            {"eglTerminate", (void*)eglTerminate},
            {"eglWaitClient", (void*)eglWaitClient},
            {"eglWaitGL", (void*)eglWaitGL},
            {"eglWaitNative", (void*)eglWaitNative},
            {"eglWaitSync", (void*)eglWaitSync},
        };

        if (procname[0] == 'e' && procname[1] == 'g' && procname[2] == 'l') {
            for (const auto& e : k_egl_entries) {
                if (strcmp(procname, e.name) == 0) {
                    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(e.fn);
                }
            }

            LOAD_EGL(eglGetProcAddress);
            if (egl_eglGetProcAddress != nullptr) {
                auto fn = egl_eglGetProcAddress(procname);
                ETRACE("eglGetProcAddress(\"%s\"): not wrapped, backend resolved %p", procname, (void*)fn);
                return fn;
            }
            ETRACE("eglGetProcAddress(\"%s\"): not wrapped, backend has no eglGetProcAddress", procname);
            return nullptr;
        }

        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(glXGetProcAddress(procname));
    }
}


#include "egl_renderer.h"
#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

#ifndef EGL_ANDROID_NATIVE_BUFFER_ANDROID
#define EGL_ANDROID_NATIVE_BUFFER_ANDROID 0x3140
#endif

#include <android/log.h>

#include <cstring>
#include <algorithm>
#include <cstdint>
#include <sys/ioctl.h>
#include <signal.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <android/trace.h>
#include <android/data_space.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <unistd.h>

#include "atrace.h"

#define LOG_TAG "Piperella/egl"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct ConfigAttempt {
    const char* name;
    EGLint      attribs[24];
};

const ConfigAttempt kConfigLadder[] = {
    { "ES3 10-bit RGBA hw-accel",
      { EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,     EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_CONFIG_CAVEAT,    EGL_NONE,
        EGL_RED_SIZE,   10, EGL_GREEN_SIZE, 10,
        EGL_BLUE_SIZE,  10, EGL_ALPHA_SIZE,  2,
        EGL_DEPTH_SIZE,  0, EGL_STENCIL_SIZE, 0,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE } },
    { "ES3 8-bit RGBA hw-accel",
      { EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,     EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_CONFIG_CAVEAT,    EGL_NONE,
        EGL_RED_SIZE,    8, EGL_GREEN_SIZE,  8,
        EGL_BLUE_SIZE,   8, EGL_ALPHA_SIZE,  8,
        EGL_DEPTH_SIZE,  0, EGL_STENCIL_SIZE, 0,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE } },
    { "ES3 8-bit RGBA (any caveat)",
      { EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,     EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE,    8, EGL_GREEN_SIZE,  8,
        EGL_BLUE_SIZE,   8, EGL_ALPHA_SIZE,  8,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE } },
    { "ES2 8-bit RGBA fallback",
      { EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,     EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE,    8, EGL_GREEN_SIZE,  8,
        EGL_BLUE_SIZE,   8, EGL_ALPHA_SIZE,  8,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE } },
};

const char* kVS = R"(#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
uniform vec4 u_rect;      // x, y, w, h in NDC
uniform vec4 u_uv_rect;   // uv_offset_x, uv_offset_y, uv_scale_x, uv_scale_y
uniform mat2 u_uv_xform;  // wl_surface buffer_transform (identity = NORMAL)
out vec2 v_uv;
void main() {
    // Apply the buffer transform about the UV centre, then map into the
    // (viewport) source rect.
    vec2 buv = u_uv_xform * (a_uv - 0.5) + 0.5;
    v_uv = u_uv_rect.xy + buv * u_uv_rect.zw;
    vec2 pos = u_rect.xy + a_pos * u_rect.zw;
    gl_Position = vec4(pos, 0.0, 1.0);
})";

#define HDR_GLSL \
    "uniform bool u_hdr;\n" \
    "vec3 srgb_to_linear(vec3 c){\n" \
    "  return mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(0.04045, c));\n" \
    "}\n" \
    "vec3 pq_encode(vec3 L){\n" \
    "  const float m1=0.1593017578125, m2=78.84375;\n" \
    "  const float c1=0.8359375, c2=18.8515625, c3=18.6875;\n" \
    "  vec3 Lp=pow(max(L,0.0), vec3(m1));\n" \
    "  return pow((c1+c2*Lp)/(1.0+c3*Lp), vec3(m2));\n" \
    "}\n" \
    "vec4 hdr_map(vec4 c){\n" \
    "  if(!u_hdr) return c;\n" \
    "  const mat3 bt709_to_bt2020 = mat3(\n" \
    "    0.6274, 0.0691, 0.0164,\n" \
    "    0.3293, 0.9195, 0.0880,\n" \
    "    0.0433, 0.0114, 0.8956);\n" \
    "  vec3 lin = bt709_to_bt2020 * srgb_to_linear(c.rgb);\n" \
    "  return vec4(pq_encode(lin * (203.0/10000.0)), c.a);\n" \
    "}\n"

const char* kFS_2D = R"(#version 300 es
precision highp float;
in  vec2 v_uv;
uniform sampler2D u_tex;
out vec4 frag_color;
)" HDR_GLSL R"(
void main() {
    frag_color = hdr_map(texture(u_tex, v_uv));
})";

const char* kFS_OES = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
in  vec2 v_uv;
uniform samplerExternalOES u_tex;
out vec4 frag_color;
)" HDR_GLSL R"(
void main() {
    frag_color = hdr_map(vec4(texture(u_tex, v_uv).rgb, 1.0));
})";

const float* buffer_transform_uv_matrix(int transform) {
    static const float m[8][4] = {
        { 1.f,  0.f,  0.f,  1.f},
        { 0.f, -1.f,  1.f,  0.f},
        {-1.f,  0.f,  0.f, -1.f},
        { 0.f,  1.f, -1.f,  0.f},
        {-1.f,  0.f,  0.f,  1.f},
        { 0.f,  1.f,  1.f,  0.f},
        { 1.f,  0.f,  0.f, -1.f},
        { 0.f, -1.f, -1.f,  0.f},
    };
    if (transform < 0 || transform > 7) transform = 0;
    return m[transform];
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]={0};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint link_prog(const char* vs_src, const char* fs_src) {
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]={0};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOGE("link: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool ext_present(const char* exts, const char* name) {
    if (!exts) return false;
    size_t n = strlen(name);
    const char* p = exts;
    while ((p = strstr(p, name)) != nullptr) {
        if ((p == exts || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
        p += n;
    }
    return false;
}

#ifndef GL_TEXTURE_SWIZZLE_RGBA_COMPAT
#define GL_TEXTURE_SWIZZLE_RGBA_COMPAT 0x8E46
#endif
thread_local sigjmp_buf g_sigbus_jmp = {};
thread_local volatile sig_atomic_t g_sigbus_armed = 0;
void sigbus_handler(int) {
    if (g_sigbus_armed) siglongjmp(g_sigbus_jmp, 1);
}

[[maybe_unused]] uint32_t shm_format_from_wl(uint32_t wl_fmt) {
    switch (wl_fmt) {
        case WL_SHM_FORMAT_ARGB8888: return 0;
        case WL_SHM_FORMAT_XRGB8888: return 1;
        default: return wl_fmt;
    }
}

}

static EGLDisplay g_shared_egl_display = EGL_NO_DISPLAY;

extern "C" __attribute__((visibility("default"))) EGLDisplay piperella_get_egl_display() {
    return g_shared_egl_display;
}

EglRenderer::EglRenderer() = default;
EglRenderer::~EglRenderer() { teardown(); }

bool EglRenderer::create_egl_context() {
    ScopedTrace _t("EGL create_context");
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) { LOGE("eglGetDisplay"); return false; }
    EGLint maj=0, min=0;
    if (!eglInitialize(display_, &maj, &min)) {
        LOGE("eglInitialize: 0x%x", eglGetError());
        return false;
    }
    g_shared_egl_display = display_;
    LOGI("EGL %d.%d", maj, min);

    EGLConfig chosen = nullptr;
    const ConfigAttempt* picked = nullptr;
    for (const auto& a : kConfigLadder) {
        EGLint cnt = 0;
        EGLConfig c = nullptr;
        if (eglChooseConfig(display_, a.attribs, &c, 1, &cnt) && cnt > 0) {
            chosen = c;
            picked = &a;
            break;
        }
    }
    if (!chosen) { LOGE("no EGL config"); return false; }
    config_ = chosen;
    LOGI("EGL config: %s", picked->name);

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctx_attr);
    if (context_ == EGL_NO_CONTEXT) {
        ctx_attr[1] = 2;
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctx_attr);
    }
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext: 0x%x", eglGetError());
        return false;
    }

    const EGLint pbuf[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    dummy_pbuf_ = eglCreatePbufferSurface(display_, config_, pbuf);
    if (dummy_pbuf_ == EGL_NO_SURFACE) {
        LOGW("dummy pbuffer creation failed: 0x%x", eglGetError());
    }
    return true;
}

bool EglRenderer::create_window_surface(ANativeWindow* window) {
    EGLint vid = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &vid);
    ANativeWindow_setBuffersGeometry(window, 0, 0, vid);

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent: 0x%x", eglGetError());
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        return false;
    }
    return true;
}

void EglRenderer::probe_extensions() {
    const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
    ext_dmabuf_import_    = ext_present(exts, "EGL_EXT_image_dma_buf_import");
    ext_dmabuf_modifiers_ = ext_present(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    ext_swap_with_damage_ = ext_present(exts, "EGL_KHR_swap_buffers_with_damage")
                         || ext_present(exts, "EGL_EXT_swap_buffers_with_damage");
    ext_buffer_age_ = ext_present(exts, "EGL_EXT_buffer_age")
                   || ext_present(exts, "EGL_KHR_partial_update");

    if (ext_swap_with_damage_) {
        swap_with_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
            eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
        if (!swap_with_damage_) {
            swap_with_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
                eglGetProcAddress("eglSwapBuffersWithDamageEXT"));
        }
        if (!swap_with_damage_) ext_swap_with_damage_ = false;
    }

    create_image_  = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_tex_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (ext_dmabuf_import_) {
        if (!create_image_ || !destroy_image_ || !image_target_tex_) {
            ext_dmabuf_import_ = false;
            LOGW("dmabuf import procs missing — disabling EGL_EXT_image_dma_buf_import path");
        }
    }
    if (ext_dmabuf_modifiers_) {
        query_dmabuf_formats_ = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(
            eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        query_dmabuf_modifiers_ = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
            eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    }

    if (!ext_dmabuf_import_ && create_image_ && destroy_image_ && image_target_tex_) {
        optimistic_dmabuf_ = true;
        LOGW("EGL_EXT_image_dma_buf_import nao aparece na string de extensoes; tentando import de dma-buf mesmo assim");
    }

    bool has_android_nb = ext_present(exts, "EGL_ANDROID_image_native_buffer");
    get_native_client_buffer_ = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROID>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    ext_ahwb_import_ = has_android_nb
                    && get_native_client_buffer_ != nullptr
                    && create_image_  != nullptr
                    && destroy_image_ != nullptr
                    && image_target_tex_ != nullptr;

    LOGI("EGL exts: dmabuf=%d modifiers=%d ahwb=%d swap_with_damage=%d",
         ext_dmabuf_import_, ext_dmabuf_modifiers_, ext_ahwb_import_,
         ext_swap_with_damage_);
}

GLuint EglRenderer::program_for(GLenum target) {
    return target == GL_TEXTURE_EXTERNAL_OES ? program_oes_ : program_2d_;
}

bool EglRenderer::init(ANativeWindow* window, int width, int height) {
    if (!window) return false;
    width_ = width;
    height_ = height;
    if (display_ == EGL_NO_DISPLAY) {
        if (!create_egl_context()) return false;
        probe_extensions();

        {
            DIR* dir = opendir("/dev/dri");
            int best_fd = -1;
            bool have_render = false;
            if (dir) {
                struct dirent* ent;
                while ((ent = readdir(dir)) != nullptr) {
                    bool is_render = (strncmp(ent->d_name, "renderD", 7) == 0);
                    bool is_card   = (strncmp(ent->d_name, "card",    4) == 0);
                    if (!is_render && !is_card) continue;
                    if (is_card && have_render) continue;
                    char path[256];
                    snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
                    int fd = open(path, O_RDWR | O_CLOEXEC);
                    if (fd >= 0) {
                        if (best_fd >= 0) ::close(best_fd);
                        best_fd    = fd;
                        have_render = is_render;
                    }
                }
                closedir(dir);
            }
            if (best_fd < 0) {
                best_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
                if (best_fd < 0)
                    best_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
            }
            drm_fd_ = best_fd;
        }

        if (drm_fd_ >= 0) {
            struct DrmGetCap { uint64_t capability; uint64_t value; };
            DrmGetCap cap = { 0x13, 0 };
            if (ioctl(drm_fd_, _IOWR('d', 0x0c, DrmGetCap), &cap) == 0 && cap.value != 0)
                drm_syncobj_capable_ = true;
            LOGI("DRM syncobj capable: %d", drm_syncobj_capable_);
        }

        struct sigaction sa{};
        sa.sa_handler = sigbus_handler;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGBUS, &sa, nullptr);
    }
    if (!create_window_surface(window)) return false;

    if (!ext_bgra_) {
        const char* gl_exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        ext_bgra_ = gl_exts && strstr(gl_exts, "GL_EXT_texture_format_BGRA8888") != nullptr;
        LOGI("GL exts: bgra=%d", ext_bgra_);
    }

    if (!program_2d_)  program_2d_  = link_prog(kVS, kFS_2D);
    if (!program_oes_) program_oes_ = link_prog(kVS, kFS_OES);
    if (!program_2d_) {
        LOGE("failed to build sampler2D program");
        return false;
    }
    if (!program_oes_) {
        LOGW("failed to build samplerExternalOES program — dmabuf surfaces will not render");
    }

    auto cache_locs = [](GLuint prog, ProgLocs& locs) {
        if (!prog) return;
        locs.rect    = glGetUniformLocation(prog, "u_rect");
        locs.tex     = glGetUniformLocation(prog, "u_tex");
        locs.uv_rect = glGetUniformLocation(prog, "u_uv_rect");
        locs.uv_xform = glGetUniformLocation(prog, "u_uv_xform");
        locs.hdr     = glGetUniformLocation(prog, "u_hdr");
    };
    cache_locs(program_2d_,  locs_2d_);
    cache_locs(program_oes_, locs_oes_);

    if (!vbo_) {
        const float quad[] = {
            0.f, 0.f,   0.f, 1.f,
            1.f, 0.f,   1.f, 1.f,
            0.f, 1.f,   0.f, 0.f,
            1.f, 1.f,   1.f, 0.f,
        };
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    LOGI("EglRenderer init OK %dx%d", width_, height_);
    return true;
}

void EglRenderer::on_resize(int w, int h) {
    width_ = w; height_ = h;
}

bool EglRenderer::attach_window_output(uint64_t id, ANativeWindow* window,
                                       int width, int height) {
    if (!window || id == 0 || display_ == EGL_NO_DISPLAY || config_ == nullptr ||
        context_ == EGL_NO_CONTEXT) return false;
    auto existing = window_outputs_.find(id);
    if (existing != window_outputs_.end()) detach_window_output(id);

    EGLint visual = 0;
    if (!eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &visual))
        return false;
    ANativeWindow_acquire(window);
    ANativeWindow_setBuffersGeometry(window, 0, 0, visual);
    EGLSurface out = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (out == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface(client %llu): 0x%x",
             static_cast<unsigned long long>(id), eglGetError());
        ANativeWindow_release(window);
        return false;
    }
    WindowOutput slot{};
    slot.window = window;
    slot.surface = out;
    slot.width = width > 0 ? width : ANativeWindow_getWidth(window);
    slot.height = height > 0 ? height : ANativeWindow_getHeight(window);
    window_outputs_.emplace(id, slot);
    LOGI("per-toplevel EGL output attached id=%llu %dx%d",
         static_cast<unsigned long long>(id), slot.width, slot.height);
    return true;
}

void EglRenderer::detach_window_output(uint64_t id) {
    auto it = window_outputs_.find(id);
    if (it == window_outputs_.end()) return;
    if (display_ != EGL_NO_DISPLAY && it->second.surface != EGL_NO_SURFACE)
        eglDestroySurface(display_, it->second.surface);
    if (it->second.window) ANativeWindow_release(it->second.window);
    window_outputs_.erase(it);
}

void EglRenderer::resize_window_output(uint64_t id, int width, int height) {
    auto it = window_outputs_.find(id);
    if (it == window_outputs_.end() || width <= 0 || height <= 0) return;
    it->second.width = width;
    it->second.height = height;
}

void EglRenderer::render_scene_to_window_output(uint64_t id,
        const std::vector<RenderItem>& items, int64_t frame_time_ns) {
    auto it = window_outputs_.find(id);
    if (it == window_outputs_.end() || it->second.surface == EGL_NO_SURFACE) return;
    EGLSurface saved_surface = surface_;
    const int saved_width = width_, saved_height = height_;
    const int saved_age = buffer_age_;
    const bool saved_cursor = cursor_visible_;
    surface_ = it->second.surface;
    width_ = it->second.width;
    height_ = it->second.height;
    buffer_age_ = 0;
    cursor_visible_ = false;
    render_scene(items, nullptr, frame_time_ns);
    surface_ = saved_surface;
    width_ = saved_width;
    height_ = saved_height;
    buffer_age_ = saved_age;
    cursor_visible_ = saved_cursor;
    if (saved_surface != EGL_NO_SURFACE &&
        !eglMakeCurrent(display_, saved_surface, saved_surface, context_)) {
        LOGW("failed to restore primary EGL output after client output: 0x%x", eglGetError());
    }
}

bool EglRenderer::import_shm(Surface* s, wl_resource* shm_buffer) {
    if (!s || !shm_buffer) return false;
    wl_shm_buffer* shm = wl_shm_buffer_get(shm_buffer);
    if (!shm) return false;

    const int32_t  bw     = wl_shm_buffer_get_width(shm);
    const int32_t  bh     = wl_shm_buffer_get_height(shm);
    const int32_t  stride = wl_shm_buffer_get_stride(shm);
    const uint32_t fmt    = wl_shm_buffer_get_format(shm);
    if (bw <= 0 || bh <= 0 || stride < bw * 4) return false;

    GLint swz[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
    switch (fmt) {
        case WL_SHM_FORMAT_ARGB8888: swz[0] = GL_BLUE; swz[2] = GL_RED; break;
        case WL_SHM_FORMAT_XRGB8888: swz[0] = GL_BLUE; swz[2] = GL_RED; swz[3] = GL_ONE; break;
        case WL_SHM_FORMAT_ABGR8888:
        case WL_SHM_FORMAT_RGBA8888: break;
        case WL_SHM_FORMAT_XBGR8888:
        case WL_SHM_FORMAT_RGBX8888: swz[3] = GL_ONE; break;
        default:
            LOGW("Unsupported wl_shm format %u, defaulting to RGBA", fmt);
            break;
    }
    const int fmt_key = static_cast<int>(fmt) + 1;

    const bool fresh = (s->texture == 0);
    if (fresh) glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    if (fresh) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    const bool realloc = fresh || (bw != s->tex_w || bh != s->tex_h);
    const bool fmt_changed = fresh || s->tex_fmt != fmt_key;
    s->tex_w = bw;
    s->tex_h = bh;
    s->buffer_w = bw;
    s->buffer_h = bh;
    s->tex_target = GL_TEXTURE_2D;
    s->buffer_kind = BufferKind::Shm;
    if (fmt_changed) {
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA_COMPAT, swz);
        s->tex_fmt = fmt_key;
    }

    int ux = 0, uy = 0, uw = bw, uh = bh;
    if (!realloc && !s->damage_surface.empty() &&
        s->current.buffer_scale <= 1 && s->current.buffer_transform == 0 &&
        s->current.src_w <= 0 && s->current.dst_w <= 0) {
        int x1 = bw, y1 = bh, x2 = 0, y2 = 0;
        for (const auto& r : s->damage_surface) {
            if (r.empty()) continue;
            x1 = std::min(x1, std::max(0, r.x));
            y1 = std::min(y1, std::max(0, r.y));
            x2 = std::max(x2, std::min(bw, r.x + r.w));
            y2 = std::max(y2, std::min(bh, r.y + r.h));
        }
        if (x2 > x1 && y2 > y1) { ux = x1; uy = y1; uw = x2 - x1; uh = y2 - y1; }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    void* data = wl_shm_buffer_get_data(shm);
    if (!data) { glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); glBindTexture(GL_TEXTURE_2D, 0); return false; }

    g_sigbus_armed = 1;
    wl_shm_buffer_begin_access(shm);
    bool ok = true;
    if (sigsetjmp(g_sigbus_jmp, 1) == 0) {
        if (realloc) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        } else {
            const uint8_t* base = static_cast<const uint8_t*>(data) + static_cast<size_t>(uy) * stride + static_cast<size_t>(ux) * 4;
            glTexSubImage2D(GL_TEXTURE_2D, 0, ux, uy, uw, uh, GL_RGBA, GL_UNSIGNED_BYTE, base);
        }
    } else {
        LOGW("SIGBUS on shm buffer read — client truncated pool");
        ok = false;
    }
    wl_shm_buffer_end_access(shm);
    g_sigbus_armed = 0;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return ok;
}

bool EglRenderer::wait_acquire(Surface* s) {
    if (!s || s->acquire_point.syncobj_handle == 0) return true;
    if (drm_fd_ < 0 || !drm_syncobj_capable_) {
        LOGW("acquire sync point present but DRM syncobj wait is unavailable");
        return false;
    }
    uint32_t h = s->acquire_point.syncobj_handle;
    uint64_t pt = s->acquire_point.timeline_point;
    struct { uint64_t handles; uint64_t points; int64_t timeout_nsec;
             uint32_t count_handles; uint32_t flags; uint32_t first_signaled;
             uint32_t pad; } w = {};
    w.handles = reinterpret_cast<uint64_t>(&h);
    w.points = reinterpret_cast<uint64_t>(&pt);
    w.timeout_nsec = 2000000000LL;
    w.count_handles = 1;
    w.flags = (1u << 0) | (1u << 1);
#define DRM_IOCTL_BASE 'd'
#define DRM_IOC(nr,type) _IOC(_IOC_READ|_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof(type))
    if (ioctl(drm_fd_, DRM_IOC(0xCA, w), &w) != 0) {
        LOGE("acquire wait failed: %s", strerror(errno));
        return false;
    }
    s->acquire_point = {};
    return true;
}

bool EglRenderer::import_dmabuf(Surface* s,
        int width, int height, uint32_t fourcc, uint64_t modifier,
        const std::vector<int>& fds,
        const std::vector<uint32_t>& offsets,
        const std::vector<uint32_t>& strides,
        uint32_t flags) {
    if (!(ext_dmabuf_import_ || optimistic_dmabuf_) || !s || fds.empty()) return false;
    EGLint attrs[64];
    int i = 0;
    attrs[i++] = EGL_WIDTH;       attrs[i++] = width;
    attrs[i++] = EGL_HEIGHT;      attrs[i++] = height;
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[i++] = static_cast<EGLint>(fourcc);

    static const EGLint plane_fd_attr[]     = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint plane_offset_attr[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint plane_stride_attr[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint mod_lo_attr[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint mod_hi_attr[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };

    constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;
    bool send_mod = (modifier != DRM_FORMAT_MOD_INVALID) && ext_dmabuf_modifiers_;
    int planes = static_cast<int>(fds.size());
    for (int p = 0; p < planes && p < 4; ++p) {
        attrs[i++] = plane_fd_attr[p];     attrs[i++] = fds[p];
        attrs[i++] = plane_offset_attr[p]; attrs[i++] = static_cast<EGLint>(offsets[p]);
        attrs[i++] = plane_stride_attr[p]; attrs[i++] = static_cast<EGLint>(strides[p]);
        if (send_mod) {
            attrs[i++] = mod_lo_attr[p]; attrs[i++] = static_cast<EGLint>(modifier & 0xffffffffu);
            attrs[i++] = mod_hi_attr[p]; attrs[i++] = static_cast<EGLint>(modifier >> 32);
        }
    }
    attrs[i++] = EGL_NONE;

    EGLImageKHR image = create_image_(display_, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT,
                                      static_cast<EGLClientBuffer>(nullptr),
                                      attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        static bool logged = false;
        if (!logged) { logged = true; LOGW("eglCreateImageKHR(LINUX_DMA_BUF) failed: 0x%x (optimistic=%d)", eglGetError(), optimistic_dmabuf_); }
        return false;
    }

    if (!s->texture) glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s->texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_tex_(GL_TEXTURE_EXTERNAL_OES, image);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    if (s->dmabuf.egl_image != EGL_NO_IMAGE_KHR) {
        destroy_image_(display_, s->dmabuf.egl_image);
    }
    s->dmabuf.egl_image = image;
    if (&fds != &s->dmabuf.fds) {
        for (int oldfd : s->dmabuf.fds) if (oldfd >= 0) ::close(oldfd);
        s->dmabuf.fds      = fds;
        s->dmabuf.offsets  = offsets;
        s->dmabuf.strides  = strides;
    }
    s->dmabuf.width    = width;
    s->dmabuf.height   = height;
    s->dmabuf.fourcc   = fourcc;
    s->dmabuf.modifier = modifier;
    s->dmabuf.flags    = flags;
    s->buffer_w  = width;
    s->buffer_h  = height;
    s->tex_target = GL_TEXTURE_EXTERNAL_OES;
    s->buffer_kind = BufferKind::Dmabuf;
    return true;
}

bool EglRenderer::import_ahwb(Surface* s, AHardwareBuffer* ahwb,
                               int width, int height, uint32_t fourcc) {
    if (!ext_ahwb_import_ || !s || !ahwb) return false;
    EGLClientBuffer cb = get_native_client_buffer_(ahwb);
    if (!cb) {
        LOGW("import_ahwb: eglGetNativeClientBufferANDROID returned null");
        return false;
    }
    const EGLint attrs[] = { EGL_NONE };
    EGLImageKHR img = create_image_(display_, EGL_NO_CONTEXT,
                                    EGL_ANDROID_NATIVE_BUFFER_ANDROID, cb, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        LOGW("import_ahwb: eglCreateImageKHR(ANDROID_NATIVE_BUFFER) failed: 0x%x",
             eglGetError());
        return false;
    }
    if (!s->texture) glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s->texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_tex_(GL_TEXTURE_EXTERNAL_OES, img);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (s->dmabuf.egl_image != EGL_NO_IMAGE_KHR)
        destroy_image_(display_, s->dmabuf.egl_image);
    s->dmabuf.egl_image = img;
    s->dmabuf.width  = width;
    s->dmabuf.height = height;
    s->dmabuf.fourcc = fourcc;
    s->buffer_w    = width;
    s->buffer_h    = height;
    s->tex_target  = GL_TEXTURE_EXTERNAL_OES;
    s->buffer_kind = BufferKind::Dmabuf;
    LOGI("ZERO-COPY: AHWB/dmabuf import OK %dx%d fourcc=0x%x (EGL_ANDROID_NATIVE_BUFFER)",
         width, height, fourcc);
    return true;
}

#ifndef EGL_GL_COLORSPACE_KHR
#define EGL_GL_COLORSPACE_KHR 0x309D
#endif
#ifndef EGL_GL_COLORSPACE_SRGB_KHR
#define EGL_GL_COLORSPACE_SRGB_KHR 0x3089
#endif
#ifndef EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHRU_EXT
#define EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHRU_EXT 0x3493
#endif
#ifndef EGL_GL_COLORSPACE_BT2020_PQ_EXT
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT 0x3340
#endif
#ifndef EGL_GL_COLORSPACE_BT2020_LINEAR_EXT
#define EGL_GL_COLORSPACE_BT2020_LINEAR_EXT 0x333F
#endif

#ifndef ADATASPACE_BT2020_PQ
#define ADATASPACE_BT2020_PQ 0x9C60000
#endif
#ifndef ADATASPACE_DISPLAY_P3
#define ADATASPACE_DISPLAY_P3 0x88A0000
#endif

bool EglRenderer::set_hdr_color_space(ANativeWindow* window, int data_space,
                                       EGLint colorspace_attr) {
    if (!window) return false;

    hdr_pq_output_ = (colorspace_attr == EGL_GL_COLORSPACE_BT2020_PQ_EXT);

    if (data_space >= 0) {
        int32_t err = ANativeWindow_setBuffersDataSpace(window, data_space);
        if (err != 0) {
            LOGW("ANativeWindow_setBuffersDataSpace(%d) failed: %d",
                 data_space, err);
            return false;
        }
        LOGI("ANativeWindow_setBuffersDataSpace = %d", data_space);
    }

    if (display_ != EGL_NO_DISPLAY && config_ != nullptr && window) {
        if (surface_ != EGL_NO_SURFACE) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }

        EGLint attrs[3];
        const EGLint* attr_ptr = nullptr;
        if (colorspace_attr != 0) {
            attrs[0] = EGL_GL_COLORSPACE_KHR;
            attrs[1] = colorspace_attr;
            attrs[2] = EGL_NONE;
            attr_ptr = attrs;
        }

        surface_ = eglCreateWindowSurface(display_, config_, window, attr_ptr);
        if (surface_ == EGL_NO_SURFACE) {
            LOGW("eglCreateWindowSurface with colorspace=0x%x failed: 0x%x; "
                 "falling back to sRGB", colorspace_attr, eglGetError());
            hdr_pq_output_ = false;
            surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
            if (surface_ == EGL_NO_SURFACE) {
                LOGE("eglCreateWindowSurface fallback also failed: 0x%x",
                     eglGetError());
                if (dummy_pbuf_ != EGL_NO_SURFACE)
                    eglMakeCurrent(display_, dummy_pbuf_, dummy_pbuf_, context_);
                return false;
            }
        }

        if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
            LOGE("eglMakeCurrent after color space change: 0x%x", eglGetError());
            return false;
        }
        glViewport(0, 0, width_, height_);
    }

    return true;
}

void EglRenderer::set_frame_rate(ANativeWindow* window, float frame_rate_hz,
                                  int32_t compat) {
    if (!window) return;
    if (frame_rate_hz > 0.0f) {
        int32_t err = 0;
#if __ANDROID_API__ >= 30
        err = ANativeWindow_setFrameRate(window, frame_rate_hz, compat);
#else
        (void)window;
        (void)frame_rate_hz;
        (void)compat;
#endif
        if (err != 0) {
            LOGW("ANativeWindow_setFrameRate(%.1f Hz, compat=%d) err=%d",
                 frame_rate_hz, compat, err);
        } else {
            LOGI("ANativeWindow_setFrameRate: %.1f Hz (compat=%d)",
                 frame_rate_hz, compat);
        }
    } else {
        int32_t err = 0;
#if __ANDROID_API__ >= 31
        err = ANativeWindow_clearFrameRate(window);
#endif
        if (err != 0) {
            LOGW("ANativeWindow_clearFrameRate err=%d", err);
        }
    }
}

bool EglRenderer::build_dmabuf_feedback_table(State* st) {
    if (!ext_dmabuf_modifiers_ || !query_dmabuf_formats_
        || !query_dmabuf_modifiers_) {
        return false;
    }
    EGLint num_fmts = 0;
    if (!query_dmabuf_formats_(display_, 0, nullptr, &num_fmts) || num_fmts <= 0) {
        return false;
    }
    std::vector<EGLint> fmts(num_fmts);
    query_dmabuf_formats_(display_, num_fmts, fmts.data(), &num_fmts);

    constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;
    std::vector<std::pair<uint32_t, uint64_t>> all;
    for (EGLint f : fmts) {
        EGLint num_mods = 0;
        if (!query_dmabuf_modifiers_(display_, f, 0, nullptr, nullptr, &num_mods)
            || num_mods <= 0) {
            all.emplace_back(static_cast<uint32_t>(f), DRM_FORMAT_MOD_INVALID);
            continue;
        }
        std::vector<EGLuint64KHR> mods(num_mods);
        std::vector<EGLBoolean>   ext_only(num_mods);
        query_dmabuf_modifiers_(display_, f, num_mods, mods.data(),
                                ext_only.data(), &num_mods);
        for (int i = 0; i < num_mods; ++i) {
            all.emplace_back(static_cast<uint32_t>(f),
                             static_cast<uint64_t>(mods[i]));
        }
    }
    if (all.empty()) return false;

    size_t bytes = all.size() * 16;
    int fd = static_cast<int>(syscall(SYS_memfd_create, "dmabuf-feedback",
                                      MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) return false;
    if (ftruncate(fd, bytes) != 0) { ::close(fd); return false; }
    void* map = mmap(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { ::close(fd); return false; }
    auto* p = static_cast<uint8_t*>(map);
    for (auto& fm : all) {
        uint32_t f = fm.first;
        uint64_t m = fm.second;
        memcpy(p, &f, 4);
        memset(p+4, 0, 4);
        memcpy(p+8, &m, 8);
        p += 16;
    }
    munmap(map, bytes);

    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        LOGW("memfd sealing failed: %s (non-fatal)", strerror(errno));
    }

    if (st->dmabuf_feedback_fd >= 0) {
        ::close(st->dmabuf_feedback_fd);
        st->dmabuf_feedback_fd = -1;
    }
    size_t record_count = all.size();
    st->dmabuf_feedback_fd   = fd;
    st->dmabuf_feedback_size = bytes;
    st->dmabuf_formats       = std::move(all);

    {
        struct stat dst{};
        if (drm_fd_ >= 0 && fstat(drm_fd_, &dst) == 0)
            st->drm_device_id = static_cast<uint64_t>(dst.st_rdev);
        else
            st->drm_device_id = 0;
    }
    LOGI("dmabuf feedback table: %zu records", record_count);
    return true;
}

void EglRenderer::render_scene(const std::vector<RenderItem>& items,
                               const Rect* damage, int64_t ) {
    ScopedTrace _t("EGL render_scene");
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) {
        return;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGW("eglMakeCurrent in render_scene: 0x%x", eglGetError());
        return;
    }
    eglSwapInterval(display_, 0);
    glViewport(0, 0, width_, height_);
    if (buffer_age_ == 0 || damage == nullptr) {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    for (const auto& it : items) {
        Surface* s = it.surface;
        if (!s || !s->texture || s->buffer_kind == BufferKind::None) continue;
        GLuint prog = program_for(s->tex_target);
        if (!prog) continue;
        glUseProgram(prog);

        const ProgLocs& locs = (s->tex_target == GL_TEXTURE_EXTERNAL_OES)
                               ? locs_oes_ : locs_2d_;

        if (locs.rect < 0 || locs.tex < 0) {
            LOGE("Shader uniform location not found: u_rect=%d u_tex=%d",
                 locs.rect, locs.tex);
        }

        float uv_x = 0.f, uv_y = 0.f, uv_w = 1.f, uv_h = 1.f;
        if (s->buffer_w > 0 && s->buffer_h > 0 &&
            s->current.src_w > 0 && s->current.src_h > 0) {
            uv_x = static_cast<float>(s->current.src_x) / s->buffer_w;
            uv_y = static_cast<float>(s->current.src_y) / s->buffer_h;
            uv_w = static_cast<float>(s->current.src_w) / s->buffer_w;
            uv_h = static_cast<float>(s->current.src_h) / s->buffer_h;
        }
        if (s->buffer_kind == BufferKind::Dmabuf && (s->dmabuf.flags & 0x1u)) {
            uv_y = uv_y + uv_h;
            uv_h = -uv_h;
        }
        glUniform4f(locs.uv_rect, uv_x, uv_y, uv_w, uv_h);

        if (locs.uv_xform >= 0)
            glUniformMatrix2fv(locs.uv_xform, 1, GL_FALSE,
                               buffer_transform_uv_matrix(s->current.buffer_transform));

        float fx = (it.x   / static_cast<float>(width_))  * 2.f - 1.f;
        float fy = 1.f - ((it.y + it.h) / static_cast<float>(height_)) * 2.f;
        float fw = (it.w / static_cast<float>(width_))  * 2.f;
        float fh = (it.h / static_cast<float>(height_)) * 2.f;
        glUniform4f(locs.rect, fx, fy, fw, fh);
        glUniform1i(locs.tex, 0);
        if (locs.hdr >= 0) glUniform1i(locs.hdr, hdr_pq_output_ ? 1 : 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(s->tex_target, s->texture);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                              reinterpret_cast<void*>(2*sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    draw_cursor();

    if (ext_swap_with_damage_ && swap_with_damage_) {
        EGLint dmg[4];
        if (damage && !damage->empty()) {
            EGLint x0 = damage->x;
            EGLint y0 = height_ - (damage->y + damage->h);
            EGLint x1 = x0 + damage->w;
            EGLint y1 = y0 + damage->h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > width_)  x1 = width_;
            if (y1 > height_) y1 = height_;
            dmg[0] = x0; dmg[1] = y0;
            dmg[2] = (x1 > x0) ? (x1 - x0) : 0;
            dmg[3] = (y1 > y0) ? (y1 - y0) : 0;
        } else {
            dmg[0] = 0; dmg[1] = 0; dmg[2] = width_; dmg[3] = height_;
        }
        if (!swap_with_damage_(display_, surface_, dmg, 1)) {
            EGLint err = eglGetError();
            if (err != EGL_BAD_NATIVE_WINDOW)
                LOGW("eglSwapBuffersWithDamageKHR: 0x%x", err);
        }
    } else if (!eglSwapBuffers(display_, surface_)) {
        EGLint err = eglGetError();
        if (err != EGL_BAD_NATIVE_WINDOW) LOGW("eglSwapBuffers: 0x%x", err);
    }
    first_frame_done_ = true;

    if (ext_buffer_age_) {
        EGLint age = 0;
        if (!eglQuerySurface(display_, surface_, EGL_BUFFER_AGE_KHR, &age)) {
            buffer_age_ = 0;
        } else {
            buffer_age_ = age;
        }
    }
}

void EglRenderer::set_cursor(int x, int y, bool visible) {
    cursor_x_ = x; cursor_y_ = y; cursor_visible_ = visible;
}

void EglRenderer::ensure_cursor_texture() {
    if (cursor_tex_) return;
    static const char* A[] = {
        "X           ",
        "XX          ",
        "X.X         ",
        "X..X        ",
        "X...X       ",
        "X....X      ",
        "X.....X     ",
        "X......X    ",
        "X.......X   ",
        "X........X  ",
        "X.....XXXXX ",
        "X..X..X     ",
        "X.X X..X    ",
        "XX  X..X    ",
        "X    X..X   ",
        "     X..X   ",
        "      X.X   ",
        "      XXX   ",
    };
    const int H = 18, W = 12;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            uint8_t* p = &px[(static_cast<size_t>(r) * W + c) * 4];
            char ch = A[r][c];
            if (ch == 'X') { p[0]=0;   p[1]=0;   p[2]=0;   p[3]=255; }
            else if (ch == '.') { p[0]=255; p[1]=255; p[2]=255; p[3]=255; }
        }
    glGenTextures(1, &cursor_tex_);
    glBindTexture(GL_TEXTURE_2D, cursor_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    cursor_w_ = W * 3;
    cursor_h_ = H * 3;
}

void EglRenderer::draw_cursor() {
    if (!cursor_visible_ || width_ <= 0 || height_ <= 0) return;
    ensure_cursor_texture();
    if (!cursor_tex_) return;
    GLuint prog = program_for(GL_TEXTURE_2D);
    if (!prog) return;
    glUseProgram(prog);
    const ProgLocs& locs = locs_2d_;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform4f(locs.uv_rect, 0.f, 0.f, 1.f, 1.f);
    if (locs.uv_xform >= 0)
        glUniformMatrix2fv(locs.uv_xform, 1, GL_FALSE, buffer_transform_uv_matrix(0));
    float fx = (cursor_x_ / static_cast<float>(width_)) * 2.f - 1.f;
    float fy = 1.f - ((cursor_y_ + cursor_h_) / static_cast<float>(height_)) * 2.f;
    float fw = (cursor_w_ / static_cast<float>(width_)) * 2.f;
    float fh = (cursor_h_ / static_cast<float>(height_)) * 2.f;
    glUniform4f(locs.rect, fx, fy, fw, fh);
    glUniform1i(locs.tex, 0);
    if (locs.hdr >= 0) glUniform1i(locs.hdr, hdr_pq_output_ ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cursor_tex_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          reinterpret_cast<void*>(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void EglRenderer::render_frame(int64_t frame_time_ns) {
    ScopedTrace _t("EGL render_frame (empty)");
    render_scene({}, nullptr, frame_time_ns);
}

void EglRenderer::teardown() {
    for (auto& [id, output] : window_outputs_) {
        if (output.surface != EGL_NO_SURFACE && display_ != EGL_NO_DISPLAY)
            eglDestroySurface(display_, output.surface);
        if (output.window) ANativeWindow_release(output.window);
    }
    window_outputs_.clear();
    if (display_ == EGL_NO_DISPLAY) return;

    bool context_current = false;
    if (context_ != EGL_NO_CONTEXT) {
        EGLSurface draw = (surface_ != EGL_NO_SURFACE) ? surface_ : dummy_pbuf_;
        if (draw != EGL_NO_SURFACE) {
            context_current = eglMakeCurrent(display_, draw, draw, context_);
        }
    }

    if (context_current) {
        if (vbo_)        { glDeleteBuffers(1, &vbo_);    vbo_ = 0; }
        if (program_2d_)  { glDeleteProgram(program_2d_);  program_2d_  = 0; }
        if (program_oes_) { glDeleteProgram(program_oes_); program_oes_ = 0; }
    }

    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    if (dummy_pbuf_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, dummy_pbuf_);
        dummy_pbuf_ = EGL_NO_SURFACE;
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }

    if (drm_fd_ >= 0) { ::close(drm_fd_); drm_fd_ = -1; }

    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;
}

void EglRenderer::drain_pending_deletes(std::vector<GLuint>& queue) {
    if (queue.empty()) return;
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
        EGLSurface draw = (surface_ != EGL_NO_SURFACE) ? surface_ : dummy_pbuf_;
        if (draw != EGL_NO_SURFACE) {
            eglMakeCurrent(display_, draw, draw, context_);
        }
    }
    for (GLuint tex : queue) {
        if (tex) glDeleteTextures(1, &tex);
    }
    queue.clear();
}

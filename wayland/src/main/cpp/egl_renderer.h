// egl_renderer.h — EGL/GLES3 backend for the Piperella compositor.
//
// Owns EGLDisplay, EGLContext, the on-screen EGLSurface bound to the
// SurfaceView's ANativeWindow, and a 1×1 pbuffer used for the
// `eglMakeCurrent(EGL_NO_SURFACE)` workaround documented in COMPOSITOR.md.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct Surface;
struct State;

struct RenderItem {
    Surface* surface = nullptr;
    int x = 0, y = 0;
    int w = 0, h = 0;
};

class EglRenderer {
public:
    EglRenderer();
    ~EglRenderer();

    EglRenderer(const EglRenderer&) = delete;
    EglRenderer& operator=(const EglRenderer&) = delete;

    bool init(ANativeWindow* window, int width, int height);
    void teardown();
    void on_resize(int width, int height);

    // Each xdg_toplevel gets a dedicated Android Surface/EGLSurface while
    // sharing this renderer's EGL context and imported Wayland buffer textures.
    bool attach_window_output(uint64_t id, ANativeWindow* window, int width, int height);
    void detach_window_output(uint64_t id);
    void resize_window_output(uint64_t id, int width, int height);
    void render_scene_to_window_output(uint64_t id, const std::vector<RenderItem>& items,
                                      int64_t frame_time_ns);
    bool has_window_outputs() const { return !window_outputs_.empty(); }

    // Compose every renderable surface in z-order. `damage` is in output
    // coords; null means "no incremental damage info — full clear+swap".
    void render_scene(const std::vector<RenderItem>& items,
                      const struct Rect* damage,
                      int64_t frame_time_ns);

    // Backwards-compatible single-frame entrypoint (no scene). Used by
    // labwc.cpp when the scene graph is empty so we still touch the GL
    // context periodically.
    void render_frame(int64_t frame_time_ns);

    bool needs_first_clear() const { return !first_frame_done_; }
    // Embedded mode: the primary output is only a lifetime anchor that is never
    // shown, so skip its clear + swap entirely. Still marks the first-clear done.
    void skip_primary_frame() { first_frame_done_ = true; }

    int width()  const { return width_;  }
    int height() const { return height_; }

    // Probed extension capabilities — read-only.
    bool has_swap_with_damage() const { return ext_swap_with_damage_; }
    bool has_dmabuf_import()    const { return ext_dmabuf_import_ || optimistic_dmabuf_; }
    bool has_dmabuf_modifiers() const { return ext_dmabuf_modifiers_; }
    bool has_ahwb_import()      const { return ext_ahwb_import_; }
    bool has_ext_bgra()         const { return ext_bgra_; }
    int buffer_age() const { return buffer_age_; }

    // Drain a pending-texture-delete queue on the render thread (EGL current).
    // Call from the render thread (or from teardown) before rendering/cleanup.
    void drain_pending_deletes(std::vector<GLuint>& queue);

    // wl_shm upload path.
    bool import_shm(Surface* s, struct wl_resource* shm_buffer);

    // dmabuf import (M5.2). Returns true on success and stamps surface->dmabuf.
    // Wait the current Wayland explicit-sync acquire point before any backend
    // (GLES or Vulkan) samples the committed client buffer.
    bool wait_acquire(Surface* s);

    bool import_dmabuf(Surface* s,
                       int width, int height, uint32_t fourcc, uint64_t modifier,
                       const std::vector<int>& fds,
                       const std::vector<uint32_t>& offsets,
                       const std::vector<uint32_t>& strides,
                       uint32_t flags);

    // AHardwareBuffer import path — uses EGL_ANDROID_NATIVE_BUFFER_ANDROID
    // (always available on Adreno vendor EGL) instead of EGL_LINUX_DMA_BUF_EXT.
    // This is the compositor-side import for buffers allocated via ahwb_pool.
    bool import_ahwb(Surface* s, AHardwareBuffer* ahwb,
                     int width, int height, uint32_t fourcc);

    // Build a {format, modifier} table on a memfd and store
    // st->dmabuf_feedback_{fd,size,formats}. Called once at init.
    bool build_dmabuf_feedback_table(State* st);

    // --- HDR support ---
    // Recreate the EGL window surface with HDR color space attributes and
    // set the ANativeWindow buffer data space. Returns true on success.
    bool set_hdr_color_space(ANativeWindow* window, int data_space,
                             EGLint colorspace_attr);

    // --- VRR support ---
    // Call ANativeWindow_setFrameRate to hint the display refresh rate.
    // frame_rate_hz: target rate (0 = clear). compat: FRAME_RATE_COMPATIBILITY_*.
    void set_frame_rate(ANativeWindow* window, float frame_rate_hz, int32_t compat);

    // Trackpad cursor drawn by the compositor (client cursors are still TODO).
    // The app's trackpad mode positions it each frame; (x,y) is the arrow tip.
    void set_cursor(int x, int y, bool visible);

    EGLDisplay egl_display() const { return display_; }
    int drm_fd() const { return drm_fd_; }
    // C06-11: expose DRM_CAP_SYNCOBJ probe result so callers can gate syncobj ioctls.
    bool drm_syncobj_capable() const { return drm_syncobj_capable_; }

private:
    bool create_egl_context();
    bool create_window_surface(ANativeWindow* window);
    void probe_extensions();
    GLuint program_for(GLenum target);

    // Compositor-drawn trackpad cursor.
    void ensure_cursor_texture();
    void draw_cursor();
    GLuint cursor_tex_ = 0;
    int  cursor_w_ = 0, cursor_h_ = 0;
    int  cursor_x_ = 0, cursor_y_ = 0;
    bool cursor_visible_ = false;

    // C06-08: cached uniform locations per shader program (populated after link).
    struct ProgLocs {
        GLint rect    = -1;
        GLint tex     = -1;
        GLint uv_rect = -1;
        GLint uv_xform = -1;
        GLint hdr     = -1;
    };

    struct WindowOutput {
        ANativeWindow* window = nullptr; // acquired reference
        EGLSurface surface = EGL_NO_SURFACE;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<uint64_t, WindowOutput> window_outputs_;

    EGLDisplay display_     = EGL_NO_DISPLAY;
    EGLConfig  config_      = nullptr;
    EGLContext context_     = EGL_NO_CONTEXT;
    EGLSurface surface_     = EGL_NO_SURFACE;
    EGLSurface dummy_pbuf_  = EGL_NO_SURFACE;

    int  width_  = 0;
    int  height_ = 0;
    bool first_frame_done_ = false;
    int  buffer_age_ = 0;
    // True while the output surface is BT.2020 PQ — the composite shaders then
    // color-convert SDR client content into PQ instead of writing it verbatim.
    bool hdr_pq_output_ = false;

    GLuint program_2d_ = 0;
    GLuint program_oes_ = 0;
    GLuint vbo_ = 0;

    // C06-08: per-program cached locations (populated once after link_prog).
    ProgLocs locs_2d_;
    ProgLocs locs_oes_;

    bool ext_swap_with_damage_ = false;
    bool ext_dmabuf_import_    = false;
    bool ext_dmabuf_modifiers_ = false;
    bool ext_ahwb_import_      = false;   // EGL_ANDROID_image_native_buffer
    // libEGL do Android filtra a string de extensões (EGL_EXT_image_dma_buf_import costuma
    // sumir), mas o driver vendor aceita EGL_LINUX_DMA_BUF_EXT em eglCreateImageKHR.
    bool optimistic_dmabuf_    = false;
    bool ext_bgra_             = false;
    bool ext_buffer_age_       = false;   // C06-05: EGL_EXT_buffer_age or KHR_partial_update

    int  drm_fd_              = -1;
    bool drm_syncobj_capable_ = false;    // C06-11: DRM_CAP_SYNCOBJ confirmed at init

    // EGL extension fn pointers (resolved at init).
    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC swap_with_damage_ = nullptr;
    PFNEGLCREATEIMAGEKHRPROC           create_image_     = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC          destroy_image_    = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_tex_ = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC     query_dmabuf_formats_ = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC   query_dmabuf_modifiers_ = nullptr;
    // EGL_ANDROID_get_native_client_buffer — converts AHardwareBuffer → EGLClientBuffer.
    typedef EGLClientBuffer (*PFNEGLGETNATIVECLIENTBUFFERANDROID)(const AHardwareBuffer*);
    PFNEGLGETNATIVECLIENTBUFFERANDROID  get_native_client_buffer_ = nullptr;
};

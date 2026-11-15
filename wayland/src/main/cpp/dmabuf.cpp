// dmabuf.cpp — zwp_linux_dmabuf_v1 v5 + zwp_linux_dmabuf_feedback_v1.
//
// v4+ deprecates the per-format `format`/`modifier` events on the dmabuf
// global; clients call `get_default_feedback`/`get_surface_feedback` to get
// a feedback object instead. We send: main_device → format_table memfd →
// one tranche (target=main_device, all formats) → tranche_done → done.
//
// C07-05: wl_buffer creation uses deferred import — EGL image is built at
// render time (when a Surface* is available), not at wl_buffer creation.

#include "labwc.h"
#include "egl_renderer.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "linux-dmabuf-protocol.h"
}

#include <android/log.h>

#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOG_TAG "Piperella/dmabuf"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;

// "No explicit modifier" sentinel (drm_fourcc.h). Stored on buffers created
// without a modifier so the importer omits the EGL modifier attributes rather
// than passing this (import-failing) value as a real modifier.
static constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;

// The wl_buffer implementation for dmabuf-backed buffers. File-scope so its
// address is stable and usable as the wl_resource_instance_of discriminator.
static const struct wl_buffer_interface g_dmabuf_buffer_impl = {
    [](wl_client*, wl_resource* r){ wl_resource_destroy(r); },
};

// C07-05: DmabufBuffer accessor. The tag check alone is NOT a safe type
// discriminator — a wl_buffer of another kind (e.g. wp_single_pixel_buffer,
// whose user_data's first word is client-controlled) can forge the tag and be
// reinterpreted as a DmabufBuffer, reading a std::vector control block past the
// end of a smaller struct (OOB read → dup()/close() of garbage fds → crash).
// Gate on the actual wl_buffer implementation first.
DmabufBuffer* dmabuf_buffer_from_resource(wl_resource* r) {
    if (!r) return nullptr;
    if (!wl_resource_instance_of(r, &wl_buffer_interface, &g_dmabuf_buffer_impl))
        return nullptr;
    void* ud = wl_resource_get_user_data(r);
    if (!ud) return nullptr;
    auto* db = static_cast<DmabufBuffer*>(ud);
    if (db->tag != DmabufBuffer::TAG) return nullptr;
    return db;
}

namespace {

constexpr uint32_t DMABUF_VERSION = 5;

// ---------- zwp_linux_buffer_params_v1 -------------------------------------

struct Params {
    int width = 0, height = 0;
    uint32_t fourcc = 0, flags = 0;
    uint64_t modifier = 0;
    bool modifier_set = false;       // v5+: track if modifier has been set
    bool used = false;               // C07-03: track if create/create_immed called
    std::vector<int> fds;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> strides;
};

Params* params_from(wl_resource* r) {
    return static_cast<Params*>(wl_resource_get_user_data(r));
}
void params_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void params_add(wl_client*, wl_resource* r, int32_t fd, uint32_t plane_idx,
                uint32_t offset, uint32_t stride,
                uint32_t modifier_hi, uint32_t modifier_lo) {
    Params* p = params_from(r);
    if (!p) { ::close(fd); return; }
    if (plane_idx >= 4) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index %u out of range", plane_idx);
        ::close(fd);
        return;
    }
    if (p->fds.size() <= plane_idx) {
        p->fds.resize(plane_idx + 1, -1);
        p->offsets.resize(plane_idx + 1, 0);
        p->strides.resize(plane_idx + 1, 0);
    }

    // C07-02: error on duplicate plane with PLANE_SET code.
    if (p->fds[plane_idx] >= 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane index %u already set", plane_idx);
        ::close(fd);
        return;
    }

    uint64_t new_mod = (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
    // v5+: all planes must use the same modifier.
    if (p->modifier_set && new_mod != p->modifier) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                               "all planes must use the same modifier");
        ::close(fd);
        return;
    }
    p->fds[plane_idx]     = fd;
    p->offsets[plane_idx] = offset;
    p->strides[plane_idx] = stride;
    p->modifier   = new_mod;
    p->modifier_set = true;
}

// C07-05: destroy callback for wl_buffer resources backed by DmabufBuffer.
static void dmabuf_buffer_resource_destroy(wl_resource* r) {
    DmabufBuffer* db = static_cast<DmabufBuffer*>(wl_resource_get_user_data(r));
    if (!db) return;
    for (int fd : db->fds) if (fd >= 0) ::close(fd);
    delete db;
}

void create_buffer_common(wl_client* c, wl_resource* r, uint32_t id,
                          int32_t w, int32_t h, uint32_t fourcc, uint32_t flags,
                          bool immediate) {
    Params* p = params_from(r);
    if (!p) return;

    // C07-03: enforce ALREADY_USED.
    if (p->used) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params object already used");
        return;
    }
    p->used = true;

    // C07-07: validate completeness and format before creating the wl_buffer.
    if (p->fds.empty()) {
        if (immediate) {
            wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                                   "no planes added");
        } else {
            zwp_linux_buffer_params_v1_send_failed(r);
        }
        return;
    }
    if (fourcc == 0) {
        if (immediate) {
            wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                                   "fourcc is zero");
        } else {
            zwp_linux_buffer_params_v1_send_failed(r);
        }
        return;
    }

    // A plane gap (e.g. plane 0 and 2 added, 1 left as -1) would hand a -1 fd
    // to EGL_DMA_BUF_PLANE*_FD_EXT. Spec requires INCOMPLETE.
    for (size_t pi = 0; pi < p->fds.size(); ++pi) {
        if (p->fds[pi] < 0) {
            if (immediate) {
                wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                                       "plane %zu missing", pi);
            } else {
                zwp_linux_buffer_params_v1_send_failed(r);
            }
            return;
        }
    }

    p->width = w; p->height = h; p->fourcc = fourcc; p->flags = flags;

    // C07-05: deferred import — store params + dup'd fds in DmabufBuffer.
    // The EGL image is built at render time when a Surface* is available.
    auto* db = new DmabufBuffer();
    db->width    = w;
    db->height   = h;
    db->fourcc   = fourcc;
    db->flags    = flags;
    // Preserve "implicit modifier" as INVALID so the importer omits the EGL
    // modifier attrs rather than forcing LINEAR(0) on an implicit buffer.
    db->modifier = p->modifier_set ? p->modifier : DRM_FORMAT_MOD_INVALID;
    db->offsets  = p->offsets;
    db->strides  = p->strides;
    for (int fd : p->fds) {
        db->fds.push_back(fd >= 0 ? ::dup(fd) : -1);
    }

    // C07-04: create the wl_buffer AFTER successful validation.
    wl_resource* buf = wl_resource_create(c, &wl_buffer_interface, 1,
                                          immediate ? id : 0);
    if (!buf) {
        for (int fd : db->fds) if (fd >= 0) ::close(fd);
        delete db;
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(buf, &g_dmabuf_buffer_impl, db, dmabuf_buffer_resource_destroy);

    if (!immediate) zwp_linux_buffer_params_v1_send_created(r, buf);
}

void params_create(wl_client* c, wl_resource* r, int32_t w, int32_t h,
                   uint32_t fourcc, uint32_t flags) {
    if (w <= 0 || h <= 0) {
        zwp_linux_buffer_params_v1_send_failed(r);
        return;
    }
    create_buffer_common(c, r, 0, w, h, fourcc, flags, false);
}
void params_create_immed(wl_client* c, wl_resource* r, uint32_t id,
                         int32_t w, int32_t h, uint32_t fourcc, uint32_t flags) {
    if (w <= 0 || h <= 0) {
        // C07-07: INVALID_DIMENSIONS for create_immed.
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                               "invalid dimensions %dx%d", w, h);
        return;
    }
    create_buffer_common(c, r, id, w, h, fourcc, flags, true);
}

const struct zwp_linux_buffer_params_v1_interface params_impl = {
    params_destroy,
    params_add,
    params_create,
    params_create_immed,
};

void params_resource_destroy(wl_resource* r) {
    Params* p = params_from(r);
    if (!p) return;
    for (int fd : p->fds) if (fd >= 0) ::close(fd);
    delete p;
}

// ---------- feedback object -------------------------------------------------

void send_feedback(State* st, wl_resource* fb) {
    if (st->dmabuf_feedback_fd < 0) return;
    wl_array dev;
    wl_array_init(&dev);
    auto* p = wl_array_add(&dev, sizeof(uint64_t));
    if (p) memcpy(p, &st->drm_device_id, sizeof(uint64_t));
    zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &dev);
    zwp_linux_dmabuf_feedback_v1_send_format_table(fb,
        st->dmabuf_feedback_fd,
        static_cast<uint32_t>(st->dmabuf_feedback_size));
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &dev);

    // C07-01: spec-mandated event order:
    //   tranche_target_device → tranche_flags → tranche_formats → tranche_done
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);

    wl_array idx;
    wl_array_init(&idx);
    for (size_t i = 0; i < st->dmabuf_formats.size(); ++i) {
        uint16_t v = static_cast<uint16_t>(i);
        auto* w = wl_array_add(&idx, sizeof(uint16_t));
        if (w) memcpy(w, &v, sizeof(uint16_t));
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
    wl_array_release(&idx);
    wl_array_release(&dev);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);
}

void feedback_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    feedback_destroy,
};

// ---------- zwp_linux_dmabuf_v1 ---------------------------------------------

void dmabuf_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void dmabuf_create_params(wl_client* c, wl_resource* parent, uint32_t id) {
    auto* p = new Params();
    // Child objects inherit the parent's bound version (Wayland convention).
    wl_resource* r = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
                                        wl_resource_get_version(parent), id);
    if (!r) { delete p; wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &params_impl, p, params_resource_destroy);
}

void dmabuf_get_default_feedback(wl_client* c, wl_resource* parent, uint32_t id) {
    auto* st = g_state;
    if (!st) return;
    wl_resource* r = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface,
                                        wl_resource_get_version(parent), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &feedback_impl, nullptr, nullptr);
    send_feedback(st, r);
}

void dmabuf_get_surface_feedback(wl_client* c, wl_resource* dmabuf_res, uint32_t id,
                                 wl_resource* surface_res) {
    // C07-A1: validate surface_res before using it.
    if (!surface_res) {
        LOGW("get_surface_feedback: null surface resource");
    } else {
        void* ud = wl_resource_get_user_data(surface_res);
        if (!ud) {
            LOGW("get_surface_feedback: surface resource has null user_data");
        }
    }
    // Per spec, surface-specific feedback falls back to default when per-surface
    // feedback is not available (we don't have it yet).
    dmabuf_get_default_feedback(c, dmabuf_res, id);
}

const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    dmabuf_destroy,
    dmabuf_create_params,
    dmabuf_get_default_feedback,
    dmabuf_get_surface_feedback,
};

void dmabuf_bind(wl_client* c, void* /*data*/, uint32_t version, uint32_t id) {
    uint32_t v = version < DMABUF_VERSION ? version : DMABUF_VERSION;
    wl_resource* r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);
    // v4+ clients use feedback objects, so the format/modifier events are
    // deprecated for them. But v1–v3 clients learn supported formats ONLY from
    // these events — without them an older client sees zero formats. Emit them.
    auto* st = g_state;
    if (v < 4 && st) {
        for (const auto& fm : st->dmabuf_formats) {
            // v1 format event: advertise each format once (skip duplicates from
            // multiple modifiers). INVALID modifier still implies the format is
            // usable with an implicit modifier.
            zwp_linux_dmabuf_v1_send_format(r, fm.first);
            if (v >= 3 && fm.second != DRM_FORMAT_MOD_INVALID) {
                zwp_linux_dmabuf_v1_send_modifier(r, fm.first,
                    static_cast<uint32_t>(fm.second >> 32),
                    static_cast<uint32_t>(fm.second & 0xffffffffu));
            }
        }
    }
    LOGI("zwp_linux_dmabuf_v1 bound (version=%u)", v);
}

}  // namespace

// Build a minimal hardcoded format table when we have AHardwareBuffer import
// but no vendor EGL dmabuf support. Advertises ABGR8888 with implicit modifier
// (DRM_FORMAT_MOD_INVALID) so clients know what format to use.
static bool build_ahwb_format_table(State* st) {
    // DRM_FORMAT_ABGR8888 = 0x34324241 (matches AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)
    constexpr uint32_t DRM_FORMAT_ABGR8888 = 0x34324241u;
    constexpr uint32_t DRM_FORMAT_XBGR8888 = 0x34324258u;
    constexpr uint32_t DRM_FORMAT_ARGB8888 = 0x34325241u;
    constexpr uint32_t DRM_FORMAT_XRGB8888 = 0x34325258u;
    st->dmabuf_formats = {
        { DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_XBGR8888, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID },
        { DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID },
    };

    // 16-byte record: u32 format, u8[4] pad, u64 modifier.
    size_t bytes = st->dmabuf_formats.size() * 16;
    int fd = static_cast<int>(syscall(SYS_memfd_create, "dmabuf-feedback-ahwb",
                                      MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) return false;
    if (ftruncate(fd, bytes) != 0) { ::close(fd); return false; }
    void* map = mmap(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { ::close(fd); return false; }
    auto* p = static_cast<uint8_t*>(map);
    for (const auto& fm : st->dmabuf_formats) {
        uint32_t f = fm.first;
        uint64_t m = fm.second;
        memcpy(p, &f, 4);
        memset(p+4, 0, 4);
        memcpy(p+8, &m, 8);
        p += 16;
    }
    munmap(map, bytes);
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);

    if (st->dmabuf_feedback_fd >= 0) ::close(st->dmabuf_feedback_fd);
    st->dmabuf_feedback_fd   = fd;
    st->dmabuf_feedback_size = bytes;
    st->drm_device_id        = 0;
    LOGI("dmabuf: built default format table (ABGR/XBGR/ARGB/XRGB8888 + implicit modifier)");
    return true;
}

bool dmabuf_globals_register(State* st) {
    if (st->g_dmabuf) return true;
    // O global é anunciado SEMPRE, mesmo no modo headless (sem renderer ainda).
    // Antes ele só existia depois que um EglRenderer subia e declarava suporte a
    // dma-buf/AHWB; clientes que conectam antes (ou num aparelho cuja string de
    // extensões do EGL esconde EGL_EXT_image_dma_buf_import) nunca viam o global e
    // o libhybris-wayland falhava com "o compositor nao anuncia zwp_linux_dmabuf_v1".
    // A importação real continua sendo feita (e pode falhar com log) na hora de
    // desenhar; aqui só garantimos que o cliente consegue criar buffers.
    if (!st->renderer) {
        LOGI("dmabuf: sem renderer ainda — anunciando o global com tabela padrão (modo headless)");
    } else if (!st->renderer->has_dmabuf_import() && !st->renderer->has_ahwb_import()) {
        LOGW("dmabuf: EGL sem import de dma-buf/AHWB declarado — anunciando mesmo assim (import será tentado em tempo de desenho)");
    }
    // Tabela mínima para o feedback v4+ e para os eventos format/modifier de v1–v3.
    // Se o renderer tiver tabela real (build_dmabuf_feedback_table) ela substitui esta.
    if (st->dmabuf_feedback_fd < 0) {
        build_ahwb_format_table(st);
    }
    st->g_dmabuf = wl_global_create(st->display,
                                    &zwp_linux_dmabuf_v1_interface,
                                    DMABUF_VERSION, st, dmabuf_bind);
    return st->g_dmabuf != nullptr;
}

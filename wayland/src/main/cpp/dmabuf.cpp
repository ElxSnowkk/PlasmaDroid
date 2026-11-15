
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

static constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;

static const struct wl_buffer_interface g_dmabuf_buffer_impl = {
    [](wl_client*, wl_resource* r){ wl_resource_destroy(r); },
};

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

struct Params {
    int width = 0, height = 0;
    uint32_t fourcc = 0, flags = 0;
    uint64_t modifier = 0;
    bool modifier_set = false;
    bool used = false;
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

    if (p->fds[plane_idx] >= 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane index %u already set", plane_idx);
        ::close(fd);
        return;
    }

    uint64_t new_mod = (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
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

    if (p->used) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params object already used");
        return;
    }
    p->used = true;

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

    auto* db = new DmabufBuffer();
    db->width    = w;
    db->height   = h;
    db->fourcc   = fourcc;
    db->flags    = flags;
    db->modifier = p->modifier_set ? p->modifier : DRM_FORMAT_MOD_INVALID;
    db->offsets  = p->offsets;
    db->strides  = p->strides;
    for (int fd : p->fds) {
        db->fds.push_back(fd >= 0 ? ::dup(fd) : -1);
    }

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

void dmabuf_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void dmabuf_create_params(wl_client* c, wl_resource* parent, uint32_t id) {
    auto* p = new Params();
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
    if (!surface_res) {
        LOGW("get_surface_feedback: null surface resource");
    } else {
        void* ud = wl_resource_get_user_data(surface_res);
        if (!ud) {
            LOGW("get_surface_feedback: surface resource has null user_data");
        }
    }
    dmabuf_get_default_feedback(c, dmabuf_res, id);
}

const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    dmabuf_destroy,
    dmabuf_create_params,
    dmabuf_get_default_feedback,
    dmabuf_get_surface_feedback,
};

void dmabuf_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < DMABUF_VERSION ? version : DMABUF_VERSION;
    wl_resource* r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);
    auto* st = g_state;
    if (v < 4 && st) {
        for (const auto& fm : st->dmabuf_formats) {
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

}

static bool build_ahwb_format_table(State* st) {
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
    if (!st->renderer) {
        LOGI("dmabuf: sem renderer ainda — anunciando o global com tabela padrão (modo headless)");
    } else if (!st->renderer->has_dmabuf_import() && !st->renderer->has_ahwb_import()) {
        LOGW("dmabuf: EGL sem import de dma-buf/AHWB declarado — anunciando mesmo assim (import será tentado em tempo de desenho)");
    }
    if (st->dmabuf_feedback_fd < 0) {
        build_ahwb_format_table(st);
    }
    st->g_dmabuf = wl_global_create(st->display,
                                    &zwp_linux_dmabuf_v1_interface,
                                    DMABUF_VERSION, st, dmabuf_bind);
    return st->g_dmabuf != nullptr;
}

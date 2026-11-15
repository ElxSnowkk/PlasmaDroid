// syncobj.cpp — wp_linux_drm_syncobj_manager_v1 minimal implementation.
// Inline DRM ioctl structs for SYNCOBJ_FD_TO_HANDLE and TIMELINE_WAIT/SIGNAL.
// No system header dependency on drm.h.
#include "labwc.h"
#include "egl_renderer.h"
extern "C" {
#include "linux-drm-syncobj-v1-protocol.h"
}
#include <android/log.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LOG_TAG "Piperella/syncobj"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t SO_VERSION = 1;

// ---- inline DRM ioctl helpers ----

#define DRM_IOCTL_BASE 'd'
#define DRM_IOC(nr, type) _IOC(_IOC_READ|_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof(type))

// Must match the kernel UAPI exactly (include/uapi/drm/drm.h): the order is
// handle, flags, fd, pad — flags comes BEFORE fd. The previous {handle, fd,
// flags, pad} layout wrote the client's fd into the kernel's flags field, so
// every timeline import returned -EINVAL and disconnected the client.
struct drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
    uint32_t pad;
};
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE DRM_IOC(0xC2, drm_syncobj_handle)

// C07-15: Two separate structs — WAIT (0xCA) has timeout_nsec (40 bytes),
// SIGNAL (0xCD) is 24 bytes matching kernel drm_syncobj_timeline_array.
// Kernel ABI confirmed from uapi/drm/drm.h (linux mainline).
struct drm_syncobj_timeline_wait {
    uint64_t handles;
    uint64_t points;
    int64_t  timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
};
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT   DRM_IOC(0xCA, drm_syncobj_timeline_wait)

// 24 bytes — matches kernel drm_syncobj_timeline_array exactly.
// first_signaled/pad removed (those belong to drm_syncobj_timeline_wait only).
struct drm_syncobj_timeline_signal {
    uint64_t handles;
    uint64_t points;
    uint32_t count_handles;
    uint32_t flags;
};
#define DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL DRM_IOC(0xCD, drm_syncobj_timeline_signal)

struct drm_syncobj_destroy { uint32_t handle; uint32_t pad; };
#define DRM_IOCTL_SYNCOBJ_DESTROY DRM_IOC(0xC0, drm_syncobj_destroy)

// ---- timeline object ----

struct SyncobjTimeline {
    uint32_t drm_handle = 0;
};

// C07-13: resource destroy callbacks handle memory; request handlers just call
// wl_resource_destroy to keep the two lifecycle paths consistent.
static void timeline_resource_destroy(wl_resource* r) {
    auto* tl = static_cast<SyncobjTimeline*>(wl_resource_get_user_data(r));
    if (!tl) return;
    // Release the imported kernel syncobj handle (was leaked for the life of
    // drm_fd, one per timeline object the client created).
    if (tl->drm_handle != 0) {
        EglRenderer* renderer = g_state ? g_state->renderer.get() : nullptr;
        int drm_fd = renderer ? renderer->drm_fd() : -1;
        if (drm_fd >= 0) {
            drm_syncobj_destroy d = { tl->drm_handle, 0 };
            if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d) != 0)
                LOGW("SYNCOBJ_DESTROY(%u) failed: %s", tl->drm_handle, strerror(errno));
        }
    }
    delete tl;
}

static void timeline_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
    timeline_destroy,
};

// ---- syncobj surface ----

struct SyncobjSurface {
    Surface* surface = nullptr;
};

// C07-13: resource destroy callback clears Surface back-pointers.
static void so_surface_resource_destroy(wl_resource* r) {
    auto* ss = static_cast<SyncobjSurface*>(wl_resource_get_user_data(r));
    if (ss && ss->surface) {
        ss->surface->has_syncobj_surface  = false;
        ss->surface->syncobj_surface_res  = nullptr;
    }
    delete ss;
}

static void so_surface_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static void so_surface_set_acquire_point(wl_client*, wl_resource* r,
                                         wl_resource* timeline_res,
                                         uint32_t point_hi, uint32_t point_lo) {
    SyncobjSurface* ss = static_cast<SyncobjSurface*>(wl_resource_get_user_data(r));
    SyncobjTimeline* tl = static_cast<SyncobjTimeline*>(wl_resource_get_user_data(timeline_res));
    // C07-12: post no_surface error when the associated wl_surface was destroyed.
    if (!ss || !ss->surface) {
        wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                               "associated wl_surface was destroyed");
        return;
    }
    if (!tl) return;
    // C07-14: write to pending (double-buffered) fields; applied at commit.
    ss->surface->pending_acquire_point.syncobj_handle = tl->drm_handle;
    ss->surface->pending_acquire_point.timeline_point = (static_cast<uint64_t>(point_hi) << 32) | point_lo;
}
static void so_surface_set_release_point(wl_client*, wl_resource* r,
                                         wl_resource* timeline_res,
                                         uint32_t point_hi, uint32_t point_lo) {
    SyncobjSurface* ss = static_cast<SyncobjSurface*>(wl_resource_get_user_data(r));
    SyncobjTimeline* tl = static_cast<SyncobjTimeline*>(wl_resource_get_user_data(timeline_res));
    // C07-12: post no_surface error when the associated wl_surface was destroyed.
    if (!ss || !ss->surface) {
        wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                               "associated wl_surface was destroyed");
        return;
    }
    if (!tl) return;
    // C07-14: write to pending (double-buffered) fields; applied at commit.
    ss->surface->pending_release_point.syncobj_handle = tl->drm_handle;
    ss->surface->pending_release_point.timeline_point = (static_cast<uint64_t>(point_hi) << 32) | point_lo;
}
static const struct wp_linux_drm_syncobj_surface_v1_interface so_surface_impl = {
    so_surface_destroy,
    so_surface_set_acquire_point,
    so_surface_set_release_point,
};

// ---- syncobj manager ----

static void so_mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void so_mgr_get_surface(wl_client* c, wl_resource* mgr_res, uint32_t id,
                                wl_resource* surf_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surf_res));
    if (!s) { wl_client_post_implementation_error(c, "get_surface: bad surface"); return; }

    // C07-11: post surface_exists error if this surface already has a syncobj.
    // The error is defined on the MANAGER, not the wl_surface.
    if (s->has_syncobj_surface) {
        wl_resource_post_error(mgr_res,
                               WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
                               "surface already has a syncobj surface object");
        return;
    }

    auto* ss = new SyncobjSurface{ s };
    wl_resource* r = wl_resource_create(c, &wp_linux_drm_syncobj_surface_v1_interface,
                                         SO_VERSION, id);
    if (!r) { delete ss; wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &so_surface_impl, ss, so_surface_resource_destroy);

    // C07-11: mark surface and store back-reference for commit-time validation.
    s->has_syncobj_surface = true;
    s->syncobj_surface_res = r;

    // C07-12: when the wl_surface is destroyed before the syncobj surface
    // object, null SyncobjSurface::surface (preventing UAF in
    // set_acquire/release_point). Do NOT capture `ss` directly: if the syncobj
    // surface object was destroyed first, `ss` is freed and this hook would
    // write through it. Reach it via the surface's own back-pointer, which
    // so_surface_resource_destroy already nulls — so a freed ss makes the hook
    // a no-op.
    s->destruction_hooks.push_back([](Surface* dead) {
        if (!dead->syncobj_surface_res) return;
        auto* live = static_cast<SyncobjSurface*>(
            wl_resource_get_user_data(dead->syncobj_surface_res));
        if (live) live->surface = nullptr;
        dead->has_syncobj_surface = false;
        dead->syncobj_surface_res = nullptr;
    });
}
static void so_mgr_import_timeline(wl_client* c, wl_resource*, uint32_t id, int32_t fd) {
    auto* tl = new SyncobjTimeline();
    wl_resource* r = wl_resource_create(c, &wp_linux_drm_syncobj_timeline_v1_interface,
                                         SO_VERSION, id);
    if (!r) { delete tl; wl_client_post_no_memory(c); if (fd >= 0) ::close(fd); return; }

    // C07-13: register destroy callback before ioctl so tl is always freed.
    wl_resource_set_implementation(r, &timeline_impl, tl, timeline_resource_destroy);

    // Import fd to DRM syncobj handle.
    EglRenderer* renderer = g_state ? g_state->renderer.get() : nullptr;
    int drm_fd = renderer ? renderer->drm_fd() : -1;
    if (drm_fd >= 0 && fd >= 0) {
        drm_syncobj_handle args = {};
        args.fd = static_cast<uint32_t>(fd);
        args.flags = 0;
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args) == 0) {
            tl->drm_handle = args.handle;
        } else {
            LOGE("SYNCOBJ_FD_TO_HANDLE failed: %s", strerror(errno));
            // C07-10: post invalid_timeline error — client provided an unusable fd.
            wl_resource_post_error(r,
                                   WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
                                   "failed to import syncobj timeline fd");
            if (fd >= 0) ::close(fd);
            return;
        }
    }
    if (fd >= 0) ::close(fd);
}
static const struct wp_linux_drm_syncobj_manager_v1_interface so_mgr_impl = {
    so_mgr_destroy,
    so_mgr_get_surface,
    so_mgr_import_timeline,
};

static void so_mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wp_linux_drm_syncobj_manager_v1_interface,
                                         ver < SO_VERSION ? ver : SO_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &so_mgr_impl, nullptr, nullptr);
}

bool syncobj_globals_register(State* st) {
    // Only advertise explicit-sync if the renderer actually has a DRM fd with
    // timeline-syncobj support. Advertising it otherwise means every timeline
    // import is a no-op (drm_handle stays 0) and the commit-time validation
    // then disconnects a perfectly conforming client. (Called after renderer
    // init, like the dmabuf global, so this check is meaningful.)
    if (!st->renderer || st->renderer->drm_fd() < 0 ||
        !st->renderer->drm_syncobj_capable()) {
        LOGW("explicit-sync unsupported (no DRM timeline syncobj) — skipping global");
        return true;
    }
    st->g_syncobj = wl_global_create(st->display, &wp_linux_drm_syncobj_manager_v1_interface,
                                      SO_VERSION, st, so_mgr_bind);
    return st->g_syncobj != nullptr;
}

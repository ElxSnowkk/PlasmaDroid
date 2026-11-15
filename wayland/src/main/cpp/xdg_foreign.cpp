
#include "labwc.h"

extern "C" {
#include "xdg-foreign-protocol.h"
}

#include <android/log.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "Piperella/foreign"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

extern State* g_state;

static constexpr uint32_t XDG_EXPORTER_VERSION = 1;
static constexpr uint32_t XDG_IMPORTER_VERSION = 1;

struct ExportRec {
    Surface*                  surface = nullptr;
    wl_resource*              exported_res = nullptr;
    std::vector<wl_resource*> imports;
};

static std::unordered_map<std::string, ExportRec> s_exports;

struct ImportRec {
    std::string handle;
};

static uint64_t s_handle_counter = 0;

static Toplevel* toplevel_for_surface_locked(State* st, Surface* s) {
    if (!s) return nullptr;
    for (auto& t : st->toplevels) if (t->surface == s) return t.get();
    return nullptr;
}

static void exported_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct zxdg_exported_v2_interface exported_impl = {
    exported_destroy,
};

static void exported_resource_destroy(wl_resource* r) {
    auto* handle = static_cast<std::string*>(wl_resource_get_user_data(r));
    if (!handle) return;
    auto it = s_exports.find(*handle);
    if (it != s_exports.end()) {
        for (wl_resource* imp : it->second.imports)
            zxdg_imported_v2_send_destroyed(imp);
        s_exports.erase(it);
    }
    delete handle;
}

static void exporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void exporter_export_toplevel(wl_client* c, wl_resource*,
                                     uint32_t id, wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));

    wl_resource* r = wl_resource_create(c, &zxdg_exported_v2_interface,
                                        XDG_EXPORTER_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }

    char buf[32];
    snprintf(buf, sizeof(buf), "pip-%llx",
             (unsigned long long)(++s_handle_counter));
    auto* handle = new std::string(buf);

    ExportRec rec;
    rec.surface = s;
    rec.exported_res = r;
    s_exports[*handle] = rec;

    wl_resource_set_implementation(r, &exported_impl, handle,
                                   exported_resource_destroy);
    zxdg_exported_v2_send_handle(r, handle->c_str());
    LOGI("export_toplevel: surface=%p handle=%s", (void*)s, handle->c_str());
}

static const struct zxdg_exporter_v2_interface exporter_impl = {
    exporter_destroy,
    exporter_export_toplevel,
};

static void imported_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void imported_set_parent_of(wl_client*, wl_resource* r,
                                   wl_resource* surface_res) {
    auto* imp = static_cast<ImportRec*>(wl_resource_get_user_data(r));
    if (!imp) return;
    Surface* child = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    if (!child) return;

    auto it = s_exports.find(imp->handle);
    if (it == s_exports.end()) {
        LOGW("set_parent_of: export handle %s no longer valid", imp->handle.c_str());
        return;
    }
    Surface* parent = it->second.surface;

    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    Toplevel* ct = toplevel_for_surface_locked(st, child);
    Toplevel* pt = toplevel_for_surface_locked(st, parent);
    if (ct) {
        ct->foreign_parent = parent;
        if (pt && ct->z_order <= pt->z_order) ct->z_order = st->next_z_order++;
        LOGI("set_parent_of: child_tl=%p parent_surface=%p (parent_tl=%p)",
             (void*)ct, (void*)parent, (void*)pt);
    } else {
        LOGW("set_parent_of: child surface %p has no toplevel yet", (void*)child);
    }
}

static const struct zxdg_imported_v2_interface imported_impl = {
    imported_destroy,
    imported_set_parent_of,
};

static void imported_resource_destroy(wl_resource* r) {
    auto* imp = static_cast<ImportRec*>(wl_resource_get_user_data(r));
    if (!imp) return;
    auto it = s_exports.find(imp->handle);
    if (it != s_exports.end()) {
        auto& v = it->second.imports;
        v.erase(std::remove(v.begin(), v.end(), r), v.end());
    }
    delete imp;
}

static void importer_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void importer_import_toplevel(wl_client* c, wl_resource*,
                                     uint32_t id, const char* handle) {
    wl_resource* r = wl_resource_create(c, &zxdg_imported_v2_interface,
                                        XDG_IMPORTER_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }

    auto* imp = new ImportRec();
    if (handle) imp->handle = handle;
    wl_resource_set_implementation(r, &imported_impl, imp,
                                   imported_resource_destroy);

    auto it = s_exports.find(imp->handle);
    if (it == s_exports.end()) {
        LOGW("import_toplevel: unknown handle %s -> destroyed", imp->handle.c_str());
        zxdg_imported_v2_send_destroyed(r);
        return;
    }
    it->second.imports.push_back(r);
    LOGI("import_toplevel: handle=%s imported_res=%p", imp->handle.c_str(), (void*)r);
}

static const struct zxdg_importer_v2_interface importer_impl = {
    importer_destroy,
    importer_import_toplevel,
};

static void exporter_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &zxdg_exporter_v2_interface,
        ver < XDG_EXPORTER_VERSION ? ver : XDG_EXPORTER_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &exporter_impl, nullptr, nullptr);
    LOGI("zxdg_exporter_v2 bound (client %p ver %u)", (void*)c, ver);
}

static void importer_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &zxdg_importer_v2_interface,
        ver < XDG_IMPORTER_VERSION ? ver : XDG_IMPORTER_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &importer_impl, nullptr, nullptr);
    LOGI("zxdg_importer_v2 bound (client %p ver %u)", (void*)c, ver);
}

bool xdg_foreign_globals_register(State* st) {
    st->g_xdg_exporter = wl_global_create(st->display, &zxdg_exporter_v2_interface,
                                          XDG_EXPORTER_VERSION, st, exporter_bind);
    st->g_xdg_importer = wl_global_create(st->display, &zxdg_importer_v2_interface,
                                          XDG_IMPORTER_VERSION, st, importer_bind);
    return st->g_xdg_exporter && st->g_xdg_importer;
}

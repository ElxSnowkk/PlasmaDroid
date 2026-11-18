
#include "vk_intercept.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>

#ifdef __ANDROID__
#include <android/dlext.h>
#endif

#include "got_patch.h"

#ifdef VKI_STANDALONE
#define VKI_LOG(...) (fprintf(stderr, "[vk_intercept] " __VA_ARGS__), fputc('\n', stderr))
#else

#include <android/log.h>
#define VKI_LOG(...) __android_log_print(ANDROID_LOG_INFO, "Incandescent", __VA_ARGS__)
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::musttail)
#define VKI_TAIL_RETURN(call) [[clang::musttail]] return call
#endif
#endif
#ifndef VKI_TAIL_RETURN
#define VKI_TAIL_RETURN(call) return call
#endif

namespace {

std::once_flag g_once;
int g_result = 0;
std::string g_bridge;
std::atomic<int> g_hits{0};
std::atomic<int> g_hit_reported{0};

struct Report {
    std::string state = "off", reason, mode, kind, route, bridge, lsfg = "off";
    int surface = -1, loader = -1, patch_dlopen = 0, patch_vk = 0, needs_libvulkan = 0, hits = 0;
} g_rep;

void write_report() {
    const char *path = getenv("INC_VK_REPORT");
    if (!path || !*path) return;
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "STATE=%s\nREASON=%s\nDRIVER_MODE=%s\nDRIVER_KIND=%s\nDRIVER_ROUTE=%s\nSURFACE=%d\nLOADER=%d\n"
               "PATCH_DLOPEN=%d\nPATCH_VK=%d\nNEEDS_LIBVULKAN=%d\nHITS=%d\nBRIDGE=%s\nLSFG=%s\n",
            g_rep.state.c_str(), g_rep.reason.c_str(), g_rep.mode.c_str(), g_rep.kind.c_str(), g_rep.route.c_str(), g_rep.surface, g_rep.loader,
            g_rep.patch_dlopen, g_rep.patch_vk, g_rep.needs_libvulkan, g_rep.hits, g_rep.bridge.c_str(), g_rep.lsfg.c_str());
    if (fclose(f) != 0 || rename(tmp.c_str(), path) != 0) unlink(tmp.c_str());
}

int finish(const char *state, const std::string &reason) {
    g_rep.state = state;
    g_rep.reason = reason;
    VKI_LOG("ANGLE/Vulkan: %s%s%s", state, reason.empty() ? "" : " - ", reason.c_str());
    write_report();
    return !strcmp(state, "active");
}

bool has(const std::string &json, const char *needle) { return json.find(needle) != std::string::npos; }
std::string field(const std::string &json, const char *key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = json.find(k);
    if (p == std::string::npos) return "";
    const size_t s = p + k.size(), e = json.find('"', s);
    return e == std::string::npos ? "" : json.substr(s, e - s);
}

bool is_vulkan_loader_name(const char *n) {
    const char *b = strrchr(n, '/');
    b = b ? b + 1 : n;
    if (strncmp(b, "libvulkan.so", 12) != 0) return false;
    return b[12] == '\0' || b[12] == '.';
}

void note_hit(const char *name) {
    const int n = ++g_hits;
    if (g_hit_reported.exchange(1) == 0) {
        g_rep.hits = n;
        VKI_LOG("ANGLE pediu %s: devolvida a ponte do driver injetado", name);
        write_report();
    }
}

extern "C" {
void *vki_dlopen(const char *name, int flags) {
    if (name && is_vulkan_loader_name(name)) {
        if (void *h = dlopen(g_bridge.c_str(), flags)) { note_hit(name); return h; }
        const char *e = dlerror();
        VKI_LOG("falha ao abrir a ponte (%s); o ANGLE fica com o Vulkan do sistema", e ? e : "?");
    }
    VKI_TAIL_RETURN(dlopen(name, flags));
}
#ifdef __ANDROID__
void *vki_android_dlopen_ext(const char *name, int flags, const android_dlextinfo *ext) {
    if (name && is_vulkan_loader_name(name)) {
        if (void *h = dlopen(g_bridge.c_str(), flags & ~RTLD_NOLOAD)) { note_hit(name); return h; }
        const char *e = dlerror();
        VKI_LOG("falha ao abrir a ponte (%s); o ANGLE fica com o Vulkan do sistema", e ? e : "?");
    }
    VKI_TAIL_RETURN(android_dlopen_ext(name, flags, ext));
}
#endif
}

int match_angle(const char *path, void *) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (strstr(b, "kryox") || strstr(b, "incandescent")) return 0;
    if (strstr(b, "angle")) return 1;
    const char *extra = getenv("INC_VK_PATCH_MODULES");
    if (!extra || !*extra) return 0;
    std::string list = std::string(",") + extra + ",", me = std::string(",") + b + ",";
    return list.find(me) != std::string::npos;
}

void *visit_dlopen(const char *, const char *sym, void *, void *) {
    if (!strcmp(sym, "dlopen")) return reinterpret_cast<void *>(&vki_dlopen);
#ifdef __ANDROID__
    if (!strcmp(sym, "android_dlopen_ext")) return reinterpret_cast<void *>(&vki_android_dlopen_ext);
#endif
    return nullptr;
}

struct SymCtx { void *loader; int seen, missing; void *(*lsfg)(const char *); };
void *visit_vk(const char *, const char *sym, void *, void *c) {
    auto *s = static_cast<SymCtx *>(c);
    if (strncmp(sym, "vk", 2) != 0) return nullptr;
    s->seen++;
    void *p = s->lsfg ? s->lsfg(sym) : nullptr;
    if (!p) p = dlsym(s->loader, sym);
    if (!p) s->missing++;
    return p;
}

int run() {
    const char *bridge = getenv("INC_VK_BRIDGE");
    if (!bridge || !*bridge) return finish("off", "INC_VK_BRIDGE nao definido");
    g_bridge = bridge;
    g_rep.bridge = bridge;

    setenv("KRYOX_VK_WANT_LOADER", "1", 0);

    void *h = dlopen(bridge, RTLD_NOW | RTLD_LOCAL);
    if (!h) { const char *e = dlerror(); return finish("failed", std::string("dlopen da ponte falhou: ") + (e ? e : "?")); }
    using StatusFn = const char *(*)();
    using LoaderFn = void *(*)();
    auto status = reinterpret_cast<StatusFn>(dlsym(h, "kryox_status"));
    if (!status) return finish("failed", "a ponte nao exporta kryox_status");
    const std::string js = status();

    g_rep.mode = field(js, "mode");
    g_rep.kind = field(js, "kind");
    g_rep.route = field(js, "route");
    g_rep.surface = has(js, "\"surface\":true") ? 1 : 0;
    g_rep.loader = has(js, "\"loader\":true") ? 1 : 0;
    g_rep.lsfg = field(js, "lsfg");
    const bool lsfgActive = g_rep.lsfg == "active";

    if (g_rep.mode != "custom" && !lsfgActive)
        return finish("skipped", "driver custom nao ficou ativo (mode=" + g_rep.mode + "; " + field(js, "reason") + "): ANGLE segue no Vulkan do sistema");
    if (!g_rep.surface)
        return finish("skipped", "o driver custom nao expoe VK_KHR_android_surface (ICD aberto sem o loader Android): o ANGLE nao conseguiria "
                                 "apresentar; deixado no Vulkan do sistema");

    gp_stats st;
    const int would = gp_walk(match_angle, nullptr, visit_dlopen, nullptr, 0, &st);
    g_rep.needs_libvulkan = st.needs_libvulkan;
    if (st.modules == 0) return finish("failed", "nenhum modulo ANGLE encontrado entre os carregados (nomes: *angle*; INC_VK_PATCH_MODULES para outros)");

    void *loader = nullptr;
    if (auto lf = reinterpret_cast<LoaderFn>(dlsym(h, "kryox_vk_loader_handle"))) loader = lf();
    int vk_to_patch = 0;
    using LsfgFn = void *(*)(const char *);
    const LsfgFn lsfgOverride = lsfgActive ? reinterpret_cast<LsfgFn>(dlsym(h, "kryox_vk_lsfg_override")) : nullptr;
    if (st.needs_libvulkan) {
        if (!loader) {
            if (would == 0) return finish("failed", "o ANGLE liga libvulkan.so em tempo de carga e a ponte nao expoe um loader completo (driver ICD direto)");
            VKI_LOG("aviso: ANGLE tem DT_NEEDED libvulkan.so, mas a ponte nao tem loader completo; so o dlopen sera interceptado");
        } else {
            SymCtx sc{loader, 0, 0, lsfgOverride};
            gp_stats sv;
            vk_to_patch = gp_walk(match_angle, nullptr, visit_vk, &sc, 0, &sv);
            if (sc.missing) return finish("failed", std::to_string(sc.missing) + " simbolo(s) vk* do ANGLE nao existem no loader do driver injetado; nada foi trocado");
        }
    }
    if (would == 0 && vk_to_patch == 0) return finish("failed", "o ANGLE nao importa dlopen nem vk*: nao ha por onde interceptar");

    int wrote_dl = 0, wrote_vk = 0, failed = 0;
    if (would) { gp_stats w; wrote_dl = gp_walk(match_angle, nullptr, visit_dlopen, nullptr, 1, &w); failed += w.failed; }
    if (vk_to_patch) { SymCtx sc{loader, 0, 0, lsfgOverride}; gp_stats w; wrote_vk = gp_walk(match_angle, nullptr, visit_vk, &sc, 1, &w); failed += w.failed; }
    g_rep.patch_dlopen = wrote_dl;
    g_rep.patch_vk = wrote_vk;
    if (failed) return finish("failed", std::to_string(failed) + " slot(s) nao puderam ser reescritos (mprotect negado?)");
    VKI_LOG("ANGLE/Vulkan: %d import(s) dlopen e %d import(s) vk* redirecionados em %d modulo(s)", wrote_dl, wrote_vk, st.modules);
    return finish("active", "");
}

}

extern "C" int inc_vk_intercept_angle(void) {
    std::call_once(g_once, [] { g_result = run(); });
    return g_result;
}

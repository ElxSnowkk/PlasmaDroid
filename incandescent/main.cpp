// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - main.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "config/settings.h"
#include "config/stats.h"
#include "egl/egl.h"
#include "egl/loader.h"
#include "gl/envvars.h"
#include "gl/gl.h"
#include "gl/log.h"
#include "gl/inc.h"
#include "gles/loader.h"
#include "includes.h"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unwind.h>

#define DEBUG 0

#ifndef __APPLE__
__attribute__((used))
#endif
const char* license = "GNU LGPL-2.1 License";

namespace {
char g_crash_path[512];
alignas(16) char g_altstack[64 * 1024];
int g_probe[2] = {-1, -1};

struct BtState {
    void** cur;
    void** end;
};

_Unwind_Reason_Code crash_bt_cb(struct _Unwind_Context* ctx, void* arg) {
    auto* s = static_cast<BtState*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (s->cur == s->end) return _URC_END_OF_STACK;
        *s->cur++ = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

void crash_emit(int fd, const char* buf, int n) {
    if (n > 0) (void)!write(fd, buf, static_cast<size_t>(n));
}

bool crash_safe_read(const void* addr, void* out, size_t len) {
    if (g_probe[0] < 0 || !addr) return false;
    ssize_t w = write(g_probe[1], addr, len);
    if (w <= 0) return false;
    if (w != static_cast<ssize_t>(len)) {
        char drop[64];
        (void)!read(g_probe[0], drop, static_cast<size_t>(w) < sizeof drop ? static_cast<size_t>(w) : sizeof drop);
        return false;
    }
    return read(g_probe[0], out, len) == static_cast<ssize_t>(len);
}

void crash_sym(int fd, int idx, const char* tag, uintptr_t pc) {
    char line[640];
    Dl_info d;
    memset(&d, 0, sizeof d);
    int n;
    if (dladdr(reinterpret_cast<void*>(pc), &d) && d.dli_fname) {
        n = snprintf(line, sizeof line, "  #%d %s pc=%p %s+0x%lx (%s)\n", idx, tag, reinterpret_cast<void*>(pc),
                     d.dli_fname, static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(d.dli_fbase)),
                     d.dli_sname ? d.dli_sname : "?");
    } else {
        n = snprintf(line, sizeof line, "  #%d %s pc=%p ?\n", idx, tag, reinterpret_cast<void*>(pc));
    }
    crash_emit(fd, line, n);
}

void crash_map_line(int fd, const char* l, const uintptr_t* pcs, int npc) {
    char* dash = nullptr;
    const unsigned long long lo = strtoull(l, &dash, 16);
    if (!dash || *dash != '-') return;
    char* perm = nullptr;
    const unsigned long long hi = strtoull(dash + 1, &perm, 16);
    const bool ours = strstr(l, "incandescent") != nullptr;
    unsigned long long foff = 0;
    if (perm && *perm == ' ') {
        char* o = perm + 1;
        while (*o && *o != ' ') ++o;
        foff = strtoull(o, nullptr, 16);
    }
    const char* path = strchr(l, '/');
    char line[640];
    if (ours) {
        const int n = snprintf(line, sizeof line, "  map: %s\n", l);
        crash_emit(fd, line, n);
    }
    for (int i = 0; i < npc; ++i) {
        if (pcs[i] >= lo && pcs[i] < hi) {
            const int n = snprintf(line, sizeof line, "  hit #%d pc=%p -> %s+0x%llx\n", i,
                                   reinterpret_cast<void*>(pcs[i]), path ? path : "[anon]", foff + (pcs[i] - lo));
            crash_emit(fd, line, n);
        }
    }
    (void)hi;
}

void crash_dump_to(int fd, int sig, siginfo_t* si, void* uctx) {
    char line[640];
    int n = snprintf(line, sizeof line, "[incandescent-crash] sinal=%d code=%d addr=%p pid=%d tid=%ld\n", sig,
                     si ? si->si_code : 0, si ? si->si_addr : nullptr, static_cast<int>(getpid()),
                     static_cast<long>(syscall(SYS_gettid)));
    crash_emit(fd, line, n);

    uintptr_t pcs[64];
    int npc = 0;
#if defined(__aarch64__)
    if (uctx) {

        auto* uc = static_cast<ucontext_t*>(uctx);
        const uintptr_t pc = uc->uc_mcontext.pc;
        const uintptr_t lr = uc->uc_mcontext.regs[30];
        const uintptr_t sp = uc->uc_mcontext.sp;
        uintptr_t fp = uc->uc_mcontext.regs[29];
        n = snprintf(line, sizeof line, "  ctx pc=%p lr=%p sp=%p fp=%p fault_addr=%p\n", reinterpret_cast<void*>(pc),
                     reinterpret_cast<void*>(lr), reinterpret_cast<void*>(sp), reinterpret_cast<void*>(fp),
                     reinterpret_cast<void*>(uc->uc_mcontext.fault_address));
        crash_emit(fd, line, n);
        crash_sym(fd, 0, "PC", pc);
        pcs[npc++] = pc;
        if (lr && lr != pc) {
            crash_sym(fd, 1, "LR", lr);
            pcs[npc++] = lr;
        }
        for (int i = 2; i < 40 && npc < 64; ++i) {
            if (!fp || (fp & 7)) break;
            uintptr_t rec[2];
            if (!crash_safe_read(reinterpret_cast<void*>(fp), rec, sizeof rec)) break;
            if (rec[1]) {
                crash_sym(fd, i, "fp", rec[1]);
                pcs[npc++] = rec[1];
            }
            if (rec[0] <= fp) break;
            fp = rec[0];
        }
    } else
#endif
    {
        void* frames[48];
        BtState st{frames, frames + 48};
        _Unwind_Backtrace(crash_bt_cb, &st);
        for (void** p = frames; p < st.cur; ++p) {
            crash_sym(fd, static_cast<int>(p - frames), "unw", reinterpret_cast<uintptr_t>(*p));
            if (npc < 64) pcs[npc++] = reinterpret_cast<uintptr_t>(*p);
        }
    }

    int mfd = open("/proc/self/maps", O_RDONLY);
    if (mfd >= 0) {
        static char buf[8192];
        size_t have = 0;
        bool eof = false;
        while (!eof) {
            const ssize_t r = read(mfd, buf + have, sizeof buf - 1 - have);
            if (r <= 0) eof = true; else have += static_cast<size_t>(r);
            buf[have] = '\0';
            char* l = buf;
            for (char* e; (e = strchr(l, '\n')) != nullptr; l = e + 1) {
                *e = '\0';
                crash_map_line(fd, l, pcs, npc);
            }
            size_t rest = static_cast<size_t>(buf + have - l);
            if (eof && rest) crash_map_line(fd, l, pcs, npc);
            memmove(buf, l, rest);
            have = rest >= sizeof buf - 1 ? 0 : rest;
        }
        close(mfd);
    }
}

void crash_handler(int sig, siginfo_t* si, void* uctx) {
    crash_dump_to(2, sig, si, uctx);
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        crash_dump_to(fd, sig, si, uctx);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void inc_install_crash_reporter() {
    const char* off = getenv("INC_CRASH_REPORT");
    if (off && off[0] == '0') return;
    const char* dir = getenv("INC_DIR_PATH");
    snprintf(g_crash_path, sizeof g_crash_path, "%s/crash.log", (dir && *dir) ? dir : "/tmp");
    if (pipe2(g_probe, O_NONBLOCK | O_CLOEXEC) != 0) g_probe[0] = g_probe[1] = -1;
    stack_t old_ss;
    memset(&old_ss, 0, sizeof old_ss);
    bool have_alt = sigaltstack(nullptr, &old_ss) == 0 && !(old_ss.ss_flags & SS_DISABLE);
    if (!have_alt) {
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_sp = g_altstack;
        ss.ss_size = sizeof g_altstack;
        have_alt = sigaltstack(&ss, nullptr) == 0;
    }
    static const int sigs[] = {SIGTRAP, SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (int sig : sigs) {
        struct sigaction old;
        memset(&old, 0, sizeof old);
        if (sigaction(sig, nullptr, &old) != 0) continue;
        if ((old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_DFL) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | (have_alt ? SA_ONSTACK : 0);
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
    }
}
}

#include <cstdlib>
#include <pthread.h>

namespace {
struct ThreadDtor { void (*fn)(void*); void* obj; ThreadDtor* next; };
pthread_key_t g_thread_dtor_key;
pthread_once_t g_thread_dtor_once = PTHREAD_ONCE_INIT;
void thread_dtor_run(void* head) {
    for (ThreadDtor* d = static_cast<ThreadDtor*>(head); d;) {
        ThreadDtor* next = d->next;
        d->fn(d->obj);
        free(d);
        d = next;
    }
}
void thread_dtor_make_key() { pthread_key_create(&g_thread_dtor_key, thread_dtor_run); }
}

extern "C" int __cxa_thread_atexit(void (*fn)(void*), void* obj, void*) noexcept {
    pthread_once(&g_thread_dtor_once, thread_dtor_make_key);
    auto* d = static_cast<ThreadDtor*>(malloc(sizeof(ThreadDtor)));
    if (!d) return -1;
    d->fn = fn; d->obj = obj;
    d->next = static_cast<ThreadDtor*>(pthread_getspecific(g_thread_dtor_key));
    return pthread_setspecific(g_thread_dtor_key, d);
}

void init_config() {
    if (!check_path()) return;
    config_refresh();

    bump_launch_count();
}

void show_license() {
    LOG_V("The Open Source License of Incandescent: ");
    LOG_V("  %s", license);
}

#if PROFILING

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

void init_perfetto() {
    perfetto::TracingInitArgs args;

    args.backends |= perfetto::kSystemBackend;

    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();
}
#endif

void proc_init() {

    LOG_I("[PlasmaGL] camada EGL r3 carregada (build %s %s)", __DATE__, __TIME__);
    inc_install_crash_reporter();
    init_config();

    clear_log();
    start_log();

    LOG_V("Initializing %s ...", RENDERERNAME);
    show_license();

    init_settings();

    load_libs();
    init_target_egl();
    init_target_gles();
    set_multidraw_setting();

    init_settings_post();

#if PROFILING
    init_perfetto();
#endif

#ifndef __APPLE__
    destroy_temp_egl_ctx();
#endif
    g_initialized = 1;
}

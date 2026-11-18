

#include "../includes.h"
#include "mali_tuning.h"
#include "../config/settings.h"
#include "log.h"
#include "../gles/loader.h"
#include "inc.h"
#include <atomic>
#include <cctype>
#include <string>

namespace {
std::atomic<int> g_mali_state{0};

std::string lower(const char* s) {
    std::string r = s ? s : "";
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}
}

bool mali_gpu_detected() {
    int st = g_mali_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;
    if (!GLES.glGetString) return false;
    const char* renderer = reinterpret_cast<const char*>(GLES.glGetString(GL_RENDERER));
    const char* vendor = reinterpret_cast<const char*>(GLES.glGetString(GL_VENDOR));
    if (!renderer && !vendor) return false;

    const std::string r = lower(renderer), v = lower(vendor);
    const bool mali = r.find("mali") != std::string::npos || v == "arm";
    g_mali_state.store(mali ? 1 : 2, std::memory_order_release);
    LOG_I("Incandescent: GPU %s (renderer=\"%s\", vendor=\"%s\")", mali ? "Mali detectada" : "nao-Mali",
          renderer ? renderer : "", vendor ? vendor : "")
    return mali;
}

bool mali_tuning_active() {
    switch (global_settings.mali_tuning) {
    case 2: return true;
    case 1: return mali_gpu_detected();
    default: return false;
    }
}

bool mali_avoid_sync_enabled() {
    return mali_tuning_active();
}

void mali_before_swap() {
    if (!mali_tuning_active()) return;
    if (!GLES.glGetIntegerv || !GLES.glInvalidateFramebuffer) return;
    GLint drawFbo = -1;
    GLES.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    if (drawFbo != 0) return;
    static const GLenum attachments[2] = {GL_DEPTH, GL_STENCIL};
    GLES.glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, attachments);
}

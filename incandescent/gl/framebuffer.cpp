// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/framebuffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "framebuffer.h"
#include "../egl/context.h"
#include <mutex>
#include <memory>
#include <ska/flat_hash_map.hpp>
#include "log.h"
#include "../config/settings.h"
#include "FSR1/FSR1.h"

#define DEBUG 0

#define FB_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool inc_fb_warned = false;                                                                              \
        if (!inc_fb_warned) {                                                                                           \
            inc_fb_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

static GLint MAX_COLOR_ATTACHMENTS = 0;
static GLint MAX_DRAW_BUFFERS = 0;

namespace {

struct fbo_ctx_state_t {
    ska::flat_hash_map<GLuint, std::unique_ptr<framebuffer_t>> table;
    GLuint draw = 0;
    GLuint read = 0;
};
std::mutex g_fbo_mutex;

ska::flat_hash_map<unsigned long long, std::unique_ptr<fbo_ctx_state_t>> g_fbo_ctxs;

thread_local fbo_ctx_state_t g_fbo_default;
thread_local fbo_ctx_state_t* g_fc = &g_fbo_default;
}

void inc_framebuffer_bind_context(unsigned long long ctx_id) {
    if (ctx_id == 0) {
        g_fc = &g_fbo_default;
        return;
    }
    std::lock_guard<std::mutex> lock(g_fbo_mutex);
    std::unique_ptr<fbo_ctx_state_t>& slot = g_fbo_ctxs[ctx_id];
    if (!slot) slot = std::make_unique<fbo_ctx_state_t>();
    g_fc = slot.get();
}

void inc_framebuffer_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_fbo_mutex);
    const auto it = g_fbo_ctxs.find(ctx_id);
    if (it == g_fbo_ctxs.end()) return;
    if (g_fc == it->second.get()) g_fc = &g_fbo_default;
    g_fbo_ctxs.erase(it);
}

#define framebuffers (g_fc->table)
#define current_draw_fbo (g_fc->draw)
#define current_read_fbo (g_fc->read)

bool inc_draw_framebuffer_all_none() {
    const auto it = framebuffers.find(current_draw_fbo);
    return it != framebuffers.end() && it->second->color_attachments_all_none;
}
void ensure_max_attachments() {

    if (MAX_COLOR_ATTACHMENTS == 0) {
        GLint v = 0;
        GLES.glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &v);
        if (v > 0) MAX_COLOR_ATTACHMENTS = v;
    }
    if (MAX_DRAW_BUFFERS == 0) {
        GLint v = 0;
        GLES.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v);
        if (v > 0) MAX_DRAW_BUFFERS = v;
    }
}

static GLint max_color_attachments_or_default() {
    return MAX_COLOR_ATTACHMENTS > 0 ? MAX_COLOR_ATTACHMENTS : 8;
}
framebuffer_t& get_framebuffer(GLuint id) {

    const auto it = framebuffers.find(id);
    if (it != framebuffers.end() && it->second) return *it->second;

    std::unique_ptr<framebuffer_t>& slot = framebuffers[id];
    if (!slot) slot = std::make_unique<framebuffer_t>();
    return *slot;
}
void InitFramebufferMap(size_t expectedSize) {
    framebuffers.reserve(expectedSize);
}
void init_framebuffer(framebuffer_t& fbo) {
    ensure_max_attachments();
    const size_t want = static_cast<size_t>(max_color_attachments_or_default());
    if (!fbo.initialized) {
        fbo.color_attachments.assign(want, attachment_t{});
        fbo.initialized = true;
        return;
    }

    if (fbo.color_attachments.size() < want) fbo.color_attachments.resize(want, attachment_t{});
}
void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    LOG()
    LOG_D("glBindFramebuffer, target = %s, framebuffer = %u", glEnumToString(target), framebuffer)
    ensure_max_attachments();

    GLuint draw_fb = framebuffer;
    if (framebuffer == 0 && target != GL_READ_FRAMEBUFFER) {
        draw_fb = FSR1_Context::g_renderFBO;
        FSR1_Context::g_dirty = true;
    }

    if (draw_fb != 0) {
        init_framebuffer(get_framebuffer(draw_fb));
    }

    if (target != GL_READ_FRAMEBUFFER) {
        set_gl_state_current_draw_fbo(draw_fb);
    }

    if (target == GL_DRAW_FRAMEBUFFER || target == GL_FRAMEBUFFER) {
        current_draw_fbo = draw_fb;
    }
    if (target == GL_READ_FRAMEBUFFER || target == GL_FRAMEBUFFER) {
        current_read_fbo = framebuffer;
    }

    if (target == GL_FRAMEBUFFER && draw_fb != framebuffer) {
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    } else {
        GLES.glBindFramebuffer(target, draw_fb);
    }
}

static thread_local int g_fsr_read_depth = 0;

inc_fsr_read_scope_t::inc_fsr_read_scope_t() {

    if (FSR1_Context::g_renderFBO == 0 || current_read_fbo != 0) return;
    counted = true;
    if (g_fsr_read_depth++ > 0) return;
    active = true;
    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_renderFBO);
}
inc_fsr_read_scope_t::~inc_fsr_read_scope_t() {
    if (!counted) return;
    --g_fsr_read_depth;

    if (active) GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void update_attachment(GLenum target, GLenum attachment, const attachment_t& what) {
    GLuint current_fbo = (target == GL_READ_FRAMEBUFFER) ? current_read_fbo : current_draw_fbo;
    if (current_fbo == 0) return;
    if (attachment < GL_COLOR_ATTACHMENT0 || attachment >= GL_COLOR_ATTACHMENT0 + (GLenum)max_color_attachments_or_default()) {
        return;
    }
    framebuffer_t& fbo = get_framebuffer(current_fbo);
    init_framebuffer(fbo);
    const size_t index = attachment - GL_COLOR_ATTACHMENT0;
    if (index >= fbo.color_attachments.size()) return;
    fbo.color_attachments[index] = what;

    if (index < fbo.draw_buffer_map.size()) fbo.draw_buffer_map[index] = 0;
}

void restore_home_attachments(framebuffer_t& fbo);

void reattach(GLenum target, GLenum attachment, const attachment_t& a) {
    switch (a.kind) {
    case attach_kind_t::Texture2D:
        GLES.glFramebufferTexture2D(target, attachment, a.textarget, a.texture, a.level);
        break;
    case attach_kind_t::TextureLayer:
        GLES.glFramebufferTextureLayer(target, attachment, a.texture, a.level, a.layer);
        break;
    case attach_kind_t::TextureAll:
        GLES.glFramebufferTexture(target, attachment, a.texture, a.level);
        break;
    case attach_kind_t::Renderbuffer:
        GLES.glFramebufferRenderbuffer(target, attachment, a.textarget, a.texture);
        break;
    case attach_kind_t::None:

        break;
    }
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    update_attachment(target, attachment, {attach_kind_t::Texture2D, textarget, texture, level, 0});
    GLES.glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {

    update_attachment(target, attachment, {attach_kind_t::TextureAll, 0, texture, level, 0});
    GLES.glFramebufferTexture(target, attachment, texture, level);
}

void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    update_attachment(target, attachment, {attach_kind_t::TextureLayer, 0, texture, level, layer});
    GLES.glFramebufferTextureLayer(target, attachment, texture, level, layer);
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    update_attachment(target, attachment, {attach_kind_t::Renderbuffer, renderbuffertarget, renderbuffer, 0, 0});
    GLES.glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}

void restore_home_attachments(framebuffer_t& fbo) {
    if (fbo.draw_buffer_map.empty()) return;

    for (size_t idx = 0; idx < fbo.color_attachments.size(); ++idx) {
        const attachment_t& a = fbo.color_attachments[idx];
        if (a.kind == attach_kind_t::None) continue;
        reattach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)idx, a);
    }
    fbo.draw_buffer_map.clear();
}

extern "C" {
GLAPI GLAPIENTRY void glDeleteFramebuffersARB(GLsizei n, const GLuint* names) __attribute__((alias("glDeleteFramebuffers")));
GLAPI GLAPIENTRY void glFramebufferRenderbufferARB(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                   GLuint renderbuffer) __attribute__((alias("glFramebufferRenderbuffer")));
GLAPI GLAPIENTRY void glFramebufferTextureLayerARB(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                   GLint layer) __attribute__((alias("glFramebufferTextureLayer")));
}

void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                       GLint dstY1, GLbitfield mask, GLenum filter) {
    LOG()
    inc_fsr_read_scope_t fsr_read;

    if (inc_fsr1_render_scaled()) {
        if (current_read_fbo == 0) inc_fsr1_scale_corners(&srcX0, &srcY0, &srcX1, &srcY1);
        if (FSR1_Context::g_renderFBO != 0 && current_draw_fbo == FSR1_Context::g_renderFBO)
            inc_fsr1_scale_corners(&dstX0, &dstY0, &dstX1, &dstY1);
    }
    GLES.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    CHECK_GL_ERROR
}

void glDeleteFramebuffers(GLsizei n, const GLuint* names) {

    if (names != nullptr) {
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint name = names[i];
            if (name == 0) continue;
            framebuffers.erase(name);

            if (current_draw_fbo == name) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            if (current_read_fbo == name) glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }
    }
    GLES.glDeleteFramebuffers(n, names);
}

static GLenum inc_default_fb_buffer(GLenum b) {
    switch (b) {
    case 0x0400:
    case 0x0401:
    case 0x0402:
    case 0x0403:
    case 0x0404:
    case 0x0406:
    case 0x0407:
    case 0x0408:
        return GL_BACK;
    default:
        return b;
    }
}
void glDrawBuffer(GLenum buffer) {
    LOG()
    LOG_D("glDrawBuffer %d", buffer)

    if (current_draw_fbo == 0) {
        GLenum buffers[] = {buffer};
        glDrawBuffers(1, buffers);
    } else {

        if (current_draw_fbo == FSR1_Context::g_renderFBO &&
            (buffer == GL_BACK || buffer == GL_FRONT || buffer == GL_FRONT_AND_BACK || buffer == GL_LEFT ||
             buffer == GL_BACK_LEFT || buffer == GL_FRONT_LEFT)) {
            LOG_D("glDrawBuffer 0x%x on the FSR1 target -> GL_COLOR_ATTACHMENT0", buffer)
            buffer = GL_COLOR_ATTACHMENT0;
        }

        const GLint maxAttachments = max_color_attachments_or_default();

        if (buffer == GL_NONE) {
            get_framebuffer(current_draw_fbo).color_attachments_all_none = true;
            std::vector<GLenum> buffers(maxAttachments, GL_NONE);
            glDrawBuffers(maxAttachments, buffers.data());
        } else if (buffer >= GL_COLOR_ATTACHMENT0 && buffer < GL_COLOR_ATTACHMENT0 + maxAttachments) {
            get_framebuffer(current_draw_fbo).color_attachments_all_none = false;
            std::vector<GLenum> buffers(maxAttachments, GL_NONE);
            buffers[buffer - GL_COLOR_ATTACHMENT0] = buffer;
            glDrawBuffers(maxAttachments, buffers.data());
        }
    }
    CHECK_GL_ERROR;
}
void glDrawBuffers(GLsizei n, const GLenum* bufs) {
    LOG()
    if (current_draw_fbo == 0) {
        GLenum fixed[16];
        if (bufs && n > 0 && n <= 16) {
            for (GLsizei i = 0; i < n; ++i) fixed[i] = inc_default_fb_buffer(bufs[i]);
            GLES.glDrawBuffers(n, fixed);
        } else {
            GLES.glDrawBuffers(n, bufs);
        }
        CHECK_GL_ERROR
        return;
    }

    framebuffer_t& fbo = get_framebuffer(current_draw_fbo);
    init_framebuffer(fbo);

    bool all_none = true;
    for (int i = 0; i < n; ++i) {
        if (bufs[i] != GL_NONE) {
            all_none = false;
            break;
        }
    }

    if (all_none) {
        LOG_D("glDrawBuffers, fb %d all_none true", current_draw_fbo)
        fbo.color_attachments_all_none = true;
        GLES.glDrawBuffers(n, bufs);
        return;
    } else {
        LOG_D("glDrawBuffers, fb %d all_none false", current_draw_fbo)
        fbo.color_attachments_all_none = false;
    }

    bool identity = true;
    for (int i = 0; i < n; i++) {
        if (bufs[i] != GL_COLOR_ATTACHMENT0 + (GLenum)i) {
            identity = false;
            break;
        }
    }
    if (identity) {
        LOG_D("glDrawBuffers, fb %d identity order, nothing to move", current_draw_fbo)
        restore_home_attachments(fbo);
        GLES.glDrawBuffers(n, bufs);
        return;
    }

    for (int i = 0; i < n; i++) {
        if (bufs[i] < GL_COLOR_ATTACHMENT0 || bufs[i] >= GL_COLOR_ATTACHMENT0 + (GLenum)max_color_attachments_or_default()) continue;
        if (bufs[i] - GL_COLOR_ATTACHMENT0 >= fbo.color_attachments.size()) continue;
        if (fbo.color_attachments[bufs[i] - GL_COLOR_ATTACHMENT0].kind != attach_kind_t::None) continue;

        FB_WARN_ONCE("glDrawBuffers: fb %u wants attachment %u in slot %d but nothing is recorded there; "
                     "passing the list through rather than detaching it",
                     current_draw_fbo, bufs[i] - GL_COLOR_ATTACHMENT0, i);
        restore_home_attachments(fbo);
        GLES.glDrawBuffers(n, bufs);
        return;
    }

    restore_home_attachments(fbo);

    std::vector<GLenum> new_bufs(n);
    fbo.draw_buffer_map.assign(max_color_attachments_or_default(), 0);
    for (int i = 0; i < n; i++) {
        if (bufs[i] >= GL_COLOR_ATTACHMENT0 && bufs[i] < GL_COLOR_ATTACHMENT0 + (GLenum)max_color_attachments_or_default()) {
            GLenum logical_attachment = bufs[i];
            GLenum physical_attachment = GL_COLOR_ATTACHMENT0 + i;
            new_bufs[i] = physical_attachment;
            size_t index = logical_attachment - GL_COLOR_ATTACHMENT0;
            if (index >= fbo.color_attachments.size() || index >= fbo.draw_buffer_map.size()) {
                new_bufs[i] = bufs[i];
                continue;
            }
            reattach(GL_DRAW_FRAMEBUFFER, physical_attachment, fbo.color_attachments[index]);

            fbo.draw_buffer_map[index] = physical_attachment;
        } else {
            new_bufs[i] = bufs[i];
        }
    }
    GLES.glDrawBuffers(n, new_bufs.data());
}
void glReadBuffer(GLenum src) {
    if (current_read_fbo != 0 && src >= GL_COLOR_ATTACHMENT0 && src < GL_COLOR_ATTACHMENT0 + max_color_attachments_or_default()) {
        framebuffer_t& fbo = get_framebuffer(current_read_fbo);
        init_framebuffer(fbo);
        const size_t index = static_cast<size_t>(src - GL_COLOR_ATTACHMENT0);

        if (index < fbo.draw_buffer_map.size() && fbo.draw_buffer_map[index] != 0) {
            GLES.glReadBuffer(fbo.draw_buffer_map[index]);
            return;
        }
    }
    GLES.glReadBuffer(current_read_fbo == 0 ? inc_default_fb_buffer(src) : src);
    CHECK_GL_ERROR
}
GLenum glCheckFramebufferStatus(GLenum target) {
    GLenum status = GLES.glCheckFramebufferStatus(target);
    if (global_settings.ignore_error == IgnoreErrorLevel::Full && status != GL_FRAMEBUFFER_COMPLETE) {
        return GL_FRAMEBUFFER_COMPLETE;
    }
    return status;
}

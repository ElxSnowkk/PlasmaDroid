// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/framebuffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header



// How an attachment got attached, so the draw-buffer shuffle can put it back the
// same way.
//
// There used to be no such field, and the shuffle assumed every attachment had
// arrived through glFramebufferTexture2D. Everything else -- renderbuffers,
// layered attachments -- left the record at all zeros, and re-attaching a zero
// record is a detach: a framebuffer whose colour attachment was a renderbuffer
// lost it the first time the application called glDrawBuffers.







// The framebuffer table of the current context. It used to be a plain global
// that gl/gl.cpp reached by extern; it is per-context now, so it has to be
// fetched rather than named.
// True when the draw framebuffer currently bound in *this* context has had every
// colour attachment set to GL_NONE. Answers false for an id this context has
// never bound, which the caller could not check when it indexed the table
// directly from another translation unit.

// Points the backend READ binding at the FSR1 render target for as long as it
// lives, and only when the application is reading its own framebuffer 0 while
// FSR1 is on. A no-op in every other case.
//
// glBindFramebuffer redirects the DRAW binding of framebuffer 0 to the FSR1
// target but deliberately leaves the READ binding on the window surface, and the
// upscale only reaches that surface inside ApplyFSR at swap time. So everything
// that reads framebuffer 0 -- glReadPixels for a screenshot, glCopyTexSubImage2D
// for a scene copy, and the depth blits, whose depth buffer lives on the render
// target and never on the surface at all -- was reading the previous frame's
// upscaled output, or undefined content after eglSwapBuffers, instead of the
// frame the application had just drawn.

#ifndef INCANDESCENT_FRAMEBUFFER_H
#define INCANDESCENT_FRAMEBUFFER_H

#include <vector>
#include <GL/gl.h>
#include <cstddef>

enum class attach_kind_t : unsigned char {
    None = 0,
    Texture2D,
    TextureLayer,
    TextureAll,
    Renderbuffer,
};

struct attachment_t {
    attach_kind_t kind = attach_kind_t::None;
    GLenum textarget = 0;
    GLuint texture = 0;
    GLint level = 0;
    GLint layer = 0;
};
struct framebuffer_t {
    bool initialized = false;
    bool color_attachments_all_none = false;

    std::vector<attachment_t> color_attachments;

    std::vector<GLenum> draw_buffer_map;

};

#ifdef __cplusplus
extern "C"
{
#endif

    GLint getMaxDrawBuffers();

    GLAPI GLAPIENTRY void glBindFramebuffer(GLenum target, GLuint framebuffer);
    GLAPI GLAPIENTRY void glDeleteFramebuffers(GLsizei n, const GLuint* names);
    GLAPI GLAPIENTRY void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                            GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    GLAPI GLAPIENTRY void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                                 GLint level);
    GLAPI GLAPIENTRY void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
    GLAPI GLAPIENTRY void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                    GLint layer);
    GLAPI GLAPIENTRY void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                    GLuint renderbuffer);
    GLAPI GLAPIENTRY void glDrawBuffer(GLenum buf);
    GLAPI GLAPIENTRY void glDrawBuffers(GLsizei n, const GLenum* bufs);
    GLAPI GLAPIENTRY void glReadBuffer(GLenum src);
    GLAPI GLAPIENTRY GLenum glCheckFramebufferStatus(GLenum target);

#ifdef __cplusplus
}
#endif

void InitFramebufferMap(size_t expectedSize);

bool inc_draw_framebuffer_all_none();

struct inc_fsr_read_scope_t {
    bool active = false;
    bool counted = false;
    inc_fsr_read_scope_t();
    ~inc_fsr_read_scope_t();
    inc_fsr_read_scope_t(const inc_fsr_read_scope_t&) = delete;
    inc_fsr_read_scope_t& operator=(const inc_fsr_read_scope_t&) = delete;
};

#endif

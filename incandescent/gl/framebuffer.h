

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

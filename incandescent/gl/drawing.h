

#ifndef INCANDESCENT_DRAWING_H
#define INCANDESCENT_DRAWING_H

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <GLES3/gl32.h>
#include "../includes.h"
#include <GL/gl.h>
#include "glcorearb.h"
#include "log.h"
#include "../gles/loader.h"
#include "inc.h"

struct SamplerInfo {
    GLint locWidth;
    GLint locHeight;
    std::vector<GLint> samplers;
};

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                  GLsizei primcount);
    GLAPI GLAPIENTRY void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLint basevertex);
    GLAPI GLAPIENTRY void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type,
                                              const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);

    GLAPI GLAPIENTRY void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                              const void* indices);
    GLAPI GLAPIENTRY void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                                        GLenum type, const void* indices, GLint basevertex);
    GLAPI GLAPIENTRY void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                                            const void* indices, GLsizei instancecount,
                                                            GLint basevertex);

    GLAPI GLAPIENTRY void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count,
                                                            GLsizei instancecount, GLuint baseinstance);
    GLAPI GLAPIENTRY void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                              const void* indices, GLsizei instancecount,
                                                              GLuint baseinstance);
    GLAPI GLAPIENTRY void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                                        const void* indices, GLsizei instancecount,
                                                                        GLint basevertex, GLuint baseinstance);

    GLAPI GLAPIENTRY void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer,
                                             GLenum access, GLenum format);
    GLAPI GLAPIENTRY void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
    GLAPI GLAPIENTRY void glMemoryBarrier(GLbitfield barriers);
    GLAPI GLAPIENTRY void glUniform1i(GLint location, GLint v0);

#ifdef __cplusplus
}
#endif

#endif

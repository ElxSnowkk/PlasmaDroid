

#ifndef INCANDESCENT_MULTIDRAW_H
#define INCANDESCENT_MULTIDRAW_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <GLES3/gl32.h>
#include "../includes.h"
#include <GL/gl.h>
#include "glcorearb.h"
#include "log.h"
#include "../gles/loader.h"
#include "inc.h"

#ifdef __cplusplus
extern "C"
{
#endif

    struct draw_elements_indirect_command_t {
        GLuint count;
        GLuint instanceCount;
        GLuint firstIndex;
        GLint baseVertex;
        GLuint reservedMustBeZero;
    };

    struct draw_arrays_indirect_command_t {
        GLuint count;
        GLuint instanceCount;
        GLuint first;
        GLuint baseInstanceOrReserved;
    };

    struct drawcmd_compute_t {
        GLuint firstIndex;
        GLint baseVertex;
    };

    GLAPI GLAPIENTRY void glMultiDrawElementsBaseVertex(GLenum mode, GLsizei* counts, GLenum type,
                                                        const void* const* indices, GLsizei primcount,
                                                        const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_indirect(GLenum mode, GLsizei* counts, GLenum type,
                                                                    const void* const* indices, GLsizei primcount,
                                                                    const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_multiindirect(GLenum mode, GLsizei* counts, GLenum type,
                                                                         const void* const* indices, GLsizei primcount,
                                                                         const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_basevertex(GLenum mode, GLsizei* counts, GLenum type,
                                                                      const void* const* indices, GLsizei primcount,
                                                                      const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_drawelements(GLenum mode, GLsizei* counts, GLenum type,
                                                                        const void* const* indices, GLsizei primcount,
                                                                        const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_compute(GLenum mode, GLsizei* counts, GLenum type,
                                                                   const void* const* indices, GLsizei primcount,
                                                                   const GLint* basevertex);
    GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_multibasevertex(GLenum mode, GLsizei* counts, GLenum type,
                                                                  const void* const* indices, GLsizei primcount,
                                                                  const GLint* basevertex);

    GLAPI GLAPIENTRY void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type,
                                              const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_indirect(GLenum mode, const GLsizei* count, GLenum type,
                                                          const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_multiindirect(GLenum mode, const GLsizei* count, GLenum type,
                                                               const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_basevertex(GLenum mode, const GLsizei* count, GLenum type,
                                                            const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_drawelements(GLenum mode, const GLsizei* count, GLenum type,
                                                              const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_compute(GLenum mode, const GLsizei* count, GLenum type,
                                                         const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_multibasevertex(GLenum mode, const GLsizei* count, GLenum type,
                                                        const void* const* indices, GLsizei primcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawElements_multiarrays(GLenum mode, const GLsizei* count, GLenum type,
                                                           const void* const* indices, GLsizei primcount);

    GLAPI GLAPIENTRY void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawArrays_unroll(GLenum mode, const GLint* first, const GLsizei* count,
                                                            GLsizei drawcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawArrays_multiarrays(GLenum mode, const GLint* first, const GLsizei* count,
                                                         GLsizei drawcount);
    GLAPI GLAPIENTRY void inc_glMultiDrawArrays_multiindirect(GLenum mode, const GLint* first, const GLsizei* count,
                                                             GLsizei drawcount);
    GLAPI GLAPIENTRY void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount,
                                                    GLsizei stride);
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect,
                                                      GLsizei drawcount, GLsizei stride);

    GLAPI GLAPIENTRY void glMultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                                         GLsizei maxdrawcount, GLsizei stride);
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                                           GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);

#ifdef __cplusplus
}
#endif

#endif

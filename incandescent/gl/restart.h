

#ifndef INCANDESCENT_RESTART_H
#define INCANDESCENT_RESTART_H

#include "../includes.h"
#include <GL/gl.h>

#ifdef __cplusplus
extern "C"
{
#endif

    bool inc_restart_needs_rewrite(GLenum type);

    bool inc_restart_needs_driver_fixed(GLenum type);

    bool inc_draw_elements_restart(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex,
                                  GLsizei instancecount);

    void inc_restart_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif

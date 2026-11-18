

#ifndef INCANDESCENT_VERTEXATTRIB_H
#define INCANDESCENT_VERTEXATTRIB_H

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

    GLAPI GLAPIENTRY void glVertexAttribI1ui(GLuint index, GLuint x);
    GLAPI GLAPIENTRY void glVertexAttribI2ui(GLuint index, GLuint x, GLuint y);
    GLAPI GLAPIENTRY void glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z);

#ifdef __cplusplus
}
#endif

#endif

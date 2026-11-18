

#ifndef INCANDESCENT_PROGRAM_H
#define INCANDESCENT_PROGRAM_H

#include <GL/gl.h>

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name);
    GLAPI GLAPIENTRY void glLinkProgram(GLuint program);
    GLAPI GLAPIENTRY void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
    GLAPI GLAPIENTRY void glUseProgram(GLuint program);
    GLAPI GLAPIENTRY GLuint glCreateProgram();
    GLAPI GLAPIENTRY void glAttachShader(GLuint program, GLuint shader);
    GLAPI GLAPIENTRY GLuint glCreateShader(GLenum shaderType);
    GLAPI GLAPIENTRY void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                                                 GLchar* uniformName);

#ifdef __cplusplus
}
#endif

#endif

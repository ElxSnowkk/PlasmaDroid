

#ifndef INCANDESCENT_SHADER_H
#define INCANDESCENT_SHADER_H

#include <GL/gl.h>
#include <string>

struct shader_t {
    GLuint id;
    std::string converted;

    std::string frag_data_changed_converted;
    int frag_data_changed;
};

extern struct shader_t shaderInfo;

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                                         const GLint* length);

    GLAPI GLAPIENTRY void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);

#ifdef __cplusplus
}
#endif

#endif

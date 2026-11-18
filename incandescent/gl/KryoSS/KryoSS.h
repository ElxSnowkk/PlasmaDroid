

#pragma once

#include "../../gles/gles.h"

namespace KryoSS {

const char* FragmentSource();

const char* ComputeSource();

void BindProgramUniforms(GLuint program);
void BindComputeUniforms(GLuint program);
}

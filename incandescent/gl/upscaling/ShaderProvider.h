

#pragma once

#include "../../gles/gles.h"

namespace Upscaling {
const char* FragmentSource();
const char* ComputeSource();
void BindProgramUniforms(GLuint program);
void BindComputeUniforms(GLuint program);
}

// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// KryoSS spatial upscaler — independent Incandescent module.
// SPDX-License-Identifier: LGPL-2.1-only


// GLSL source selected from the configured quality tier. The tier is fixed for
// the lifetime of the program, so this is intentionally built once per process.
// Compute path is available on ES 3.1+ and writes to an RGBA8 image.

// Called with the corresponding program current. Static params are uploaded once.

#pragma once

#include "../../gles/gles.h"

namespace KryoSS {

const char* FragmentSource();

const char* ComputeSource();

void BindProgramUniforms(GLuint program);
void BindComputeUniforms(GLuint program);
}

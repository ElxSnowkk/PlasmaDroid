// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// KryoSS spatial upscaler — independent Incandescent module.
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include "../../gles/gles.h"

namespace KryoSS {

const char* FragmentSource();

const char* ComputeSource();

void BindProgramUniforms(GLuint program);
void BindComputeUniforms(GLuint program);
}

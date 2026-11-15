// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Generic upscaler shader selection for Incandescent.
// FSR1 and KryoSS provide independent fragment implementations.
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include "../../gles/gles.h"

namespace Upscaling {
const char* FragmentSource();
const char* ComputeSource();
void BindProgramUniforms(GLuint program);
void BindComputeUniforms(GLuint program);
}

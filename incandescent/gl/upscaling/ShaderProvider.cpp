

#include "ShaderProvider.h"

#include "../FSR1/FSRShaderSource.h"
#include "../KryoSS/KryoSS.h"
#include "../../config/settings.h"

namespace Upscaling {
const char* FragmentSource() {
    return global_settings.upscaler_mode == 1 ? KryoSS::FragmentSource() : FSR_FSSource;
}

const char* ComputeSource() {
    return global_settings.upscaler_mode == 1 ? KryoSS::ComputeSource() : nullptr;
}

void BindProgramUniforms(GLuint program) {
    if (global_settings.upscaler_mode == 1) KryoSS::BindProgramUniforms(program);
}

void BindComputeUniforms(GLuint program) {
    if (global_settings.upscaler_mode == 1) KryoSS::BindComputeUniforms(program);
}
}

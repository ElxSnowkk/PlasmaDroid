

#include "KryoSS.h"

#include "KryoSSParams.h"
#include "KryoSSShaderSource.h"
#include "KryoSSComputeSource.h"
#include "../../config/settings.h"
#include "../inc.h"
#include "../log.h"

#include <string>

#define DEBUG 0

namespace {
int SelectedTier() {
    const int tier = global_settings.upscaler_tier;
    return (tier >= 0 && tier < kryoss::TIER_COUNT) ? tier : kryoss::BALANCED;
}
}

namespace KryoSS {
const char* FragmentSource() {

    static const std::string source = std::string("#version 450\n#define KRYOSS_MODE ") +
                                      std::to_string(SelectedTier()) + "\n" + KRYOSS_FSBody;
    return source.c_str();
}

const char* ComputeSource() {
    static const std::string source = std::string("#version 450\n#define KRYOSS_MODE ") +
                                      std::to_string(SelectedTier()) + "\n" + KRYOSS_CSBody;
    return source.c_str();
}

void BindProgramUniforms(GLuint program) {
    const GLint paramsLoc = GLES.glGetUniformLocation(program, "uKryoss");
    const GLint params2Loc = GLES.glGetUniformLocation(program, "uKryoss2");
    const kryoss::Params params = kryoss::MakeParams(SelectedTier(), global_settings.upscaler_sharpness,
                                                      global_settings.upscaler_detail);
    if (paramsLoc >= 0) GLES.glUniform4fv(paramsLoc, 1, params.u1);
    if (params2Loc >= 0) GLES.glUniform4fv(params2Loc, 1, params.u2);

    LOG_D("KryoSS v5 fragment: tier=%d taps=%d sharp=%d detail=%d", SelectedTier(), kryoss::TapsFor(SelectedTier()),
          global_settings.upscaler_sharpness, global_settings.upscaler_detail);
}

void BindComputeUniforms(GLuint program) {
    const GLint inputLoc = GLES.glGetUniformLocation(program, "uInputTex");
    const GLint paramsLoc = GLES.glGetUniformLocation(program, "uKryoss");
    const GLint params2Loc = GLES.glGetUniformLocation(program, "uKryoss2");
    const GLint outputLoc = GLES.glGetUniformLocation(program, "uOutputImage");
    const kryoss::Params params = kryoss::MakeParams(SelectedTier(), global_settings.upscaler_sharpness,
                                                      global_settings.upscaler_detail);
    if (inputLoc >= 0) GLES.glUniform1i(inputLoc, 0);
    if (outputLoc >= 0) GLES.glUniform1i(outputLoc, 0);
    if (paramsLoc >= 0) GLES.glUniform4fv(paramsLoc, 1, params.u1);
    if (params2Loc >= 0) GLES.glUniform4fv(params2Loc, 1, params.u2);
    LOG_D("KryoSS v5 compute: tier=%d taps=%d sharp=%d detail=%d", SelectedTier(), kryoss::TapsFor(SelectedTier()),
          global_settings.upscaler_sharpness, global_settings.upscaler_detail);
}
}

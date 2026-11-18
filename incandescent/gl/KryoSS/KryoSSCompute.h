
#pragma once
#include <string>
#include "KryoSSComputeSource.h"

namespace kryoss_cs {

struct State {
    bool use = false;
    GLuint program = 0;
    GLint viewLoc = -1, outLoc = -1;
};
inline State g;

inline bool Supported() {
    return GLES.glDispatchCompute && GLES.glBindImageTexture && GLES.glMemoryBarrier && GLES.glTexStorage2D &&
           GLES.glCreateShader && GLES.glShaderSource && GLES.glCompileShader;
}

inline GLuint Build(int tier) {
    const std::string src = std::string("#version 310 es\n#define KRYOSS_MODE ") + std::to_string(tier) + "\n" + KRYOSS_CSBody;
    const char* p = src.c_str();
    GLuint cs = GLES.glCreateShader(GL_COMPUTE_SHADER);
    GLES.glShaderSource(cs, 1, &p, nullptr);
    GLES.glCompileShader(cs);
    GLint ok = 0;
    GLES.glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        GLES.glGetShaderInfoLog(cs, sizeof(log) - 1, nullptr, log);
        LOG_F("KryoSS compute shader error (usando o fragment shader): %s\n", log);
        GLES.glDeleteShader(cs);
        return 0;
    }
    GLuint prog = GLES.glCreateProgram();
    GLES.glAttachShader(prog, cs);
    GLES.glLinkProgram(prog);
    GLES.glDeleteShader(cs);
    GLES.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        GLES.glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        LOG_F("KryoSS compute link error (usando o fragment shader): %s\n", log);
        GLES.glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

inline void Init(int tier, int sharpPct, int detailPct) {
    g = State();
    if (global_settings.upscaler_mode != 1 || global_settings.upscaler_compute == 0 || !Supported()) return;
    g.program = Build(tier);
    if (!g.program) return;
    g.viewLoc = GLES.glGetUniformLocation(g.program, "uViewportSize");
    g.outLoc = GLES.glGetUniformLocation(g.program, "uOutSize");
    const GLint k1 = GLES.glGetUniformLocation(g.program, "uKryoss");
    const GLint k2 = GLES.glGetUniformLocation(g.program, "uKryoss2");
    const GLint tex = GLES.glGetUniformLocation(g.program, "uInputTex");
    const kryoss::Params kp = kryoss::MakeParams(tier, sharpPct, detailPct, true);
    GLES.glUseProgram(g.program);
    GLES.glUniform1i(tex, 0);
    GLES.glUniform4fv(k1, 1, kp.u1);
    GLES.glUniform4fv(k2, 1, kp.u2);
    GLES.glUseProgram(0);
    g.use = true;
    LOG_D("KryoSS v4 COMPUTE: tier=%d taps=%d sharp=%d detail=%d", tier, kryoss::TapsFor(tier, true), sharpPct, detailPct);
}

inline void AllocTarget(GLsizei w, GLsizei h) {
    if (GLES.glTexStorage2D) GLES.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    else GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

inline void Dispatch(GLuint targetTex, int inW, int inH, int outW, int outH) {
    GLES.glUseProgram(g.program);
    GLES.glUniform2f(g.viewLoc, (float)inW, (float)inH);
    GLES.glUniform2f(g.outLoc, (float)outW, (float)outH);
    GLES.glBindImageTexture(0, targetTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    GLES.glDispatchCompute((GLuint)((outW + 15) / 16), (GLuint)((outH + 15) / 16), 1);

    GLES.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    GLES.glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
}

}

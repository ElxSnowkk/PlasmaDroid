// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/FSR1/FSR1.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header


// Precisa vir DEPOIS de settings.h e de "#define DEBUG": usa LOG_*, DEBUG e global_settings do contexto deste arquivo.

// Which pieces of GL state a body in this file overwrites. Saving the rest is not
// free: everything the guard cannot answer from this layer's own tracking is a
// driver round trip, and the upscale runs once per presented frame.

// Saves the GL state the bodies in this file overwrite and puts it back.
//
// Answered from this layer's own tracking, at no driver cost:
//   - the current program. gl/program.cpp already treats gl_state->current_program
//     as the truth -- it drops a glUseProgram that repeats it -- and program names
//     are not renamed on the way to GLES, so the tracked value is the driver's.
//   - the draw framebuffer. gl/framebuffer.cpp writes gl_state->current_draw_fbo
//     with the name it hands the driver, the redirect of framebuffer 0 to the FSR1
//     render target already resolved, and it is the only file that binds a draw
//     framebuffer through GLES other than this one. It also keeps that field off
//     deleted names, through its own glDeleteFramebuffers -- with one exception,
//     RecreateFSRFBO, which deletes the render FBO behind its back and so has to
//     republish the replacement itself. A saved name has to be live: restoring one
//     GL has deleted is rejected, and the binding then stays wherever the body left
//     it.
//
// Asked of the driver, because nothing in the tree can answer:
//   - the vertex array. What this layer tracks is the application's name, the
//     mapping to the driver's lives in gl/buffer.cpp and is not exported, and the
//     tracked name outlives glDeleteVertexArrays -- restoring from it could hand
//     GLES a name it never generated.
//   - the active unit and unit 0's GL_TEXTURE_2D binding. gl/texture.h's driver
//     shadow declines to answer while FSR1 is enabled, which is exactly when this
//     runs. The active unit has to come from the driver in any case: these guards
//     nest, the moves below go straight to GLES and so never reach that shadow, and
//     an inner guard reading it would restore the outer guard's unit and leave the
//     body running on a unit it never asked for.
//   - the read framebuffer and the renderbuffer binding, which nothing tracks.
//
// The texture entry is unit 0, not whichever unit happened to be active. Unit 0 is
// the unit ApplyFSR samples the render texture from, so it is the binding that has
// to be preserved; saving the active unit's instead left the FSR1 render texture on
// unit 0 once per presented frame with nothing anywhere to put the application's
// texture back. gl/texture.cpp's driver-side shadow was narrowed around that leak
// and can be widened again now that it is gone.









// KryoSS v4: modo (0 Ultra Desempenho, 1 Desempenho, 2 Balanceado, 3 Qualidade) lido de upscalerTier.

// Monta o fragment shader do KryoSS: "#version 450" + "#define KRYOSS_MODE n" + corpo (gl/KryoSS/KryoSSShaderSource.h).
// Uma string estatica por processo: o modo nao muda em execucao (vem do config.json lido na inicializacao).

// Escolhe o fragment shader: FSR1 (padrao) ou KryoSS (upscaler_mode == 1).

// Override de escala (Incandescent): se upscaler_scale_pct em 50..99, scale = 100 / pct.

// Tamanho da janela/superficie como o JOGO a ve (eglQuerySurface; ex.: 1280x720 do Zalith). Com a reducao do render ligada ele e maior que
// g_renderWidth/Height (a resolucao em que o jogo realmente desenha); sem ela os dois sao iguais.

// Resolucao interna do KryoSS = "Resolucao minima base" (upscalerMinBase, px de altura): e a resolucao em que o jogo desenha e que o
// KryoSS escala ate a meta. Largura pela proporcao da janela; nunca acima da janela. Ex.: janela 720p, base 540 -> 960x540.
// Sem a chave (0) ou fora do KryoSS: renderiza na resolucao da janela (comportamento anterior).

// true quando o jogo desenha menor que a janela e as coordenadas dele (viewport, scissor) precisam ser encolhidas.

// O jogo desenha no "framebuffer 0" (redirecionado para o FBO de render)? So nesse caso as coordenadas em pixels da janela valem.
// Os FBOs do proprio jogo (sombras, pos-processamento) continuam no tamanho que ele pediu.


















// Textura RGBA8 de 1 nivel, imutavel (GLES 3.0 glTexStorage2D): formato/tamanho fixos desde a criacao, menos validacao no driver e sem
// re-especificacao acidental (as texturas so sao criadas/destruidas em InitFSRResources/RecreateFSRFBO). Cai no glTexImage2D se o
// ponteiro nao foi carregado.


















































// Usados por gl/framebuffer.cpp (glBlitFramebuffer): o jogo copia de/para o framebuffer 0 com coordenadas da janela.

// ---------------------------------------------------------------------------



// Plain value, not a unique_ptr like the other per-context tables: nothing here
// keeps the address of an entry. Both operator[] calls in inc_fsr1_bind_context
// are separate statements, so the first reference is dead before the second one
// can rehash the map.

#include "FSR1.h"
#include "../KryoSS/KryoSSResolution.h"
#include <mutex>
#include <ska/flat_hash_map.hpp>
#include "FSRShaderSource.h"
#include "../KryoSS/KryoSSShaderSource.h"
#include "../KryoSS/KryoSSParams.h"
#include <string>
#include "../../config/settings.h"
#include "../mali_tuning.h"

#define DEBUG 0

#include "../KryoSS/KryoSSCompute.h"

enum GLStateBits : unsigned int {
    GUARD_PROGRAM = 1u << 0,
    GUARD_VAO = 1u << 1,
    GUARD_ARRAY_BUFFER = 1u << 2,

    GUARD_TEXTURE = 1u << 3,
    GUARD_FRAMEBUFFER = 1u << 4,
    GUARD_RENDERBUFFER = 1u << 5,
};

struct GLStateGuard {
    unsigned int saved;
    GLint prevProgram = 0;
    GLint prevVAO = 0;
    GLint prevArrayBuffer = 0;
    GLint prevActiveTexture = GL_TEXTURE0;
    GLint prevTexture = 0;
    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevRenderbuffer = 0;

    explicit GLStateGuard(unsigned int bits) : saved(bits) {
        if (saved & GUARD_PROGRAM) prevProgram = static_cast<GLint>(gl_state->current_program);
        if (saved & GUARD_VAO) GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        if (saved & GUARD_ARRAY_BUFFER) GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        if (saved & GUARD_TEXTURE) {
            GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
        }
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
            prevDrawFBO = static_cast<GLint>(gl_state->current_draw_fbo);
        }
        if (saved & GUARD_RENDERBUFFER) GLES.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
    }

    void framebuffer_recreated(GLuint from, GLuint to) {
        if (!(saved & GUARD_FRAMEBUFFER) || from == 0 || from == to) return;
        if (prevReadFBO == static_cast<GLint>(from)) prevReadFBO = static_cast<GLint>(to);
        if (prevDrawFBO == static_cast<GLint>(from)) prevDrawFBO = static_cast<GLint>(to);
    }

    ~GLStateGuard() {
        if (saved & GUARD_PROGRAM) GLES.glUseProgram(prevProgram);
        if (saved & GUARD_VAO) GLES.glBindVertexArray(prevVAO);
        if (saved & GUARD_ARRAY_BUFFER) GLES.glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuffer);
        if (saved & GUARD_TEXTURE) {

            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glBindTexture(GL_TEXTURE_2D, prevTexture);
            GLES.glActiveTexture(prevActiveTexture);
        }
        if (saved & GUARD_RENDERBUFFER) GLES.glBindRenderbuffer(GL_RENDERBUFFER, prevRenderbuffer);
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
            GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        }
    }
};

namespace FSR1_Context {
    GLuint g_renderFBO = 0;
    GLuint g_renderTexture = 0;
    GLuint g_depthStencilRBO = 0;
    GLuint g_quadVAO = 0;
    GLuint g_quadVBO = 0;
    GLuint g_fsrProgram = 0;

    GLint g_inputTexLoc = -1;
    GLint g_const0Loc = -1;
    GLint g_viewportSizeLoc = -1;

    GLuint g_targetFBO = 0;
    GLuint g_targetTexture = 0;

    GLuint g_currentDrawFBO = 0;
    GLint g_viewport[4] = {0};
    GLsizei g_targetWidth = 2400;
    GLsizei g_targetHeight = 1080;
    GLsizei g_renderWidth = 1200;
    GLsizei g_renderHeight = 540;
    bool g_dirty = false;

    bool g_resolutionChanged = false;
    GLsizei g_pendingWidth = 0;
    GLsizei g_pendingHeight = 0;
}

static int KryossTier() {
    const int t = global_settings.upscaler_tier;
    return (t >= 0 && t < kryoss::TIER_COUNT) ? t : kryoss::BALANCED;
}

static const char* KryossSource() {
    static const std::string src = std::string("#version 450\n#define KRYOSS_MODE ") + std::to_string(KryossTier()) + "\n" +
                                   KRYOSS_FSBody;
    return src.c_str();
}

static const char* SelectFragmentSource() {
    return global_settings.upscaler_mode == 1 ? KryossSource() : FSR_FSSource;
}

static float ApplyScaleOverride(float scale) {
    const int pct = global_settings.upscaler_scale_pct;
    return (pct >= 50 && pct <= 99) ? 100.0f / static_cast<float>(pct) : scale;
}

static GLsizei s_surfW = 0, s_surfH = 0;

static void ComputeRenderSize(int surfW, int surfH, int* rw, int* rh) {
    *rw = surfW;
    *rh = surfH;
    if (global_settings.upscaler_mode != 1) return;
    kryoss::RenderSize(surfW, surfH, global_settings.upscaler_min_base, rw, rh);
}

static bool RenderIsScaled() {
    return s_surfW > 0 && s_surfH > 0 && FSR1_Context::g_renderWidth > 0 && FSR1_Context::g_renderHeight > 0 &&
           (FSR1_Context::g_renderWidth != s_surfW || FSR1_Context::g_renderHeight != s_surfH);
}

static bool DrawingToDefaultFramebuffer() {
    return FSR1_Context::g_renderFBO != 0 && gl_state != nullptr && gl_state->current_draw_fbo == FSR1_Context::g_renderFBO;
}

static GLint ScaleCoord(GLint v, GLsizei from, GLsizei to) { return kryoss::ScaleCoord(v, from, to); }

static void ScaleRectToRender(GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    int ix = *x, iy = *y, iw = *w, ih = *h;
    kryoss::ScaleRect(&ix, &iy, &iw, &ih, s_surfW, s_surfH, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);
    *x = ix; *y = iy; *w = iw; *h = ih;
}

void CalculateTargetResolution(FSR1_Quality_Preset preset, int renderWidth, int renderHeight, int* targetWidth,
                               int* targetHeight) {
    float scale;
    switch (preset) {
    case FSR1_Quality_Preset::UltraQuality:
        scale = 1.3f;
        break;
    case FSR1_Quality_Preset::Quality:
        scale = 1.5f;
        break;
    case FSR1_Quality_Preset::Balanced:
        scale = 1.7f;
        break;
    case FSR1_Quality_Preset::Performance:
        scale = 2.0f;
        break;
    default:
        scale = 1.5f;
        break;
    }

    scale = ApplyScaleOverride(scale);
    *targetWidth = static_cast<int>(renderWidth * scale);
    *targetHeight = static_cast<int>(renderHeight * scale);

    *targetWidth = (*targetWidth + 1) & ~1;
    *targetHeight = (*targetHeight + 1) & ~1;

    if (global_settings.upscaler_mode == 1 && s_surfW > 0 && s_surfH > 0) {
        *targetWidth = (s_surfW + 1) & ~1;
        *targetHeight = (s_surfH + 1) & ~1;
    }
    LOG_D("Render resolution: %dx%d", renderWidth, renderHeight);
    LOG_D("Target resolution: %dx%d", *targetWidth, *targetHeight);
}

void CalculateRenderResolution(FSR1_Quality_Preset preset, int targetWidth, int targetHeight, int* renderWidth,
                               int* renderHeight) {
    float scale;
    switch (preset) {
    case FSR1_Quality_Preset::UltraQuality:
        scale = 1.3f;
        break;
    case FSR1_Quality_Preset::Quality:
        scale = 1.5f;
        break;
    case FSR1_Quality_Preset::Balanced:
        scale = 1.7f;
        break;
    case FSR1_Quality_Preset::Performance:
        scale = 2.0f;
        break;
    default:
        scale = 1.5f;
    }

    scale = ApplyScaleOverride(scale);
    *renderWidth = (int)(targetWidth / scale);
    *renderHeight = (int)(targetHeight / scale);

    *renderWidth = (*renderWidth + 1) & ~1;
    *renderHeight = (*renderHeight + 1) & ~1;
}

GLuint CompileFSRShader() {
    GLuint program = glCreateProgram();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    LOG_D("Vertex shader source:\n%s", FSR_VSSource);
    glShaderSource(vs, 1, &FSR_VSSource, nullptr);
    glCompileShader(vs);

    GLint status;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, 512, nullptr, log);
        LOG_F("Vertex shader error: %s\n", log);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* fsSrc = SelectFragmentSource();
    LOG_D("Fragment shader source:\n%s", fsSrc);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, 512, nullptr, log);
        LOG_F("Fragment shader error: %s\n", log);
        return 0;
    }

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        LOG_F("Program link error: %s\n", log);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}

static void AllocRGBA8(GLsizei w, GLsizei h) {
    if (GLES.glTexStorage2D) GLES.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    else GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void InitFullscreenQuad() {
    GLStateGuard state(GUARD_VAO | GUARD_ARRAY_BUFFER);

    const float quadVertices[] = {-1.0f, -1.0f, 0.0f, 0.0f,  3.0f, -1.0f, 2.0f, 0.0f,  -1.0f, 3.0f, 0.0f, 2.0f};

    GLES.glGenVertexArrays(1, &FSR1_Context::g_quadVAO);
    GLES.glGenBuffers(1, &FSR1_Context::g_quadVBO);

    GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, FSR1_Context::g_quadVBO);

    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);

    GLES.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    GLES.glEnableVertexAttribArray(1);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

bool fsrInitialized = false;
void InitFSRResources() {
    fsrInitialized = true;

    GLStateGuard state(GUARD_PROGRAM | GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    FSR1_Context::g_fsrProgram = CompileFSRShader();

    kryoss_cs::Init(KryossTier(), global_settings.upscaler_sharpness, global_settings.upscaler_detail);

    FSR1_Context::g_inputTexLoc = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uInputTex");
    FSR1_Context::g_const0Loc = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uConst0");
    FSR1_Context::g_viewportSizeLoc = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uViewportSize");

    const GLint kryossLoc = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uKryoss");
    const GLint kryoss2Loc = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uKryoss2");

    GLES.glUseProgram(FSR1_Context::g_fsrProgram);
    GLES.glUniform1i(FSR1_Context::g_inputTexLoc, 0);
    if (global_settings.upscaler_mode == 1) {
        const kryoss::Params kp = kryoss::MakeParams(KryossTier(), global_settings.upscaler_sharpness,
                                                     global_settings.upscaler_detail);
        GLES.glUniform4fv(kryossLoc, 1, kp.u1);
        GLES.glUniform4fv(kryoss2Loc, 1, kp.u2);
        LOG_D("KryoSS v4: tier=%d taps=%d sharp=%d detail=%d", KryossTier(), kryoss::TapsFor(KryossTier()),
              global_settings.upscaler_sharpness, global_settings.upscaler_detail);
    }
    GLES.glUseProgram(0);

    InitFullscreenQuad();

    GLES.glGenTextures(1, &FSR1_Context::g_renderTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);
    AllocRGBA8(FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, FSR1_Context::g_renderWidth,
                               FSR1_Context::g_renderHeight);

    GLES.glGenFramebuffers(1, &FSR1_Context::g_renderFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, FSR1_Context::g_renderTexture, 0);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   FSR1_Context::g_depthStencilRBO);

    GLES.glGenTextures(1, &FSR1_Context::g_targetTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_targetTexture);
    kryoss_cs::AllocTarget(FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenFramebuffers(1, &FSR1_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, FSR1_Context::g_targetTexture, 0);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
}

void RecreateFSRFBO() {

    GLStateGuard state(GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    const GLuint oldRenderFBO = FSR1_Context::g_renderFBO;
    const GLuint oldTargetFBO = FSR1_Context::g_targetFBO;
    GLES.glDeleteFramebuffers(1, &FSR1_Context::g_renderFBO);
    GLES.glDeleteTextures(1, &FSR1_Context::g_renderTexture);
    GLES.glDeleteRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);

    GLES.glDeleteFramebuffers(1, &FSR1_Context::g_targetFBO);
    GLES.glDeleteTextures(1, &FSR1_Context::g_targetTexture);

    GLES.glGenTextures(1, &FSR1_Context::g_renderTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);

    AllocRGBA8(FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    GLES.glGenRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, FSR1_Context::g_renderWidth,
                               FSR1_Context::g_renderHeight);
    GLES.glGenFramebuffers(1, &FSR1_Context::g_renderFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, FSR1_Context::g_renderTexture, 0);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   FSR1_Context::g_depthStencilRBO);

    GLES.glGenTextures(1, &FSR1_Context::g_targetTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_targetTexture);
    kryoss_cs::AllocTarget(FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenFramebuffers(1, &FSR1_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, FSR1_Context::g_targetTexture, 0);

    if (oldRenderFBO != 0 && gl_state->current_draw_fbo == oldRenderFBO) {
        set_gl_state_current_draw_fbo(FSR1_Context::g_renderFBO);
    }
    state.framebuffer_recreated(oldRenderFBO, FSR1_Context::g_renderFBO);
    state.framebuffer_recreated(oldTargetFBO, FSR1_Context::g_targetFBO);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);

    LOG_D("FSR1 resources recreated: render %dx%d, target %dx%d", FSR1_Context::g_renderWidth,
          FSR1_Context::g_renderHeight, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
}

std::vector<std::pair<GLsizei, GLsizei>> g_viewportStack;

void ApplyFSR() {

    GLStateGuard state(GUARD_PROGRAM | GUARD_VAO | GUARD_TEXTURE | GUARD_FRAMEBUFFER);

    if (mali_tuning_active() && GLES.glInvalidateFramebuffer && FSR1_Context::g_renderFBO != 0) {
        static const GLenum kDepthStencil[2] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FSR1_Context::g_renderFBO);
        GLES.glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, kDepthStencil);
    }

    if (kryoss_cs::g.use) {

        GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);
        kryoss_cs::Dispatch(FSR1_Context::g_targetTexture, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
                            FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
        GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
        GLES.glViewport(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    } else {
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    GLES.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    GLES.glClear(GL_COLOR_BUFFER_BIT);

    GLES.glUseProgram(FSR1_Context::g_fsrProgram);

    GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);

    const GLfloat const0[4] = {float(FSR1_Context::g_renderWidth) / FSR1_Context::g_targetWidth,
                               float(FSR1_Context::g_renderHeight) / FSR1_Context::g_targetHeight,
                               1.0f / FSR1_Context::g_targetWidth,
                               1.0f / FSR1_Context::g_targetHeight};

    GLES.glUniform4fv(FSR1_Context::g_const0Loc, 1, const0);

    const GLfloat viewportSize[2] = {(float)FSR1_Context::g_renderWidth,
                                     (float)FSR1_Context::g_renderHeight};
    GLES.glUniform2fv(FSR1_Context::g_viewportSizeLoc, 1, viewportSize);

    GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
    GLES.glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    const GLint dstW = s_surfW > 0 ? s_surfW : FSR1_Context::g_targetWidth;
    const GLint dstH = s_surfH > 0 ? s_surfH : FSR1_Context::g_targetHeight;
    GLES.glBlitFramebuffer(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight, 0, 0, dstW, dstH,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);

    GLES.glViewport(0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);
}

void CheckResolutionChange(EGLDisplay display, EGLSurface surface) {
    GLsizei width = 0, height = 0;
    LOAD_EGL(eglQuerySurface);

    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
        display = eglGetCurrentDisplay();
        surface = eglGetCurrentSurface(EGL_DRAW);
    }

    egl_eglQuerySurface(display, surface, EGL_WIDTH, &width);
    egl_eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    OnResize(width, height);

    static unsigned s_checkCounter = 0;
    const bool applyNow = (s_checkCounter++ % 60u) == 0 || s_surfW <= 0 || s_surfH <= 0;

    if (applyNow && FSR1_Context::g_resolutionChanged) {
        FSR1_Context::g_resolutionChanged = false;
        GLsizei width = FSR1_Context::g_pendingWidth;
        GLsizei height = FSR1_Context::g_pendingHeight;

        s_surfW = width;
        s_surfH = height;
        int rw = width, rh = height;
        ComputeRenderSize(width, height, &rw, &rh);
        FSR1_Context::g_renderWidth = rw;
        FSR1_Context::g_renderHeight = rh;

        CalculateTargetResolution(global_settings.fsr1_setting, rw, rh,
                                  reinterpret_cast<int*>(&FSR1_Context::g_targetWidth),
                                  reinterpret_cast<int*>(&FSR1_Context::g_targetHeight));
        RecreateFSRFBO();
        LOG_W("KryoSS/FSR1: janela %dx%d -> render %dx%d -> saida %dx%d", static_cast<int>(width), static_cast<int>(height), rw, rh,
              static_cast<int>(FSR1_Context::g_targetWidth), static_cast<int>(FSR1_Context::g_targetHeight));
    }

}

void OnResize(int width, int height) {
    if (s_surfW == width && s_surfH == height) return;

    FSR1_Context::g_pendingWidth = width;
    FSR1_Context::g_pendingHeight = height;
    FSR1_Context::g_resolutionChanged = true;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    LOG()
    LOG_D("glViewport: x=%d, y=%d, w=%d, h=%d", x, y, w, h);

    if (w > FSR1_Context::g_pendingWidth || h > FSR1_Context::g_pendingHeight) {
        FSR1_Context::g_pendingWidth = w;
        FSR1_Context::g_pendingHeight = h;
        FSR1_Context::g_resolutionChanged = true;
    }

    if (RenderIsScaled() && DrawingToDefaultFramebuffer()) ScaleRectToRender(&x, &y, &w, &h);

    GLES.glViewport(x, y, w, h);
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    LOG()
    if (RenderIsScaled() && DrawingToDefaultFramebuffer()) ScaleRectToRender(&x, &y, &w, &h);
    GLES.glScissor(x, y, w, h);
}

bool inc_fsr1_render_scaled() { return RenderIsScaled(); }
void inc_fsr1_scale_corners(GLint* x0, GLint* y0, GLint* x1, GLint* y1) {
    if (!RenderIsScaled()) return;
    const GLsizei rw = FSR1_Context::g_renderWidth, rh = FSR1_Context::g_renderHeight;
    *x0 = ScaleCoord(*x0, s_surfW, rw);
    *x1 = ScaleCoord(*x1, s_surfW, rw);
    *y0 = ScaleCoord(*y0, s_surfH, rh);
    *y1 = ScaleCoord(*y1, s_surfH, rh);
}

namespace {

struct fsr1_ctx_state_t {
    GLuint renderFBO = 0, renderTexture = 0, depthStencilRBO = 0;
    GLuint quadVAO = 0, quadVBO = 0, fsrProgram = 0;

    GLint inputTexLoc = -1, const0Loc = -1, viewportSizeLoc = -1;
    GLuint targetFBO = 0, targetTexture = 0, currentDrawFBO = 0;
    GLsizei targetWidth = 0, targetHeight = 0, renderWidth = 0, renderHeight = 0;
    bool initialised = false;
};

std::mutex g_fsr_mutex;

ska::flat_hash_map<unsigned long long, fsr1_ctx_state_t> g_fsr_states;
fsr1_ctx_state_t g_fsr_default;
thread_local unsigned long long g_fsr_current_id = 0;

void store_into(fsr1_ctx_state_t& d) {
    d.renderFBO = FSR1_Context::g_renderFBO;
    d.renderTexture = FSR1_Context::g_renderTexture;
    d.depthStencilRBO = FSR1_Context::g_depthStencilRBO;
    d.quadVAO = FSR1_Context::g_quadVAO;
    d.quadVBO = FSR1_Context::g_quadVBO;
    d.fsrProgram = FSR1_Context::g_fsrProgram;
    d.inputTexLoc = FSR1_Context::g_inputTexLoc;
    d.const0Loc = FSR1_Context::g_const0Loc;
    d.viewportSizeLoc = FSR1_Context::g_viewportSizeLoc;
    d.targetFBO = FSR1_Context::g_targetFBO;
    d.targetTexture = FSR1_Context::g_targetTexture;
    d.currentDrawFBO = FSR1_Context::g_currentDrawFBO;
    d.targetWidth = FSR1_Context::g_targetWidth;
    d.targetHeight = FSR1_Context::g_targetHeight;
    d.renderWidth = FSR1_Context::g_renderWidth;
    d.renderHeight = FSR1_Context::g_renderHeight;
    d.initialised = fsrInitialized;
}

void load_from(const fsr1_ctx_state_t& s) {
    FSR1_Context::g_renderFBO = s.renderFBO;
    FSR1_Context::g_renderTexture = s.renderTexture;
    FSR1_Context::g_depthStencilRBO = s.depthStencilRBO;
    FSR1_Context::g_quadVAO = s.quadVAO;
    FSR1_Context::g_quadVBO = s.quadVBO;
    FSR1_Context::g_fsrProgram = s.fsrProgram;
    FSR1_Context::g_inputTexLoc = s.inputTexLoc;
    FSR1_Context::g_const0Loc = s.const0Loc;
    FSR1_Context::g_viewportSizeLoc = s.viewportSizeLoc;
    FSR1_Context::g_targetFBO = s.targetFBO;
    FSR1_Context::g_targetTexture = s.targetTexture;
    FSR1_Context::g_currentDrawFBO = s.currentDrawFBO;
    FSR1_Context::g_targetWidth = s.targetWidth;
    FSR1_Context::g_targetHeight = s.targetHeight;
    FSR1_Context::g_renderWidth = s.renderWidth;
    FSR1_Context::g_renderHeight = s.renderHeight;
    fsrInitialized = s.initialised;

}

}

void inc_fsr1_bind_context(unsigned long long ctx_id) {
    if (ctx_id == g_fsr_current_id) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);
    store_into(g_fsr_current_id == 0 ? g_fsr_default : g_fsr_states[g_fsr_current_id]);
    load_from(ctx_id == 0 ? g_fsr_default : g_fsr_states[ctx_id]);
    g_fsr_current_id = ctx_id;
}

void inc_fsr1_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);

    if (g_fsr_current_id == ctx_id) {
        load_from(g_fsr_default);
        g_fsr_current_id = 0;
    }
    g_fsr_states.erase(ctx_id);
}

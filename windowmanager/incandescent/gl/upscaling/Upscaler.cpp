

#include "Upscaler.h"
#include <mutex>
#include <cstring>
#include <ska/flat_hash_map.hpp>

#include "ShaderProvider.h"
#include "UpscalerShaderSource.h"
#include "../../config/settings.h"
#include "../../gles/loader.h"
#include "../glext.h"

#define DEBUG 0

enum GLStateBits : unsigned int {
    GUARD_PROGRAM = 1u << 0,
    GUARD_VAO = 1u << 1,
    GUARD_ARRAY_BUFFER = 1u << 2,

    GUARD_TEXTURE = 1u << 3,
    GUARD_FRAMEBUFFER = 1u << 4,
    GUARD_RENDERBUFFER = 1u << 5,
    GUARD_IMAGE = 1u << 6,
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
    GLint prevImageName = 0;
    GLint prevImageLevel = 0;
    GLint prevImageLayered = 0;
    GLint prevImageLayer = 0;
    GLint prevImageAccess = GL_READ_ONLY;
    GLint prevImageFormat = GL_RGBA8;

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
        if ((saved & GUARD_IMAGE) && GLES.glGetIntegeri_v) {
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &prevImageName);
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, 0, &prevImageLevel);
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, 0, &prevImageLayered);
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, 0, &prevImageLayer);
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, 0, &prevImageAccess);
            GLES.glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, 0, &prevImageFormat);
        }
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
        if ((saved & GUARD_IMAGE) && GLES.glBindImageTexture) {
            GLES.glBindImageTexture(0, static_cast<GLuint>(prevImageName), prevImageLevel,
                                   prevImageLayered ? GL_TRUE : GL_FALSE, prevImageLayer,
                                   static_cast<GLenum>(prevImageName ? prevImageAccess : GL_READ_ONLY),
                                   static_cast<GLenum>(prevImageName ? prevImageFormat : GL_RGBA8));
        }
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
            GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        }
    }
};

namespace Upscaling_Context {
    GLuint g_renderFBO = 0;
    GLuint g_renderTexture = 0;
    GLuint g_depthStencilRBO = 0;
    GLuint g_quadVAO = 0;
    GLuint g_quadVBO = 0;
    GLuint g_upscalerProgram = 0;
    GLuint g_computeProgram = 0;
    GLint g_computeInputSizeLoc = -1;
    GLint g_computeOutputSizeLoc = -1;
    GLint g_computeMaxGroupsX = 0;
    GLint g_computeMaxGroupsY = 0;
    bool g_computeExtentWarned = false;

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

static float ApplyScaleOverride(float scale) {
    const int pct = global_settings.upscaler_scale_pct;
    return (pct >= 50 && pct <= 99) ? 100.0f / static_cast<float>(pct) : scale;
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

GLuint CompileSelectedUpscalerShader() {
    GLuint program = glCreateProgram();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    LOG_D("Vertex shader source:\n%s", UPSCALER_VSSource);
    glShaderSource(vs, 1, &UPSCALER_VSSource, nullptr);
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
    const char* fsSrc = Upscaling::FragmentSource();
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

GLuint CompileComputeUpscalerShader() {
    const char* source = Upscaling::ComputeSource();
    const bool has_es31 = (g_gles_caps.major > 3) || (g_gles_caps.major == 3 && g_gles_caps.minor >= 1);
    if (!source || !has_es31 || !GLES.glDispatchCompute || !GLES.glBindImageTexture ||
        !GLES.glMemoryBarrier || !GLES.glGetIntegeri_v) {
        return 0;
    }

    GLint maxInvocations = 0, maxImageUnits = 0, maxComputeImages = 0, maxComputeTextures = 0;
    GLint maxX = 0, maxY = 0, maxZ = 0, maxGroupsX = 0, maxGroupsY = 0, maxGroupsZ = 0;
    GLint maxSharedBytes = 0;
    GLES.glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxInvocations);
    GLES.glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
    GLES.glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImages);
    GLES.glGetIntegerv(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS, &maxComputeTextures);
    GLES.glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &maxSharedBytes);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &maxX);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &maxY);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &maxZ);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxGroupsX);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &maxGroupsY);
    GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &maxGroupsZ);

    if (maxInvocations < 64 || maxImageUnits < 1 || maxComputeImages < 1 || maxComputeTextures < 1 ||
        maxX < 8 || maxY < 8 || maxZ < 1 || maxGroupsX < 1 || maxGroupsY < 1 || maxGroupsZ < 1 ||
        maxSharedBytes < 576) {
        LOG_W_FORCE("KryoSS compute unavailable: invocations=%d imageUnits=%d computeImages=%d "
                    "computeTextures=%d workgroup=%dx%dx%d groups=%dx%dx%d shared=%d",
                    maxInvocations, maxImageUnits, maxComputeImages, maxComputeTextures, maxX, maxY, maxZ,
                    maxGroupsX, maxGroupsY, maxGroupsZ, maxSharedBytes);
        return 0;
    }
    Upscaling_Context::g_computeMaxGroupsX = maxGroupsX;
    Upscaling_Context::g_computeMaxGroupsY = maxGroupsY;

    GLuint program = GLES.glCreateProgram();
    GLuint shader = GLES.glCreateShader(GL_COMPUTE_SHADER);
    if (!program || !shader) {
        if (shader) GLES.glDeleteShader(shader);
        if (program) GLES.glDeleteProgram(program);
        return 0;
    }
    const char* strings[] = {source};
    const GLint lengths[] = {static_cast<GLint>(strlen(source))};

    glShaderSource(shader, 1, strings, lengths);
    GLES.glCompileShader(shader);
    GLint status = GL_FALSE;
    GLES.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048] = {};
        GLES.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_W_FORCE("KryoSS compute compile failed; using fragment fallback: %s", log);
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }

    GLES.glAttachShader(program, shader);
    GLES.glLinkProgram(program);
    GLES.glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048] = {};
        GLES.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOG_W_FORCE("KryoSS compute link failed; using fragment fallback: %s", log);
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }
    GLES.glDeleteShader(shader);

    GLES.glUseProgram(program);
    Upscaling::BindComputeUniforms(program);
    GLES.glUseProgram(0);
    LOG_I("KryoSS v5 compute active: 8x8 workgroups, shared 12x12 tile");
    return program;
}

void InitFullscreenQuad() {
    GLStateGuard state(GUARD_VAO | GUARD_ARRAY_BUFFER);
    const float quadVertices[] = {-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,

                                  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f};

    GLES.glGenVertexArrays(1, &Upscaling_Context::g_quadVAO);
    GLES.glGenBuffers(1, &Upscaling_Context::g_quadVBO);

    GLES.glBindVertexArray(Upscaling_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, Upscaling_Context::g_quadVBO);

    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);

    GLES.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    GLES.glEnableVertexAttribArray(1);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

bool upscalerInitialized = false;
void InitUpscalerResources() {
    upscalerInitialized = true;

    GLStateGuard state(GUARD_PROGRAM | GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    Upscaling_Context::g_upscalerProgram = CompileSelectedUpscalerShader();

    Upscaling_Context::g_inputTexLoc = glGetUniformLocation(Upscaling_Context::g_upscalerProgram, "uInputTex");
    Upscaling_Context::g_const0Loc = glGetUniformLocation(Upscaling_Context::g_upscalerProgram, "uConst0");
    Upscaling_Context::g_viewportSizeLoc = glGetUniformLocation(Upscaling_Context::g_upscalerProgram, "uViewportSize");

    GLES.glUseProgram(Upscaling_Context::g_upscalerProgram);
    GLES.glUniform1i(Upscaling_Context::g_inputTexLoc, 0);

    Upscaling::BindProgramUniforms(Upscaling_Context::g_upscalerProgram);
    GLES.glUseProgram(0);

    Upscaling_Context::g_computeProgram = CompileComputeUpscalerShader();
    if (Upscaling_Context::g_computeProgram) {
        Upscaling_Context::g_computeInputSizeLoc =
            GLES.glGetUniformLocation(Upscaling_Context::g_computeProgram, "uInputSize");
        Upscaling_Context::g_computeOutputSizeLoc =
            GLES.glGetUniformLocation(Upscaling_Context::g_computeProgram, "uOutputSize");
    }

    InitFullscreenQuad();

    GLES.glGenTextures(1, &Upscaling_Context::g_renderTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, Upscaling_Context::g_renderTexture);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Upscaling_Context::g_renderWidth, Upscaling_Context::g_renderHeight, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenRenderbuffers(1, &Upscaling_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, Upscaling_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, Upscaling_Context::g_renderWidth,
                               Upscaling_Context::g_renderHeight);

    GLES.glGenFramebuffers(1, &Upscaling_Context::g_renderFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_renderFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Upscaling_Context::g_renderTexture, 0);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   Upscaling_Context::g_depthStencilRBO);

    GLES.glGenTextures(1, &Upscaling_Context::g_targetTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, Upscaling_Context::g_targetTexture);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenFramebuffers(1, &Upscaling_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_targetFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Upscaling_Context::g_targetTexture, 0);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_renderFBO);
}

void RecreateUpscalerFBO() {

    GLStateGuard state(GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    const GLuint oldRenderFBO = Upscaling_Context::g_renderFBO;
    const GLuint oldTargetFBO = Upscaling_Context::g_targetFBO;
    GLES.glDeleteFramebuffers(1, &Upscaling_Context::g_renderFBO);
    GLES.glDeleteTextures(1, &Upscaling_Context::g_renderTexture);
    GLES.glDeleteRenderbuffers(1, &Upscaling_Context::g_depthStencilRBO);

    GLES.glDeleteFramebuffers(1, &Upscaling_Context::g_targetFBO);
    GLES.glDeleteTextures(1, &Upscaling_Context::g_targetTexture);

    GLES.glGenTextures(1, &Upscaling_Context::g_renderTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, Upscaling_Context::g_renderTexture);

    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Upscaling_Context::g_renderWidth, Upscaling_Context::g_renderHeight, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    GLES.glGenRenderbuffers(1, &Upscaling_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, Upscaling_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, Upscaling_Context::g_renderWidth,
                               Upscaling_Context::g_renderHeight);
    GLES.glGenFramebuffers(1, &Upscaling_Context::g_renderFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_renderFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Upscaling_Context::g_renderTexture, 0);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   Upscaling_Context::g_depthStencilRBO);

    GLES.glGenTextures(1, &Upscaling_Context::g_targetTexture);
    GLES.glBindTexture(GL_TEXTURE_2D, Upscaling_Context::g_targetTexture);
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLES.glGenFramebuffers(1, &Upscaling_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_targetFBO);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Upscaling_Context::g_targetTexture, 0);

    if (oldRenderFBO != 0 && gl_state->current_draw_fbo == oldRenderFBO) {
        set_gl_state_current_draw_fbo(Upscaling_Context::g_renderFBO);
    }
    state.framebuffer_recreated(oldRenderFBO, Upscaling_Context::g_renderFBO);
    state.framebuffer_recreated(oldTargetFBO, Upscaling_Context::g_targetFBO);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_renderFBO);
    GLES.glViewport(0, 0, Upscaling_Context::g_renderWidth, Upscaling_Context::g_renderHeight);

    LOG_D("upscaler resources recreated: render %dx%d, target %dx%d", Upscaling_Context::g_renderWidth,
          Upscaling_Context::g_renderHeight, Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight);
}

std::vector<std::pair<GLsizei, GLsizei>> g_viewportStack;

void ApplyUpscaler() {
    const GLuint requiredGroupsX = static_cast<GLuint>((Upscaling_Context::g_targetWidth + 7) / 8);
    const GLuint requiredGroupsY = static_cast<GLuint>((Upscaling_Context::g_targetHeight + 7) / 8);
    const bool computeExtentOk = requiredGroupsX <= static_cast<GLuint>(Upscaling_Context::g_computeMaxGroupsX) &&
                                 requiredGroupsY <= static_cast<GLuint>(Upscaling_Context::g_computeMaxGroupsY);
    const bool useCompute = Upscaling_Context::g_computeProgram != 0 && computeExtentOk;
    if (Upscaling_Context::g_computeProgram != 0 && !computeExtentOk &&
        !Upscaling_Context::g_computeExtentWarned) {
        LOG_W_FORCE("KryoSS compute extent exceeds device limits (%u x %u groups); using fragment fallback",
                    requiredGroupsX, requiredGroupsY);
        Upscaling_Context::g_computeExtentWarned = true;
    }
    GLStateGuard state(GUARD_PROGRAM | GUARD_VAO | GUARD_TEXTURE | GUARD_FRAMEBUFFER |
                       (useCompute ? GUARD_IMAGE : 0));

    GLES.glBindTexture(GL_TEXTURE_2D, Upscaling_Context::g_renderTexture);

    if (useCompute) {

        GLES.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GLES.glUseProgram(Upscaling_Context::g_computeProgram);
        GLES.glUniform2i(Upscaling_Context::g_computeInputSizeLoc,
                         Upscaling_Context::g_renderWidth, Upscaling_Context::g_renderHeight);
        GLES.glUniform2i(Upscaling_Context::g_computeOutputSizeLoc,
                         Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight);
        GLES.glBindImageTexture(0, Upscaling_Context::g_targetTexture, 0, GL_FALSE, 0,
                                GL_WRITE_ONLY, GL_RGBA8);
        const GLuint groupsX = static_cast<GLuint>((Upscaling_Context::g_targetWidth + 7) / 8);
        const GLuint groupsY = static_cast<GLuint>((Upscaling_Context::g_targetHeight + 7) / 8);
        GLES.glDispatchCompute(groupsX, groupsY, 1);
        GLES.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT |
                             GL_TEXTURE_FETCH_BARRIER_BIT);
    } else {

        GLES.glBindFramebuffer(GL_FRAMEBUFFER, Upscaling_Context::g_targetFBO);
        GLES.glViewport(0, 0, Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight);
        GLES.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        GLES.glClear(GL_COLOR_BUFFER_BIT);
        GLES.glUseProgram(Upscaling_Context::g_upscalerProgram);

        const GLfloat const0[4] = {
            float(Upscaling_Context::g_renderWidth) / Upscaling_Context::g_targetWidth,
            float(Upscaling_Context::g_renderHeight) / Upscaling_Context::g_targetHeight,
            1.0f / Upscaling_Context::g_targetWidth,
            1.0f / Upscaling_Context::g_targetHeight};
        GLES.glUniform4fv(Upscaling_Context::g_const0Loc, 1, const0);
        const GLfloat viewportSize[2] = {static_cast<float>(Upscaling_Context::g_renderWidth),
                                         static_cast<float>(Upscaling_Context::g_renderHeight)};
        GLES.glUniform2fv(Upscaling_Context::g_viewportSizeLoc, 1, viewportSize);
        GLES.glBindVertexArray(Upscaling_Context::g_quadVAO);
        GLES.glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, Upscaling_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLES.glBlitFramebuffer(0, 0, Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight, 0, 0,
                           Upscaling_Context::g_targetWidth, Upscaling_Context::g_targetHeight,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
    GLES.glViewport(0, 0, Upscaling_Context::g_renderWidth, Upscaling_Context::g_renderHeight);
}

void CheckUpscalerResolutionChange(EGLDisplay display, EGLSurface surface) {
    GLsizei width = 0, height = 0;
    LOAD_EGL(eglQuerySurface);

    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
        display = eglGetCurrentDisplay();
        surface = eglGetCurrentSurface(EGL_DRAW);
    }

    egl_eglQuerySurface(display, surface, EGL_WIDTH, &width);
    egl_eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    OnUpscalerResize(width, height);

    if (Upscaling_Context::g_resolutionChanged) {
        Upscaling_Context::g_resolutionChanged = false;
        GLsizei width = Upscaling_Context::g_pendingWidth;
        GLsizei height = Upscaling_Context::g_pendingHeight;
        Upscaling_Context::g_renderWidth = width;
        Upscaling_Context::g_renderHeight = height;

        CalculateTargetResolution(global_settings.fsr1_setting, width, height,
                                  reinterpret_cast<int*>(&Upscaling_Context::g_targetWidth),
                                  reinterpret_cast<int*>(&Upscaling_Context::g_targetHeight));
        RecreateUpscalerFBO();
    }

}

void OnUpscalerResize(int width, int height) {
    if (Upscaling_Context::g_renderWidth == width && Upscaling_Context::g_renderHeight == height) return;

    Upscaling_Context::g_pendingWidth = width;
    Upscaling_Context::g_pendingHeight = height;
    Upscaling_Context::g_resolutionChanged = true;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    LOG()
    LOG_D("glViewport: x=%d, y=%d, w=%d, h=%d", x, y, w, h);

    if (w > Upscaling_Context::g_pendingWidth || h > Upscaling_Context::g_pendingHeight) {
        Upscaling_Context::g_pendingWidth = w;
        Upscaling_Context::g_pendingHeight = h;
        Upscaling_Context::g_resolutionChanged = true;
    }

    GLES.glViewport(x, y, w, h);
}

namespace {

struct upscaler_ctx_state_t {
    GLuint renderFBO = 0, renderTexture = 0, depthStencilRBO = 0;
    GLuint quadVAO = 0, quadVBO = 0, upscalerProgram = 0, computeProgram = 0;
    GLint computeInputSizeLoc = -1, computeOutputSizeLoc = -1;
    GLint computeMaxGroupsX = 0, computeMaxGroupsY = 0;
    bool computeExtentWarned = false;

    GLint inputTexLoc = -1, const0Loc = -1, viewportSizeLoc = -1;
    GLuint targetFBO = 0, targetTexture = 0, currentDrawFBO = 0;
    GLsizei targetWidth = 0, targetHeight = 0, renderWidth = 0, renderHeight = 0;
    bool initialised = false;
};

std::mutex g_upscaler_mutex;

ska::flat_hash_map<unsigned long long, upscaler_ctx_state_t> g_upscaler_states;
upscaler_ctx_state_t g_upscaler_default;
thread_local unsigned long long g_upscaler_current_id = 0;

void store_into(upscaler_ctx_state_t& d) {
    d.renderFBO = Upscaling_Context::g_renderFBO;
    d.renderTexture = Upscaling_Context::g_renderTexture;
    d.depthStencilRBO = Upscaling_Context::g_depthStencilRBO;
    d.quadVAO = Upscaling_Context::g_quadVAO;
    d.quadVBO = Upscaling_Context::g_quadVBO;
    d.upscalerProgram = Upscaling_Context::g_upscalerProgram;
    d.computeProgram = Upscaling_Context::g_computeProgram;
    d.computeInputSizeLoc = Upscaling_Context::g_computeInputSizeLoc;
    d.computeOutputSizeLoc = Upscaling_Context::g_computeOutputSizeLoc;
    d.computeMaxGroupsX = Upscaling_Context::g_computeMaxGroupsX;
    d.computeMaxGroupsY = Upscaling_Context::g_computeMaxGroupsY;
    d.computeExtentWarned = Upscaling_Context::g_computeExtentWarned;
    d.inputTexLoc = Upscaling_Context::g_inputTexLoc;
    d.const0Loc = Upscaling_Context::g_const0Loc;
    d.viewportSizeLoc = Upscaling_Context::g_viewportSizeLoc;
    d.targetFBO = Upscaling_Context::g_targetFBO;
    d.targetTexture = Upscaling_Context::g_targetTexture;
    d.currentDrawFBO = Upscaling_Context::g_currentDrawFBO;
    d.targetWidth = Upscaling_Context::g_targetWidth;
    d.targetHeight = Upscaling_Context::g_targetHeight;
    d.renderWidth = Upscaling_Context::g_renderWidth;
    d.renderHeight = Upscaling_Context::g_renderHeight;
    d.initialised = upscalerInitialized;
}

void load_from(const upscaler_ctx_state_t& s) {
    Upscaling_Context::g_renderFBO = s.renderFBO;
    Upscaling_Context::g_renderTexture = s.renderTexture;
    Upscaling_Context::g_depthStencilRBO = s.depthStencilRBO;
    Upscaling_Context::g_quadVAO = s.quadVAO;
    Upscaling_Context::g_quadVBO = s.quadVBO;
    Upscaling_Context::g_upscalerProgram = s.upscalerProgram;
    Upscaling_Context::g_computeProgram = s.computeProgram;
    Upscaling_Context::g_computeInputSizeLoc = s.computeInputSizeLoc;
    Upscaling_Context::g_computeOutputSizeLoc = s.computeOutputSizeLoc;
    Upscaling_Context::g_computeMaxGroupsX = s.computeMaxGroupsX;
    Upscaling_Context::g_computeMaxGroupsY = s.computeMaxGroupsY;
    Upscaling_Context::g_computeExtentWarned = s.computeExtentWarned;
    Upscaling_Context::g_inputTexLoc = s.inputTexLoc;
    Upscaling_Context::g_const0Loc = s.const0Loc;
    Upscaling_Context::g_viewportSizeLoc = s.viewportSizeLoc;
    Upscaling_Context::g_targetFBO = s.targetFBO;
    Upscaling_Context::g_targetTexture = s.targetTexture;
    Upscaling_Context::g_currentDrawFBO = s.currentDrawFBO;
    Upscaling_Context::g_targetWidth = s.targetWidth;
    Upscaling_Context::g_targetHeight = s.targetHeight;
    Upscaling_Context::g_renderWidth = s.renderWidth;
    Upscaling_Context::g_renderHeight = s.renderHeight;
    upscalerInitialized = s.initialised;

}

}

void inc_upscaling_bind_context(unsigned long long ctx_id) {
    if (ctx_id == g_upscaler_current_id) return;
    std::lock_guard<std::mutex> lock(g_upscaler_mutex);
    store_into(g_upscaler_current_id == 0 ? g_upscaler_default : g_upscaler_states[g_upscaler_current_id]);
    load_from(ctx_id == 0 ? g_upscaler_default : g_upscaler_states[ctx_id]);
    g_upscaler_current_id = ctx_id;
}

void inc_upscaling_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_upscaler_mutex);

    if (g_upscaler_current_id == ctx_id) {
        load_from(g_upscaler_default);
        g_upscaler_current_id = 0;
    }
    g_upscaler_states.erase(ctx_id);
}

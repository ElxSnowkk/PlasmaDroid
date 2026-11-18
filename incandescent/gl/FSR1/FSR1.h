

#pragma once

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef __APPLE__
#include <malloc.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../../gles/gles.h"
#include "../../gles/loader.h"
#include "../../includes.h"
#include "../framebuffer.h"
#include "../glsl/glsl_for_es.h"
#include "../log.h"
#include "../inc.h"
#include <GL/gl.h>

namespace FSR1_Context {
    extern GLuint g_renderFBO;
    extern GLuint g_renderTexture;
    extern GLuint g_depthStencilRBO;
    extern GLuint g_quadVAO;
    extern GLuint g_quadVBO;
    extern GLuint g_fsrProgram;

    extern GLint g_inputTexLoc;
    extern GLint g_const0Loc;
    extern GLint g_viewportSizeLoc;

    extern GLuint g_targetFBO;
    extern GLuint g_targetTexture;

    extern GLuint g_currentDrawFBO;
    extern GLint g_viewport[4];
    extern GLsizei g_targetWidth;
    extern GLsizei g_targetHeight;
    extern GLsizei g_renderWidth;
    extern GLsizei g_renderHeight;
    extern bool g_dirty;

    extern bool g_resolutionChanged;
    extern GLsizei g_pendingWidth;
    extern GLsizei g_pendingHeight;
}

extern bool fsrInitialized;

void inc_fsr1_bind_context(unsigned long long ctx_id);
void ApplyFSR();
void InitFSRResources();
void CheckResolutionChange(EGLDisplay display, EGLSurface surface);
void OnResize(int width, int height);

bool inc_fsr1_render_scaled();
void inc_fsr1_scale_corners(GLint* x0, GLint* y0, GLint* x1, GLint* y1);

extern "C"
{
    GLAPI void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
    GLAPI void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
}

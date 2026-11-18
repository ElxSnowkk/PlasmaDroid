

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

namespace Upscaling_Context {
    extern GLuint g_renderFBO;
    extern GLuint g_renderTexture;
    extern GLuint g_depthStencilRBO;
    extern GLuint g_quadVAO;
    extern GLuint g_quadVBO;
    extern GLuint g_upscalerProgram;

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

extern bool upscalerInitialized;

void inc_upscaling_bind_context(unsigned long long ctx_id);
void ApplyUpscaler();
void InitUpscalerResources();
void CheckUpscalerResolutionChange(EGLDisplay display, EGLSurface surface);
void OnUpscalerResize(int width, int height);

extern "C"
{
    GLAPI void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
}

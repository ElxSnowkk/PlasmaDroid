// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/ExtWrappers/MultiBindWrapper.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "MultiBindWrapper.h"
#include <cassert>
#include "../texture.h"

#define DEBUG 0

void glBindTextures(GLuint first, GLsizei count, const GLuint* textures) {

    const GLenum prevUnit = GL_TEXTURE0 + static_cast<GLuint>(inc_driver_active_texture_unit());
    for (GLsizei i = 0; i < count; ++i) {

        GLuint tex = textures ? textures[i] : 0u;
        if (tex == 0) {
            glActiveTexture(GL_TEXTURE0 + first + i);
            glBindTexture(GL_TEXTURE_2D, 0);
            continue;
        }

        TextureObject* texObject = mgGetTexObjectByID(tex);
        if (texObject == nullptr) {
            continue;
        }
        glActiveTexture(GL_TEXTURE0 + first + i);
        glBindTexture(ConvertTextureTargetToGLEnum(texObject->target), tex);
    }
    glActiveTexture(prevUnit);
}

void glBindSamplers(GLuint first, GLsizei count, const GLuint* samplers) {
    for (GLsizei i = 0; i < count; ++i) {
        glBindSampler(first + i, samplers[i]);
    }
}

void glBindImageTextures(GLuint first, GLsizei count, const GLuint* textures) {
    for (int i = 0; i < count; i++) {

        TextureObject* texObject =
            (textures != nullptr && textures[i] != 0) ? mgGetTexObjectByID(textures[i]) : nullptr;
        if (texObject == nullptr) {
            glBindImageTexture(first + i, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
        } else {
            glBindImageTexture(first + i, textures[i], 0, GL_TRUE, 0, GL_READ_WRITE, texObject->internal_format);
        }
    }
}

void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                         const GLsizei* strides) {
    for (GLsizei i = 0; i < count; ++i) {
        glBindVertexBuffer(first + i, buffers[i], offsets[i], strides[i]);
    }
}

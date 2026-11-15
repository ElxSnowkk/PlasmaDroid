// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/getter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header




// See inc_set_gl_error in gl/inc.h for why this exists and why it is per thread.















// ---- Incandescent: nome do renderer extraído da GPU real e relatório (ver INCANDESCENT-MODIFICATIONS.md) ----
// Substitui {GPU} no modelo pelo nome devolvido pelo driver GLES real (consultado por este módulo). Sem nome: "Unknown GPU" (não esconde).

// Lê uma string do driver GLES real SEM construir std::string a partir de NULL.
// glGetString devolve NULL quando não há contexto corrente (ou o símbolo não resolveu); std::string(NULL) é UB e, no
// libc++ hardened do NDK, vira `brk` (SIGTRAP) dentro do construtor da biblioteca.

// Grava GPU=... e GL_RENDERER_RAW=... em INC_GPU_REPORT (o app mostra isto como prova de qual GPU o Incandescent enxergou).

#include "getter.h"
#include "enable.h"
#include "../egl/context.h"
#include "buffer.h"
#include "texture.h"
#include <string>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <vector>
#include <random>
#include "FSR1/FSR1.h"
#include "log.h"
#include "inc.h"
#include "pixel.h"
#include "random_string_gen.h"
#include "../config/settings.h"

#define DEBUG 0

Version GLVersion;

namespace {

thread_local GLenum g_frontend_error = GL_NO_ERROR;
}

void inc_set_gl_error(GLenum error) {
    if (error == GL_NO_ERROR) return;

    if (g_frontend_error != GL_NO_ERROR) return;
    g_frontend_error = error;
    LOG_D("Incandescent raised %s", glEnumToString(error))
}

void glGetIntegerv(GLenum pname, GLint* params) {
    LOG()
    LOG_D("glGetIntegerv, pname: %s", glEnumToString(pname))
    switch (pname) {
    case GL_NUM_EXTENSIONS + GL_BACKEND_GETTER_MG:
        GLES.glGetIntegerv(pname - GL_BACKEND_GETTER_MG, params);
        return;
    case GL_CONTEXT_PROFILE_MASK:
        (*params) = GL_CONTEXT_CORE_PROFILE_BIT;
        break;
    case GL_NUM_EXTENSIONS:
        static GLint num_extensions = -1;
        if (num_extensions == -1) {
            const GLubyte* ext_str = glGetString(GL_EXTENSIONS);
            if (ext_str) {
                std::string copy_str((const char*)ext_str);
                std::string token;
                size_t pos = 0;
                num_extensions = 0;
                while ((pos = copy_str.find(' ')) != std::string::npos) {
                    num_extensions++;
                    copy_str.erase(0, pos + 1);
                }
                if (!copy_str.empty()) num_extensions++;
            } else {
                num_extensions = 0;
            }
        }
        (*params) = num_extensions;
        break;
    case GL_MAJOR_VERSION:

        if (g_current_ctx && g_current_ctx->client_type == EGL_OPENGL_API) {
            (*params) = g_current_ctx->granted_major;
        } else if (g_current_ctx) {
            GLES.glGetIntegerv(GL_MAJOR_VERSION, params);
        } else {
            (*params) = GLVersion.Major;
        }
        break;
    case GL_MINOR_VERSION:
        if (g_current_ctx && g_current_ctx->client_type == EGL_OPENGL_API) {
            (*params) = g_current_ctx->granted_minor;
        } else if (g_current_ctx) {
            GLES.glGetIntegerv(GL_MINOR_VERSION, params);
        } else {
            (*params) = GLVersion.Minor;
        }
        break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: {
        int es_params = 16;
        GLES.glGetIntegerv(pname, &es_params);
        CHECK_GL_ERROR(*params) = es_params * 2;

        break;
    }
    case 0x84E2: {

        GLint units = 8;
        GLES.glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        (*params) = units < 8 ? units : 8;
        break;
    }
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: {

        int es_params = 32;
        GLES.glGetIntegerv(pname, &es_params);
        CHECK_GL_ERROR
        const int tracked = inc_max_texture_units();
        (*params) = es_params < tracked ? es_params : tracked;
        break;
    }
    case GL_CONTEXT_FLAGS: {

        (*params) = g_current_ctx ? g_current_ctx->context_flags : 0;
        break;
    }
    case GL_ARRAY_BUFFER_BINDING:
    case GL_ATOMIC_COUNTER_BUFFER_BINDING:
    case GL_COPY_READ_BUFFER_BINDING:
    case GL_COPY_WRITE_BUFFER_BINDING:
    case GL_DRAW_INDIRECT_BUFFER_BINDING:
    case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
    case GL_PIXEL_PACK_BUFFER_BINDING:
    case GL_PIXEL_UNPACK_BUFFER_BINDING:
    case GL_SHADER_STORAGE_BUFFER_BINDING:
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
    case GL_UNIFORM_BUFFER_BINDING:
        (*params) = (int)find_bound_buffer(pname);
        LOG_D("  -> %d", *params)
        break;
    case GL_VERTEX_ARRAY_BINDING:
        (*params) = (int)find_bound_array();
        break;

    case GL_DRAW_FRAMEBUFFER_BINDING: {
        GLES.glGetIntegerv(pname, params);

        if (FSR1_Context::g_renderFBO != 0 && *params == (GLint)FSR1_Context::g_renderFBO) *params = 0;
        LOG_D("  -> %d", *params)
        break;
    }
    default: {

        GLboolean enabled = GL_FALSE;
        GLint ival = 0;
        if (inc_enable_query(pname, &enabled)) {
            (*params) = enabled ? 1 : 0;
            break;
        }
        if (inc_enable_query_int(pname, &ival)) {
            (*params) = ival;
            break;
        }

        if (inc_pixel_store_query_int(pname, params)) {
            LOG_D("  -> %d", *params)
            break;
        }
        GLES.glGetIntegerv(pname, params);
        LOG_D("  -> %d", *params)
        CHECK_GL_ERROR
    }
    }
}

GLenum glGetError() {
    LOG()

    const GLenum backend = GLES.glGetError();
    const GLenum frontend = g_frontend_error;
    g_frontend_error = GL_NO_ERROR;

    const GLenum swallowed = frontend != GL_NO_ERROR ? frontend : backend;
    if (swallowed != GL_NO_ERROR) {
        LOG_W("glGetError -> %s, reported to the application as GL_NO_ERROR", glEnumToString(swallowed))
    }
    return GL_NO_ERROR;
}

static std::string es_ext;
std::string GetExtensionsList() {
    return es_ext;
}

void InitGLESBaseExtensions() {
    std::vector<std::string> extensions;

    if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
        extensions.push_back("GL_INC_incandescent");
        extensions.push_back("GL_INC_backend_string_getter_access");
        extensions.push_back("GL_INC_settings_string_dump");
    }

    const char* base_exts[] = {"GL_ARB_fragment_program",
                               "GL_ARB_vertex_buffer_object",
                               "GL_ARB_vertex_array_object",
                               "GL_ARB_vertex_buffer",
                               "GL_EXT_vertex_array",
                               "GL_ARB_ES2_compatibility",
                               "GL_ARB_ES3_compatibility",
                               "GL_EXT_packed_depth_stencil",
                               "GL_EXT_depth_texture",
                               "GL_ARB_depth_texture",
                               "GL_ARB_shading_language_100",
                               "GL_ARB_imaging",
                               "GL_ARB_draw_buffers_blend",
                               "OpenGL15",
                               "GL_ARB_shader_storage_buffer_object",
                               "GL_ARB_shader_image_load_store",
                               "GL_ARB_clear_texture",
                               "GL_ARB_get_program_binary",
                               "GL_ARB_separate_shader_objects",
                               "GL_ARB_multi_bind",
                               "GL_KHR_no_error"};

    extensions.insert(extensions.end(), std::begin(base_exts), std::end(base_exts));

    if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) {
        for (int i = extensions.size() - 1; i > 0; --i) {
            int j = rand() % (i + 1);
            std::swap(extensions[i], extensions[j]);
        }
    }

    es_ext.clear();
    for (const auto& ext : extensions) {
        es_ext += ext;
        es_ext += " ";
    }
}

void AppendExtension(const char* ext) {
    es_ext += ext;
    es_ext += ' ';
}

std::string getBeforeThirdSpace(const std::string& str) {
    int spaceCount = 0;
    size_t endPos = 0;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == ' ') {
            spaceCount++;
            if (spaceCount == 3) {
                endPos = i;
                break;
            }
        }
        if (spaceCount < 3) endPos = str.length();
    }

    return str.substr(0, endPos);
}

static std::string incandescentApplyTemplate(std::string t, const std::string& gpu) {
    const std::string ph = "{GPU}";
    const std::string name = (gpu.empty() || gpu == "<unknown>") ? std::string("Unknown GPU") : gpu;
    size_t pos = 0;
    while ((pos = t.find(ph, pos)) != std::string::npos) {
        t.replace(pos, ph.size(), name);
        pos += name.size();
    }
    return t;
}

static std::string gles_string(GLenum name) {
    if (!GLES.glGetString) return std::string();
    const GLubyte* s = GLES.glGetString(name);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
}

static void incandescentWriteGpuReport(const std::string& gpuFull, const std::string& gpuShort, const char* raw) {
    const char* path = getenv("INC_GPU_REPORT");
    if (!path || !*path) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "GPU=%s\nGPU_FULL=%s\nGL_RENDERER_RAW=%s\n", gpuShort.c_str(), gpuFull.c_str(), raw ? raw : "");
    fclose(f);
}

std::string getGpuName() {
    std::string gpuName = gles_string(GL_RENDERER);

    if (gpuName.empty()) {
        return "<unknown>";
    }

    if (gpuName.find("MetalANGLE, ANGLE") != std::string::npos) {
        if (gpuName.length() < 25) {
            return gpuName;
        }

        std::string gpu = gpuName.substr(23, gpuName.length() - 24);
        std::string formattedGpuName = gpu + " | MetalANGLE | Metal";
        return formattedGpuName;
    }

    if (gpuName.rfind("ANGLE", 0) == 0 && gpuName.find("Vulkan") != std::string::npos) {
        size_t firstParen = gpuName.find('(');
        size_t secondParen = gpuName.find('(', firstParen + 1);
        size_t lastParen = gpuName.rfind('(');

        std::string gpu = gpuName.substr(secondParen + 1, lastParen - secondParen - 2);

        size_t vulkanStart = gpuName.find("Vulkan ");
        size_t vulkanEnd = gpuName.find(' ', vulkanStart + 7);
        std::string vulkanVersion = gpuName.substr(vulkanStart + 7, vulkanEnd - (vulkanStart + 7));

        std::string formattedGpuName = gpu + " | ANGLE | Vulkan " + vulkanVersion;

        return formattedGpuName;
    }

    return gpuName;
}

void set_es_version() {
    const std::string rawVersion = gles_string(GL_VERSION);
    if (rawVersion.empty())
        LOG_I("glGetString(GL_VERSION) devolveu NULL (sem contexto GLES corrente?); assumindo OpenGL ES 3.0")
    std::string ESVersionStr = getBeforeThirdSpace(rawVersion);
    int major = 0, minor = 0;

    if (sscanf(ESVersionStr.c_str(), "OpenGL ES %d.%d", &major, &minor) == 2) {
        hardware->es_version = major * 100 + minor * 10;
    } else {
        hardware->es_version = 300;
    }
    LOG_I("OpenGL ES Version: %s (%d)", ESVersionStr.c_str(), hardware->es_version)
    if (hardware->es_version < 300) {
        LOG_I("OpenGL ES version is lower than 3.0! This version is not supported!")
    }
}

std::string getGLESName() {
    return getBeforeThirdSpace(gles_string(GL_VERSION));
}

static std::string rendererString;
static std::string vendorString;
static std::string versionString;
const GLubyte* glGetString(GLenum name) {
    LOG()
    LOG_D("glGetString, %s", glEnumToString(name))
    switch (name) {
    case GL_VENDOR: {
        if (vendorString.empty()) {
            if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
                std::string vendor = "PlasmaGL";
                vendorString = vendor;
            } else {
                const char choices[] = "AINM";
                vendorString = choices[rand() % 4];

                RandomStringOptions randStrOpts;
                randStrOpts.includeDigits = false;
                randStrOpts.minLength = 3;
                randStrOpts.maxLength = 8;
                randStrOpts.includeLowercase = false;
                randStrOpts.includeUppercase = false;
                randStrOpts.customChars = "IMenaNtMseAVlD";
                vendorString += GenerateRandomString(randStrOpts);
            }
        }
        return (const GLubyte*)vendorString.c_str();
    }
    case GL_VERSION: {
        if (versionString.empty()) {
            versionString = GLVersion.toString();
            if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {

                if (GLVersion.toInt(2) == DEFAULT_GL_VERSION) {
                    versionString += " Incandescent " INCANDESCENT_VERSION "";
                } else {
                    Version defaultVersion = Version(DEFAULT_GL_VERSION);
                    versionString += " §4§l(" + defaultVersion.toString() + ") Incandescent " INCANDESCENT_VERSION "§r";
                }
            } else {
                const char choices[] = "AIN";
                versionString += " ";
                versionString += choices[rand() % 3];

                RandomStringOptions randStrOpts;
                randStrOpts.includeDigits = false;
                randStrOpts.customChars = " ";
                versionString += GenerateRandomString(randStrOpts);

                RandomStringOptions randStrOpts2;
                randStrOpts2.includeDigits = false;
                randStrOpts2.includeUppercase = false;
                randStrOpts2.minLength = 1;
                randStrOpts2.maxLength = 4;

                versionString += std::to_string(MAJOR) + GenerateRandomString(randStrOpts2) + std::to_string(MINOR) +
                                 GenerateRandomString(randStrOpts2) + std::to_string(REVISION) +
                                 GenerateRandomString(randStrOpts2) + std::to_string(PATCH) +
                                 GenerateRandomString(randStrOpts2);
            }
        }
        return (const GLubyte*)versionString.c_str();
    }
    case GL_RENDERER: {
        if (rendererString == std::string("")) {

            const char* incandescentTemplate = getenv("INC_RENDERER_TEMPLATE");
            const char* incandescentRenderer = getenv("INC_RENDERER_STRING");
            if (incandescentTemplate && *incandescentTemplate) {

                const std::string gpuFull = getGpuName();
                const std::string gpuShort = gpuFull.substr(0, gpuFull.find(" | "));
                const std::string rawRenderer = gles_string(GL_RENDERER);
                incandescentWriteGpuReport(gpuFull, gpuShort, rawRenderer.c_str());
                rendererString = incandescentApplyTemplate(incandescentTemplate, gpuShort);
                LOG_I("Incandescent: GPU reportada pelo Incandescent = %s", gpuShort.c_str())
            } else if (incandescentRenderer && *incandescentRenderer) {
                rendererString = incandescentRenderer;
            } else if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
                std::string gpuName = getGpuName();
                std::string glesName = getGLESName();
                rendererString = std::string(gpuName) + " | " + std::string(glesName);
            } else {
                const char choices[] = "AINM";
                rendererString = choices[rand() % 4];

                RandomStringOptions randStrOpts;
                randStrOpts.includeDigits = true;
                randStrOpts.minLength = 6;
                randStrOpts.maxLength = 12;
                randStrOpts.includeLowercase = false;
                randStrOpts.includeUppercase = false;
                randStrOpts.customChars = "IRMenaNtfsoerAceVlDG";
                rendererString += GenerateRandomString(randStrOpts);

                int junkInfoTime = rand() % 3 + 1;
                for (int i = 0; i < junkInfoTime; ++i) {
                    rendererString += " ";
                    RandomStringOptions randStrOpts2;
                    randStrOpts2.minLength = 3;
                    randStrOpts2.maxLength = 6;
                    randStrOpts2.includeLowercase = false;
                    randStrOpts2.includeUppercase = false;
                    randStrOpts2.customChars = "IRenaNtfsoerAcieVDcsG";
                    rendererString += GenerateRandomString(randStrOpts2);
                }
            }
        }
        return (const GLubyte*)rendererString.c_str();
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        static std::string shadingLangString;

        if (shadingLangString.empty()) {
            std::string baseVer;
            if (hardware->es_version < 310) {
                baseVer = "4.00";
            } else {
                baseVer = "4.60";
            }

            if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) {
                shadingLangString = baseVer;

                int junkCount = rand() % 2 + 1;
                for (int i = 0; i < junkCount; ++i) {
                    shadingLangString += " ";
                    RandomStringOptions junkOpts;
                    junkOpts.minLength = 2;
                    junkOpts.maxLength = 5;
                    junkOpts.includeLowercase = false;
                    junkOpts.includeUppercase = false;
                    junkOpts.customChars = "IAneNDtVsaMIl";
                    shadingLangString += GenerateRandomString(junkOpts);
                }
            } else {
                shadingLangString = baseVer + " Incandescent (Incandescent base) with glslang and SPIRV-Cross";
            }
        }

        return reinterpret_cast<const GLubyte*>(shadingLangString.c_str());
    }
    case GL_EXTENSIONS: {

        static std::string extensionsString;

        if (extensionsString.empty()) {
            extensionsString = GetExtensionsList();
        }

        return (const GLubyte*)extensionsString.c_str();
    }
    case GL_SETTINGS_MG: {
        if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) return GLES.glGetString(name);

        static char* settings_string = nullptr;
        std::string tmp = dump_settings_string("  ");
        settings_string = strdup(tmp.c_str());
        return reinterpret_cast<const GLubyte*>(settings_string);
    }
    case GL_VERSION + GL_BACKEND_GETTER_MG:
    case GL_VENDOR + GL_BACKEND_GETTER_MG:
    case GL_RENDERER + GL_BACKEND_GETTER_MG:
    case GL_EXTENSIONS + GL_BACKEND_GETTER_MG:
    case GL_SHADING_LANGUAGE_VERSION + GL_BACKEND_GETTER_MG:
        if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled)
            return GLES.glGetString(name - GL_BACKEND_GETTER_MG);
        else
            return GLES.glGetString(name);
    default:
        return GLES.glGetString(name);
    }
}

const GLubyte* glGetStringi(GLenum name, GLuint index) {
    LOG()
    if (name == GL_EXTENSIONS + GL_BACKEND_GETTER_MG && global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
        return GLES.glGetStringi(name - GL_BACKEND_GETTER_MG, index);
    }

    typedef struct {
        GLenum name;
        const char** parts;
        GLuint count;
    } StringCache;
    static StringCache caches[] = {{GL_EXTENSIONS, nullptr, 0},
                                   {GL_VENDOR, nullptr, 0},
                                   {GL_VERSION, nullptr, 0},
                                   {GL_SHADING_LANGUAGE_VERSION, nullptr, 0}};
    static int initialized = 0;
    if (!initialized) {
        for (auto& cache : caches) {
            GLenum target = cache.name;
            const GLubyte* str = nullptr;
            const char* delimiter = " ";

            switch (target) {
            case GL_VENDOR:
                str = glGetString(GL_VENDOR);
                delimiter = ", ";
                break;
            case GL_VERSION:
                str = glGetString(GL_VERSION);
                delimiter = " .";
                break;
            case GL_SHADING_LANGUAGE_VERSION:
                str = glGetString(GL_SHADING_LANGUAGE_VERSION);
                break;
            case GL_EXTENSIONS:
                str = glGetString(GL_EXTENSIONS);
                break;
            default:
                return GLES.glGetStringi(name, index);
            }

            if (!str) continue;

            std::string copy_str((const char*)str);
            std::string token_str;
            size_t start = 0;
            size_t end = copy_str.find_first_of(delimiter);

            while (end != std::string::npos) {
                token_str = copy_str.substr(start, end - start);
                cache.parts = (const char**)realloc(cache.parts, (cache.count + 1) * sizeof(char*));
                cache.parts[cache.count++] = strdup(token_str.c_str());
                start = end + 1;
                end = copy_str.find_first_of(delimiter, start);
            }
            token_str = copy_str.substr(start);
            cache.parts = (const char**)realloc(cache.parts, (cache.count + 1) * sizeof(char*));
            cache.parts[cache.count++] = strdup(token_str.c_str());
        }
        initialized = 1;
    }

    for (auto& cache : caches) {
        if (cache.name == name) {
            if (index >= cache.count) {
                return nullptr;
            }
            return (const GLubyte*)cache.parts[index];
        }
    }

    return nullptr;
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    LOG()
    if (GLES.glGetQueryObjectivEXT) {
        GLES.glGetQueryObjectivEXT(id, pname, params);
        CHECK_GL_ERROR
    }
}

void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params) {
    LOG()
    if (GLES.glGetQueryObjecti64vEXT) {
        GLES.glGetQueryObjecti64vEXT(id, pname, params);
        CHECK_GL_ERROR
    }
}



#ifndef INCANDESCENT_INCLUDES_H
#define INCANDESCENT_INCLUDES_H

#define RENDERERNAME "Incandescent"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <dlfcn.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <INC/extensions.h>

#include "egl/egl.h"
#include "egl/loader.h"

#if PROFILING
#include <perfetto.h>
PERFETTO_DEFINE_CATEGORIES(perfetto::Category("glcalls").SetDescription("Calls from OpenGL"),
                           perfetto::Category("internal").SetDescription("Internal calls"));
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    static int g_initialized = 0;

    void proc_init();

#ifdef __cplusplus
}
#endif

#include <ska/flat_hash_map.hpp>

template <typename Key, typename T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>,
          class Allocator = std::allocator<std::pair<Key, T>>>
using UnorderedMap = ska::flat_hash_map<Key, T, Hash, KeyEqual, Allocator>;

#endif

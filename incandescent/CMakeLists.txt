# Incandescent (KryLabs), LGPL-2.1; baseado em trabalho (c) 2025-2026 MobileGL-Dev (ver NOTICE.md).
# Alterações (todas só de build, nenhuma no código do Incandescent além do patch KryoSS listado em INCANDESCENT-MODIFICATIONS.md):
#  - o alvo se chama sempre "incandescent" (libincandescent.so; o original usava ${CMAKE_PROJECT_NAME}, que dentro de um subprojeto aponta para o app);
#  - include/ resolvido relativo a este diretório (o original usava ${CMAKE_SOURCE_DIR});
#  - ThinLTO desligado por padrão (opção INC_ENABLE_LTO): no Termux o link com LTO pode estourar a memória;
#  - glslang/SPIRV-Cross com EXCLUDE_FROM_ALL e testes/instalação desligados (só o que o Incandescent linka é compilado).
cmake_minimum_required(VERSION 3.22.1)
project("incandescent")
set(INC_TARGET incandescent)
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(ENABLE_RTTI                ON  CACHE BOOL "Enables RTTI (will be took by glslang)" FORCE)
set(ENABLE_HLSL                OFF CACHE BOOL "Disable HLSL input for glslang" FORCE)
set(BUILD_EXTERNAL             OFF CACHE BOOL "Disable build external dependencies for glslang" FORCE)
set(ENABLE_GLSLANG_BINARIES    OFF CACHE BOOL "Disable glslangValidator/spirv-remap" FORCE)
set(ENABLE_OPT                 OFF CACHE BOOL "Disable SPIRV-Tools opt usage in glslang" FORCE)
set(BUILD_EXTERNAL             OFF CACHE BOOL "Don't build external deps in External/" FORCE)

set(GLSLANG_TESTS              OFF CACHE BOOL "Incandescent: sem testes" FORCE)
set(ENABLE_CTEST               OFF CACHE BOOL "Incandescent: sem testes" FORCE)
set(SKIP_GLSLANG_INSTALL       ON  CACHE BOOL "Incandescent: sem install" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS   OFF CACHE BOOL "Incandescent: sem testes" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL   ON  CACHE BOOL "Incandescent: sem install" FORCE)

set(SPIRV_CROSS_ENABLE_C_API   ON  CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL    ON  CACHE BOOL "Enable GLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "Disable reflect" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL    OFF CACHE BOOL "Disable HLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_MSL     OFF CACHE BOOL "Disable MSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_CPP     OFF CACHE BOOL "Disable C++ API target" FORCE)
set(SPIRV_CROSS_CLI            OFF CACHE BOOL "Disable CLI binary" FORCE)
set(SPIRV_CROSS_STATIC         ON  CACHE BOOL "Prefer static libs" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL    OFF CACHE BOOL "Disable util" FORCE)

option(IG_OPTIMIZE_DEPS "Compilar glslang/SPIRV-Cross com -O2 mesmo em Debug" ON)
if (IG_OPTIMIZE_DEPS)
    foreach (_cfg DEBUG RELWITHDEBINFO)
        set(CMAKE_C_FLAGS_${_cfg} "-O2 -g0 -DNDEBUG")
        set(CMAKE_CXX_FLAGS_${_cfg} "-O2 -g0 -DNDEBUG")
    endforeach ()
endif ()

add_subdirectory(3rdparty/glslang EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/SPIRV-Cross EXCLUDE_FROM_ALL)

if(MACOS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -fvisibility=default -funwind-tables -g -D_THREAD_SAFE -fPIC -stdlib=libc++")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -g -std=gnu99 -funwind-tables -fvisibility=hidden")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -fvisibility=hidden -funwind-tables -g -D_THREAD_SAFE -fPIC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -g -std=gnu99 -funwind-tables -fvisibility=hidden")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
    set(CMAKE_ANDROID_STL_TYPE c++_static)
endif()

option(INC_ENABLE_LTO "Compilar o Incandescent com ThinLTO (usa muita RAM no link; desligado no Incandescent)" OFF)
if (INC_ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckIPOSupported)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)

    check_ipo_supported(RESULT LTOSupported OUTPUT LTOError)

    check_c_compiler_flag("-flto" HAS_LTO_C)
    check_cxx_compiler_flag("-flto" HAS_LTO_CXX)

    if (LTOSupported OR (HAS_LTO_C AND HAS_LTO_CXX))
    check_c_compiler_flag("-flto=thin" HAS_THINLTO_C)
    check_cxx_compiler_flag("-flto=thin" HAS_THINLTO_CXX)
    if (HAS_THINLTO_C AND HAS_THINLTO_CXX)
        message(STATUS "ThinLTO supported, using -flto=thin")

        add_compile_options(-flto=thin)
        add_link_options(-flto=thin)
    else()
        message(STATUS "ThinLTO not available, fallback to CMAKE IPO")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    else()
    message(STATUS "IPO not supported: ${LTOError}")
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MATCHES "AppleClang")
    add_compile_options(-O3 -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
else ()
    add_compile_options(-O2)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/Zc:__cplusplus)
endif()

set(PROFILING OFF)

add_library(${INC_TARGET} SHARED
    main.cpp
    gl/gl_stub.cpp
    gl/gl_native.cpp
    gl/gl.cpp
    gl/envvars.cpp
    gl/log.cpp
    gl/program.cpp
    gl/shader.cpp
    gl/framebuffer.cpp
    gl/texture.cpp
    gl/drawing.cpp
    gl/multidraw.cpp
    bench/multidraw_bench.cpp
    gl/inc.cpp
    gl/buffer.cpp
    gl/getter.cpp
    gl/enable.cpp
    gl/mali_tuning.cpp
    gl/perf_stats.cpp
    gl/restart.cpp
    egl/context.cpp
    gl/pixel.cpp
    gl/transfer.cpp
    gl/random_string_gen.cpp
    gl/ExtWrappers/DSAWrapper.cpp
    gl/ExtWrappers/MultiBindWrapper.cpp
    gl/glsl/glsl_for_es.cpp
    gl/glsl/cache.cpp
    gl/FSR1/FSR1.cpp

    gl/vertexattrib.cpp
    glx/lookup.cpp
    egl/egl.cpp
    egl/loader.cpp
    gles/loader.cpp
    gles/got_patch.cpp
    gles/vk_intercept.cpp
    config/cJSON.c
    config/config.cpp
    config/gpu_utils.cpp
    config/settings.cpp
    config/stats.cpp
    init.cpp
)

target_include_directories(${INC_TARGET} PRIVATE
    ./includes
    ./3rdparty/xxhash)
target_include_directories(${INC_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(${INC_TARGET} PRIVATE
    glslang::glslang
    spirv-cross-c
)

if (NOT MSVC)
    target_link_options(${INC_TARGET} PRIVATE -Wl,-Bsymbolic-functions)
endif()

if(MACOS)
    target_link_libraries(${INC_TARGET} PUBLIC
        "-F${CMAKE_SOURCE_DIR}/external"
        "-framework libEGL"
        "-framework libGLESv2"
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
    )
elseif(ANDROID)
    target_link_libraries(${INC_TARGET} PUBLIC
        android
        log
    )
    else()
    target_link_libraries(${INC_TARGET} PUBLIC
        ${CMAKE_DL_LIBS}
    )
endif()

if (PROFILING)
    find_package(Threads)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/perfetto/sdk)
    add_library(perfetto STATIC ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/perfetto/sdk/perfetto.cc)
    target_link_libraries(${INC_TARGET} perfetto ${CMAKE_THREAD_LIBS_INIT})
    target_compile_definitions(${INC_TARGET} PUBLIC PROFILING=1)
endif()

# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

cmake_minimum_required(VERSION 3.22.1)
project(plasmarender LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PLASMA_HAS_VULKAN_COMPOSITOR 0)
set(PLASMA_SHADER_HINTS)
foreach(root "$ENV{ANDROID_SDK_ROOT}" "$ENV{ANDROID_HOME}" "$ENV{ANDROID_NDK_HOME}" "$ENV{ANDROID_NDK_ROOT}" "${ANDROID_NDK}" "${CMAKE_ANDROID_NDK}")
    if(root)
        list(APPEND PLASMA_SHADER_HINTS
            "${root}/shader-tools/linux-x86_64"
            "${root}/shader-tools/darwin-x86_64"
            "${root}/shader-tools/windows-x86_64"
            "${root}/shader-tools/linux-aarch64"
            "${root}/toolchains/llvm/prebuilt/linux-x86_64/bin"
            "${root}/toolchains/llvm/prebuilt/linux-aarch64/bin"
            "${root}/toolchains/llvm/prebuilt/darwin-x86_64/bin"
        )
    endif()
endforeach()
find_program(PLASMA_GLSLC NAMES glslc HINTS ${PLASMA_SHADER_HINTS})
find_program(PLASMA_GLSLANG NAMES glslangValidator HINTS ${PLASMA_SHADER_HINTS})

set(PLASMA_VULKAN_SOURCES)
if(PLASMA_GLSLC OR PLASMA_GLSLANG)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(PLASMA_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirv")
        set(PLASMA_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan_shaders.hpp")
        file(MAKE_DIRECTORY "${PLASMA_SHADER_DIR}")
        set(PLASMA_SHADER_OUTPUTS)
        foreach(shader fullscreen.vert wallpaper.frag present.frag glass.vert glass.frag)
            get_filename_component(stage "${shader}" LAST_EXT)
            string(SUBSTRING "${stage}" 1 -1 stage)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
            set(out "${PLASMA_SHADER_DIR}/${shader}.spv")
            if(PLASMA_GLSLC)
                add_custom_command(OUTPUT "${out}"
                    COMMAND "${PLASMA_GLSLC}" --target-env=vulkan1.1 "${src}" -o "${out}"
                    DEPENDS "${src}" VERBATIM)
            else()
                add_custom_command(OUTPUT "${out}"
                    COMMAND "${PLASMA_GLSLANG}" -V --target-env vulkan1.1 -S "${stage}" "${src}" -o "${out}"
                    DEPENDS "${src}" VERBATIM)
            endif()
            list(APPEND PLASMA_SHADER_OUTPUTS "${out}")
        endforeach()
        add_custom_command(OUTPUT "${PLASMA_SHADER_HEADER}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py" "${PLASMA_SHADER_HEADER}"
                "kFullscreenVert=${PLASMA_SHADER_DIR}/fullscreen.vert.spv"
                "kWallpaperFrag=${PLASMA_SHADER_DIR}/wallpaper.frag.spv"
                "kPresentFrag=${PLASMA_SHADER_DIR}/present.frag.spv"
                "kGlassVert=${PLASMA_SHADER_DIR}/glass.vert.spv"
                "kGlassFrag=${PLASMA_SHADER_DIR}/glass.frag.spv"
            DEPENDS ${PLASMA_SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py"
            VERBATIM)
        add_custom_target(plasma_vulkan_shaders DEPENDS "${PLASMA_SHADER_HEADER}")
        set(PLASMA_VULKAN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_compositor.cpp" "${PLASMA_SHADER_HEADER}")
        set(PLASMA_HAS_VULKAN_COMPOSITOR 1)
    else()
        message(WARNING "Python 3 não encontrado: compositor Vulkan desabilitado; GLES continua disponível")
    endif()
else()
    message(WARNING "glslc/glslangValidator não encontrado: compositor Vulkan desabilitado; GLES continua disponível")
endif()

add_library(plasmarender SHARED probe.cpp ${PLASMA_VULKAN_SOURCES})
target_compile_definitions(plasmarender PRIVATE PLASMA_HAS_VULKAN_COMPOSITOR=${PLASMA_HAS_VULKAN_COMPOSITOR})
if(PLASMA_HAS_VULKAN_COMPOSITOR)
    add_dependencies(plasmarender plasma_vulkan_shaders)
    target_include_directories(plasmarender PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()
find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)
target_link_libraries(plasmarender EGL GLESv3 ${vulkan-lib} ${android-lib} ${log-lib})

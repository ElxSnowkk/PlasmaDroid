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
project(piperella_compositor)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(WAYLAND_PREBUILT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/prebuilts/${ANDROID_ABI})
set(WAYLAND_PREBUILT_LIBDIR ${WAYLAND_PREBUILT_DIR}/lib)
set(WAYLAND_PREBUILT_INCDIR ${WAYLAND_PREBUILT_DIR}/include)

if(NOT EXISTS ${WAYLAND_PREBUILT_LIBDIR}/libwayland-server.so)
    message(FATAL_ERROR
        "libwayland-server.so not found at ${WAYLAND_PREBUILT_LIBDIR}.\n"
        "Run scripts/build-libwayland-android.sh on the host before building the APK.")
endif()

if(NOT EXISTS ${WAYLAND_PREBUILT_LIBDIR}/libxkbcommon.so)
    message(FATAL_ERROR
        "libxkbcommon.so not found at ${WAYLAND_PREBUILT_LIBDIR}.")
endif()

add_library(wayland-server SHARED IMPORTED)
set_target_properties(wayland-server PROPERTIES
    IMPORTED_LOCATION ${WAYLAND_PREBUILT_LIBDIR}/libwayland-server.so)
target_include_directories(wayland-server INTERFACE ${WAYLAND_PREBUILT_INCDIR})

add_library(xkbcommon SHARED IMPORTED)
set_target_properties(xkbcommon PROPERTIES
    IMPORTED_LOCATION ${WAYLAND_PREBUILT_LIBDIR}/libxkbcommon.so)
target_include_directories(xkbcommon INTERFACE ${WAYLAND_PREBUILT_INCDIR})

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)

set(PROTOCOL_SOURCES)
    foreach(proto
        xdg-shell
        linux-dmabuf
        presentation-time
        pointer-constraints
        relative-pointer
        viewporter
        xdg-output
        xdg-decoration
        text-input
        fractional-scale
        xdg-activation
        linux-drm-syncobj-v1
        gamepad
        tearing-control
        idle-inhibit
        keyboard-shortcuts-inhibit
        cursor-shape
        single-pixel-buffer
        ext-idle-notify
        content-type
        pointer-gestures
        alpha-modifier
        xdg-system-bell
        xdg-dialog
        xdg-foreign)
    set(_c ${PROTOCOL_DIR}/${proto}-protocol.c)
    if(EXISTS ${_c})
        list(APPEND PROTOCOL_SOURCES ${_c})
    endif()
endforeach()

option(PLASMA_WAYLAND_REQUIRE_VULKAN "Fail the Android build instead of silently shipping a GLES-only Wayland backend" ON)
set(PLASMA_WAYLAND_HAS_VULKAN 0)
set(PLASMA_WAYLAND_SHADER_HINTS)
foreach(root "${PLASMA_ANDROID_SDK_ROOT}" "$ENV{ANDROID_SDK_ROOT}" "$ENV{ANDROID_HOME}" "$ENV{ANDROID_NDK_HOME}" "$ENV{ANDROID_NDK_ROOT}" "${ANDROID_NDK}" "${CMAKE_ANDROID_NDK}")
    if(root)
        list(APPEND PLASMA_WAYLAND_SHADER_HINTS
            "${root}/shader-tools/linux-aarch64"
            "${root}/shader-tools/linux-x86_64"
            "${root}/shader-tools/darwin-x86_64"
            "${root}/shader-tools/windows-x86_64"
            "${root}/toolchains/llvm/prebuilt/linux-aarch64/bin"
            "${root}/toolchains/llvm/prebuilt/linux-x86_64/bin"
            "${root}/toolchains/llvm/prebuilt/darwin-x86_64/bin")
    endif()
endforeach()
find_program(PLASMA_WAYLAND_GLSLC NAMES glslc HINTS ${PLASMA_WAYLAND_SHADER_HINTS})
find_program(PLASMA_WAYLAND_GLSLANG NAMES glslangValidator HINTS ${PLASMA_WAYLAND_SHADER_HINTS})
if(PLASMA_WAYLAND_GLSLC OR PLASMA_WAYLAND_GLSLANG)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(PLASMA_WAYLAND_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirv")
        set(PLASMA_WAYLAND_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland_vulkan_shaders.hpp")
        file(MAKE_DIRECTORY "${PLASMA_WAYLAND_SHADER_DIR}")
        set(_wayland_vert "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wayland_quad.vert")
        set(_wayland_frag "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wayland_quad.frag")
        set(_wayland_vert_spv "${PLASMA_WAYLAND_SHADER_DIR}/wayland_quad.vert.spv")
        set(_wayland_frag_spv "${PLASMA_WAYLAND_SHADER_DIR}/wayland_quad.frag.spv")
        if(PLASMA_WAYLAND_GLSLC)
            add_custom_command(OUTPUT "${_wayland_vert_spv}"
                COMMAND "${PLASMA_WAYLAND_GLSLC}" --target-env=vulkan1.1 "${_wayland_vert}" -o "${_wayland_vert_spv}"
                DEPENDS "${_wayland_vert}" VERBATIM)
            add_custom_command(OUTPUT "${_wayland_frag_spv}"
                COMMAND "${PLASMA_WAYLAND_GLSLC}" --target-env=vulkan1.1 "${_wayland_frag}" -o "${_wayland_frag_spv}"
                DEPENDS "${_wayland_frag}" VERBATIM)
        else()
            add_custom_command(OUTPUT "${_wayland_vert_spv}"
                COMMAND "${PLASMA_WAYLAND_GLSLANG}" -V --target-env vulkan1.1 -S vert "${_wayland_vert}" -o "${_wayland_vert_spv}"
                DEPENDS "${_wayland_vert}" VERBATIM)
            add_custom_command(OUTPUT "${_wayland_frag_spv}"
                COMMAND "${PLASMA_WAYLAND_GLSLANG}" -V --target-env vulkan1.1 -S frag "${_wayland_frag}" -o "${_wayland_frag_spv}"
                DEPENDS "${_wayland_frag}" VERBATIM)
        endif()
        add_custom_command(OUTPUT "${PLASMA_WAYLAND_SHADER_HEADER}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../render/src/main/cpp/embed_spirv.py"
                "${PLASMA_WAYLAND_SHADER_HEADER}"
                "kWaylandQuadVert=${_wayland_vert_spv}"
                "kWaylandQuadFrag=${_wayland_frag_spv}"
            DEPENDS "${_wayland_vert_spv}" "${_wayland_frag_spv}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../../../render/src/main/cpp/embed_spirv.py"
            VERBATIM)
        add_custom_target(plasma_wayland_vulkan_shaders DEPENDS "${PLASMA_WAYLAND_SHADER_HEADER}")
        set(PLASMA_WAYLAND_HAS_VULKAN 1)
    else()
        if(PLASMA_WAYLAND_REQUIRE_VULKAN)
            message(FATAL_ERROR "Python 3 is required to embed the generated Wayland SPIR-V shaders")
        else()
            message(WARNING "Python 3 not found: Wayland Vulkan renderer disabled; GLES fallback remains active")
        endif()
    endif()
else()
    if(PLASMA_WAYLAND_REQUIRE_VULKAN)
        message(FATAL_ERROR
            "Wayland Vulkan backend is required but glslc/glslangValidator was not found. "
            "Install Android SDK component 'shader-tools' or install glslang in Termux, "
            "then make sure glslangValidator/glslc is in PATH. Set "
            "-DPLASMA_WAYLAND_REQUIRE_VULKAN=OFF only for an intentional GLES-only build.")
    else()
        message(WARNING "glslc/glslangValidator not found: Wayland Vulkan renderer disabled; GLES fallback remains active")
    endif()
endif()

add_library(piperella_compositor SHARED
    labwc.cpp
    plasma_wayland_bridge.cpp
    egl_renderer.cpp
    vulkan_wayland_renderer.cpp
    jni_bridge.cpp
    surface.cpp
    xdg_shell.cpp
    seat.cpp
    scene.cpp
    presentation.cpp
    ahwb_pool.cpp
    dmabuf.cpp
    data_device.cpp
    viewporter.cpp
    text_input.cpp
    fractional_scale.cpp
    activation.cpp
    syncobj.cpp
    gamepad.cpp
    tearing_control.cpp
    idle_inhibit.cpp
    keyboard_shortcuts_inhibit.cpp
    cursor_shape.cpp
    single_pixel_buffer.cpp
    ext_idle_notify.cpp
    content_type.cpp
    pointer_gestures.cpp
    alpha_modifier.cpp
    xdg_system_bell.cpp
    xdg_dialog.cpp
    xdg_foreign.cpp
    audio_engine.cpp
    notification_bridge.cpp
    adpf.cpp
    atrace.cpp
    ${PROTOCOL_SOURCES}
    protocols/cursor-shape-tablet-stub.c)

target_include_directories(piperella_compositor PRIVATE
    ${PROTOCOL_DIR}
    ${WAYLAND_PREBUILT_INCDIR})
if(PLASMA_WAYLAND_HAS_VULKAN)
    add_dependencies(piperella_compositor plasma_wayland_vulkan_shaders)
    target_include_directories(piperella_compositor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()
target_compile_definitions(piperella_compositor PRIVATE
    PLASMA_WAYLAND_HAS_VULKAN=${PLASMA_WAYLAND_HAS_VULKAN})

target_compile_options(piperella_compositor PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

find_library(PLASMA_WAYLAND_VULKAN_LIB vulkan)
if(PLASMA_WAYLAND_HAS_VULKAN AND NOT PLASMA_WAYLAND_VULKAN_LIB)
    message(FATAL_ERROR "Wayland Vulkan is enabled but Android NDK libvulkan.so was not found")
endif()

target_link_libraries(piperella_compositor PRIVATE
    android log nativewindow
    EGL GLESv3
    wayland-server
    xkbcommon
    aaudio)
if(PLASMA_WAYLAND_HAS_VULKAN)
    target_link_libraries(piperella_compositor PRIVATE ${PLASMA_WAYLAND_VULKAN_LIB})
endif()

set_target_properties(piperella_compositor PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden)

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "PlasmaDroid Wayland server currently supports arm64-v8a only (got ${ANDROID_ABI})")
endif()

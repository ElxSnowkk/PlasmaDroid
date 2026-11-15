cmake_minimum_required(VERSION 3.22.1)
project(piperella_compositor)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# C15-08: Removed explicit add_link_options("-Wl,-z,max-page-size=16384") here.
# The Gradle externalNativeBuild passes -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON which
# causes the NDK toolchain to apply -Wl,-z,max-page-size=16384 (and additionally sets
# section alignment and bionic page-size macros). Using both would be redundant;
# the Gradle flag is the canonical NDK mechanism for flexible-page-size support.
# C15-A1: ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON controls more than just the linker flag —
# it also controls __BIONIC_DEPRECATED_PAGE_SIZE_MACRO and section alignment. The
# add_link_options approach was only a subset of the required changes.

# ---------------------------------------------------------------------
# libwayland-server, cross-built for Android NDK
# ---------------------------------------------------------------------
# Built once via meson cross-file (see scripts/build-libwayland-android.sh)
# and shipped as a prebuilt .so in cpp/prebuilts/<abi>/.
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

# C15-01: Use IMPORTED targets instead of bare link_directories().
# IMPORTED targets carry their location and include dirs as usage requirements,
# which are transitive and compatible with target_link_libraries(PRIVATE/PUBLIC).
add_library(wayland-server SHARED IMPORTED)
set_target_properties(wayland-server PROPERTIES
    IMPORTED_LOCATION ${WAYLAND_PREBUILT_LIBDIR}/libwayland-server.so)
target_include_directories(wayland-server INTERFACE ${WAYLAND_PREBUILT_INCDIR})

add_library(xkbcommon SHARED IMPORTED)
set_target_properties(xkbcommon PROPERTIES
    IMPORTED_LOCATION ${WAYLAND_PREBUILT_LIBDIR}/libxkbcommon.so)
target_include_directories(xkbcommon INTERFACE ${WAYLAND_PREBUILT_INCDIR})

# C15-09: Removed PREBUILT_WAYLAND and PREBUILT_XKBCOMMON variables — unused after
# switching to IMPORTED targets above.
# C15-10: Removed duplicate WAYLAND_PREBUILT_INCDIR assignment (was set twice to
# the same value; the first assignment above is canonical).

# ---------------------------------------------------------------------
# Wayland protocol XML -> C headers (wayland-scanner runs on host)
# ---------------------------------------------------------------------
# scripts/gen-wayland-protocols.sh emits to cpp/protocols/.
# Headers + glue are committed to git.
set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)

# C15-11: Note — two orphaned protocol files exist in protocols/ but are NOT in this list:
#   - linux-drm-syncobj-protocol.c: unversioned; superseded by linux-drm-syncobj-v1-protocol.c
#   - xdg-activation-v1-protocol.c: v1-named; the non-v1 xdg-activation-protocol.c is used
# These files are not compiled and can be removed in a future cleanup.
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

# ---------------------------------------------------------------------
# Optional Vulkan client-scene backend (one Android WSI swapchain per toplevel)
# ---------------------------------------------------------------------
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

# ---------------------------------------------------------------------
# PlasmaDroid integration bridge (headless start, embedded Surface, input)
# ---------------------------------------------------------------------
# The compositor is started headless from the terminal session so Linux
# clients can connect before the first output window is created. The UI then
# attaches an Android Surface when the first xdg_toplevel appears.

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

# C15-05: Added PRIVATE scope keyword — link deps are implementation details not
# exposed in any public header, so PRIVATE is correct.
# C15-01: wayland-server and xkbcommon are now IMPORTED targets (defined above).
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

# C15-02: Added CXX_VISIBILITY_PRESET hidden so all C++ symbols default to
# hidden visibility. Only JNIEXPORT-annotated symbols and explicitly-exported
# symbols will appear in the .so's .dynsym, reducing binary size and preventing
# symbol clashes with other .so libs loaded in the same process.
set_target_properties(piperella_compositor PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden)

# ARM64 only: PlasmaDroid's PRoot/HAL integration and Mali target are AArch64.
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "PlasmaDroid Wayland server currently supports arm64-v8a only (got ${ANDROID_ABI})")
endif()

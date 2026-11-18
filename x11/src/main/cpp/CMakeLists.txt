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
project(plasma_x11 C)

set(CMAKE_C_STANDARD 11)

add_library(plasma_x11 SHARED
    src/xs_core.c
    src/xs_requests.c
    src/xs_present.c
    jni/xs_jni.c
)
target_include_directories(plasma_x11 PRIVATE include src)
target_compile_options(plasma_x11 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

find_library(log-lib log)
find_library(android-lib android)
target_link_libraries(plasma_x11 PRIVATE ${log-lib} ${android-lib})

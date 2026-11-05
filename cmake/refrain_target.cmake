# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

# refrain_add_library(<target> [SOURCES...])
#
# Adds a first-party STATIC library that lives under src/, exports the public
# include/ tree, and inherits the project-wide warning policy.
function(refrain_add_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC
                               $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    refrain_apply_warnings(${target})
    refrain_apply_debug_options(${target})
endfunction()

# refrain_add_executable(<target> [SOURCES...]) - same conventions as above.
function(refrain_add_executable target)
    add_executable(${target} ${ARGN})
    refrain_apply_warnings(${target})
    refrain_apply_debug_options(${target})
endfunction()

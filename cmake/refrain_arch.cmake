# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Architecture level handling for code generation. The cache option is a
# portable level name; this module maps it to the toolchain spelling of the
# target architecture and rejects levels that belong to another architecture.
#
#   baseline     portable instruction set (default)
#   native       the build machine (rejected for cross builds)
#   x86-64-v2    x86-64 psABI level 2: SSE4.2, POPCNT
#   x86-64-v3    x86-64 psABI level 3: AVX2, BMI2, FMA
#   x86-64-v4    x86-64 psABI level 4: AVX-512
#   armv8-a      AArch64 baseline
#   armv8.2-a    AArch64 with the 8.2 extensions
#   armv9-a      AArch64 with the 9.0 extensions

set(REFRAIN_ARCH_LEVELS
    baseline native x86-64-v2 x86-64-v3 x86-64-v4 armv8-a armv8.2-a armv9-a)

# Apply `level` to every target defined after this call in the current
# directory. Fails closed: an unknown level, a level for another architecture
# or a toolchain without a mapping stops the configure instead of silently
# building for the baseline.
function(refrain_apply_arch_level level)
    if(level STREQUAL "baseline")
        return()
    endif()
    if(NOT level IN_LIST REFRAIN_ARCH_LEVELS)
        message(FATAL_ERROR
                "REFRAIN_ARCH_LEVEL='${level}' is not one of: ${REFRAIN_ARCH_LEVELS}")
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    if(arch MATCHES "^(x86_64|amd64)$")
        # x86-64 paths only.
        if(level STREQUAL "native")
            if(CMAKE_CROSSCOMPILING)
                message(FATAL_ERROR
                        "REFRAIN_ARCH_LEVEL=native cannot be used for a cross build; name the level instead")
            endif()
            add_compile_options(-march=native)
            return()
        endif()
        if(NOT level MATCHES "^x86-64-v[234]$")
            message(FATAL_ERROR
                    "REFRAIN_ARCH_LEVEL='${level}' is an AArch64 level; the target is ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(MSVC)
            if(level STREQUAL "x86-64-v2")
                return() # SSE2 is the MSVC x64 baseline.
            elseif(level STREQUAL "x86-64-v3")
                add_compile_options(/arch:AVX2)
                return()
            endif()
            message(FATAL_ERROR
                    "REFRAIN_ARCH_LEVEL='${level}' has no MSVC switch; use GCC or Clang for AVX-512")
        endif()
        add_compile_options("-march=${level}")
        return()
    endif()

    if(arch MATCHES "^(aarch64|arm64)$")
        # AArch64 paths only.
        if(level STREQUAL "native")
            if(CMAKE_CROSSCOMPILING)
                message(FATAL_ERROR
                        "REFRAIN_ARCH_LEVEL=native cannot be used for a cross build; name the level instead")
            endif()
            add_compile_options(-mcpu=native)
            return()
        endif()
        if(NOT level MATCHES "^armv(8-a|8\\.2-a|9-a)$")
            message(FATAL_ERROR
                    "REFRAIN_ARCH_LEVEL='${level}' is an x86-64 level; the target is ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(MSVC)
            message(FATAL_ERROR
                    "REFRAIN_ARCH_LEVEL is not wired for MSVC ARM64; use GCC or Clang")
        endif()
        add_compile_options("-march=${level}")
        return()
    endif()

    message(FATAL_ERROR
            "REFRAIN_ARCH_LEVEL is not supported for target architecture '${CMAKE_SYSTEM_PROCESSOR}'")
endfunction()

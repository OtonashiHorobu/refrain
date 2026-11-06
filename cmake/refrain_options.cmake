# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

# Project options and shared compile options for every first-party target.
#
# Policy: C++26, no exceptions used internally (std::expected everywhere), no
# RTTI, warnings are errors. Third-party headers pulled in via Conan's CMakeDeps
# are imported targets with SYSTEM include directories, so their warnings never
# fail our build.

# --- options ---------------------------------------------------------------
option(REFRAIN_BUILD_TESTS "Build unit tests" ON)
option(REFRAIN_BUILD_BENCH "Build benchmarks" ON)
option(REFRAIN_BUILD_FUZZ "Build libFuzzer harnesses (Clang only)" OFF)
# Library-comparison benchmarks are optional benchmark sources that compare
# third-party implementations behind the project's interfaces. They are not
# part of this tree, so the target is created only when the sources are present
# in REFRAIN_BENCH_VARIANTS_DIR.
option(REFRAIN_BUILD_BENCH_VARIANTS
       "Build the optional library-comparison benchmarks" OFF)
option(REFRAIN_BUILD_SERVER "Build refrain-server" ON)
# REFRAIN_BUILD_CLIENT and REFRAIN_MONOLITHIC are accepted but not wired yet:
# the client stack and static mod linkage are future work. They are kept so the
# option surface (conanfile.py + CMake) stays stable.
option(REFRAIN_BUILD_CLIENT "Build the client stack (not wired yet)" OFF)
option(REFRAIN_USE_REFLECTION "Enable refl-cpp based refrain::reflect glue" ON)
option(REFRAIN_WITH_CRASHPAD "Enable Crashpad crash reporting" ON)
option(REFRAIN_WITH_WASM "Enable the wasmtime WASM runtime" ON)
option(REFRAIN_WITH_MIMALLOC
       "Use mimalloc as the engine's general-purpose allocation backend" ON)
option(REFRAIN_MIMALLOC_OVERRIDE_NEW
       "Replace the process-wide operator new/delete pair with mimalloc" ON)
# Standalone Asio is the async/network runtime base; io_uring is an
# optional Linux-only file-I/O backend (liburing). Without it Asio keeps its
# epoll reactor for sockets/timers, but on Linux it exposes no file classes at
# all - block-level reads then stay on a project worker thread. The Conan
# toolchain forces this OFF on non-Linux hosts (the liburing recipe is
# Linux-only); the plain-cmake default below matches so a Windows/macOS build
# without the Conan variable still configures.
if(WIN32 OR APPLE)
    set(REFRAIN_WITH_IO_URING_DEFAULT OFF)
else()
    set(REFRAIN_WITH_IO_URING_DEFAULT ON)
endif()
option(REFRAIN_WITH_IO_URING
       "Enable Asio's io_uring file backend (Linux + liburing)"
       ${REFRAIN_WITH_IO_URING_DEFAULT})
option(REFRAIN_MONOLITHIC "Link first-party mods statically (not wired)" ON)
# Comma-separated sanitizer list ("address,undefined" or "thread"); empty =
# disabled. CI has dedicated ASan/UBSan and TSan jobs.
set(REFRAIN_SANITIZE "" CACHE STRING "Comma-separated -fsanitize list")
# Baseline hardening: stack protector/clash protection, CET, hidden
# visibility, fortified libc calls and RELRO/BIND_NOW. Enabled by default;
# `REFRAIN_HARDEN_MAX` adds zero-initialized stack objects and caller-saved
# register clearing at a measurable runtime cost.
option(REFRAIN_HARDEN "Enable the baseline security hardening flags" OFF)
option(REFRAIN_HARDEN_MAX
       "Also zero stack storage and clear caller-saved registers on return"
       OFF)
# Link-time optimization for optimized builds. Disabled for sanitizer builds
# (instrumentation and LTO interact poorly and build time doubles) and for the
# MSVC toolchain (LTCG build time is not worth it for the CI budget); both can
# be overridden explicitly.
option(REFRAIN_LTO "Enable link-time optimization for optimized builds" OFF)

if(NOT REFRAIN_WITH_MIMALLOC)
    set(REFRAIN_MIMALLOC_OVERRIDE_NEW OFF)
endif()

# --- shared compile options -----------------------------------------------
function(refrain_apply_warnings target)
    if(MSVC)
        # /GR- mirrors -fno-rtti; /utf-8 is required by fmt's Unicode checks.
        # C4702 fires inside fmt's constexpr-heavy headers at /O2; the
        # diagnostic is attributed to the instantiation, so /external:W0 does
        # not suppress it. C4324 flags the deliberate alignas() padding used by
        # the allocator tests.
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8
                                               /GR- /Zc:__cplusplus /wd4702
                                               /wd4324)
        refrain_apply_hardening(${target})
        refrain_apply_lto(${target})
        refrain_apply_link_hardening(${target})
        return()
    endif()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow
                                            -Wnon-virtual-dtor -Wunused
                                            -Woverloaded-virtual -Wformat=2 -Werror
                                            -fno-rtti)

    # Clang exposes deprecation warnings inside the C++26 standard library that
    # some annotation/reflection headers trip over; suppress them for every
    # first-party target.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
    endif()
    # Every first-party target opts into the same hardening/LTO policy. Calling
    # these here keeps plain add_executable() call sites from silently missing
    # the flags.
    refrain_apply_hardening(${target})
    refrain_apply_lto(${target})
    refrain_apply_link_hardening(${target})
endfunction()

# Debug builds carry REFRAIN_DEBUG for asserts that should not run in
# optimized builds; ASan/UBSan/TSan stay opt-in through CMake presets.
function(refrain_apply_debug_options target)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${target} PRIVATE REFRAIN_DEBUG=1)
    endif()
endfunction()

# Security hardening applied to every first-party target. The flags are the
# Linux distro baseline (Ubuntu/Debian hardening build flags) plus CET and
# hidden visibility, which also removes unnecessary PLT indirection.
function(refrain_apply_hardening target)
    # Sanitizer builds are analysis configurations, not release artifacts;
    # keep the instrumentation readable and the build fast.
    if(NOT REFRAIN_HARDEN OR REFRAIN_SANITIZE)
        return()
    endif()
    if(MSVC)
        # /GS (buffer security check) is on by default; Control Flow Guard is
        # the remaining low-cost mitigation and needs the matching link flag.
        target_compile_options(${target} PRIVATE /guard:cf)
        return()
    endif()
    target_compile_options(${target} PRIVATE -fstack-protector-strong
                                            -fstack-clash-protection
                                            -fvisibility=hidden)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
        target_compile_options(${target} PRIVATE -fcf-protection=full)
    endif()
    if(NOT WIN32 AND
       CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        # glibc 2.34+ implements level 3; older headers fall back to level 2
        # semantics through the same macro on supported distributions.
        target_compile_definitions(${target} PRIVATE _FORTIFY_SOURCE=3)
    endif()
    if(REFRAIN_HARDEN_MAX)
        target_compile_options(${target} PRIVATE -ftrivial-auto-var-init=zero
                                                -fzero-call-used-regs=used-gpr)
    endif()
endfunction()

# Executable-only link hardening (static libraries have no link step).
function(refrain_apply_link_hardening target)
    if(NOT REFRAIN_HARDEN)
        return()
    endif()
    if(MSVC)
        target_link_options(${target} PRIVATE /guard:cf)
    elseif(WIN32)
        target_link_options(${target} PRIVATE -Wl,--dynamicbase -Wl,--nxcompat
                                               -Wl,--high-entropy-va)
    elseif(UNIX AND NOT APPLE)
        target_link_options(${target} PRIVATE -Wl,-z,relro -Wl,-z,now
                                               -Wl,-z,noexecstack
                                               -Wl,--as-needed)
    endif()
endfunction()

# LTO is only meaningful for optimized builds; it is skipped for sanitizer
# instrumentation (the sanitizer runtime and LTO interact badly and build time
# doubles). CMake's IPO property adds the right -flto/-fno-fat-lto-objects pair
# for GCC/Clang and /GL + /LTCG for MSVC.
function(refrain_apply_lto target)
    if(NOT REFRAIN_LTO OR REFRAIN_SANITIZE OR MSVC)
        return()
    endif()
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        return()
    endif()
    if(NOT REFRAIN_IPO_SUPPORTED)
        return()
    endif()
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endfunction()

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

if(NOT REFRAIN_WITH_MIMALLOC)
    set(REFRAIN_MIMALLOC_OVERRIDE_NEW OFF)
endif()

# --- shared compile options -----------------------------------------------
function(refrain_apply_warnings target)
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
endfunction()

# Debug builds get the usual stack protector coverage; ASan/UBSan/TSan are
# opt-in through CMake presets (CI matrix) so release timings stay honest.
function(refrain_apply_debug_options target)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target} PRIVATE -fstack-protector-strong)
        target_compile_definitions(${target} PRIVATE REFRAIN_DEBUG=1)
    endif()
endfunction()

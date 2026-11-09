# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

cmake_minimum_required(VERSION 4.2)
project(Refrain VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/refrain_options.cmake)

# LTO support probe: check_ipo_supported links a small program with the
# toolchain (no run), so it also works for the MinGW cross build. The
# sanitizer builds and MSVC opt out through REFRAIN_LTO/refrain_apply_lto.
if(REFRAIN_LTO AND NOT REFRAIN_SANITIZE AND NOT MSVC)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT REFRAIN_IPO_SUPPORTED LANGUAGES C CXX)
    if(NOT REFRAIN_IPO_SUPPORTED)
        message(STATUS "[Refrain] LTO requested but unsupported; disabling it")
    endif()
else()
    set(REFRAIN_IPO_SUPPORTED OFF)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # CMake 4.4's MSVC module stops at C++23; MSVC maps 23 to /std:c++latest,
    # which enables the C++26 preview features the toolset implements.
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
# The engine does not use C++26 modules. CMake 4.x defaults
# CMAKE_CXX_SCAN_FOR_MODULES to ON for C++26, which injects
# -fmodules-ts/-fmodule-mapper/-fdeps-format into every translation unit
# (slower builds, unparsable by clangd/clang tools). Turn it off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# windows.h defines min/max macros that collide with std::min/std::max and
# std::numeric_limits<T>::max(). The engine never wants those macros.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()
if(MSVC)
    # getenv/sprintf-style APIs are used deliberately; the secure-CRT
    # deprecation warnings do not describe a defect here.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Sanitizer instrumentation for every first-party target (the CI matrix uses
# dedicated ASan/UBSan and TSan builds; Release timings stay unaffected).
if(REFRAIN_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "REFRAIN_SANITIZE requires GCC or Clang")
    endif()
    message(STATUS "[Refrain] sanitizers: ${REFRAIN_SANITIZE}")
    # GCC 16's -Wmaybe-uninitialized reports false positives at -O3 once the
    # sanitizer instrumentation is inlined; keep the warning out of the gate.
    add_compile_options(-fsanitize=${REFRAIN_SANITIZE} -fno-omit-frame-pointer
                        -g -Wno-maybe-uninitialized)
    add_link_options(-fsanitize=${REFRAIN_SANITIZE})
    # Test code uses this to skip the deliberate-fault child-process test: the
    # ASan/UBSan/TSan runtimes intercept intentionally undefined behaviour
    # (the null dereference is expected to be flagged by UBSan), so "died by
    # signal" cannot be observed faithfully under instrumentation. The signal
    # contract is covered by the regular build.
    add_compile_definitions(REFRAIN_SANITIZE_BUILD=1)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        "Release"
        CACHE STRING "Build type" FORCE)
endif()

# A plain `cmake -S . -B build` without the Conan toolchain must still
# produce a sensible version string instead of "Refrain ".
if(NOT REFRAIN_VERSION)
    set(REFRAIN_VERSION "${PROJECT_VERSION}")
endif()
set(REFRAIN_VERSION "${REFRAIN_VERSION}" CACHE STRING "Refrain release version")
set(REFRAIN_BRANCH "${REFRAIN_BRANCH}" CACHE STRING "Git branch name")
set(REFRAIN_COMMIT "${REFRAIN_COMMIT}" CACHE STRING "Git commit hash")
set(REFRAIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin" CACHE STRING "Runtime output dir")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REFRAIN_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(cmake/refrain_target.cmake)

# --- third-party (Conan / CMakeDeps) --------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
# Content block compression (zstd) and content hashing (BLAKE3), Ed25519
# signatures for signed `.rfmod` manifests (libsodium) and zlib for NBT
# gzip/zlib payloads (zlib-ng drop-in).
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(ZLIB CONFIG REQUIRED)
# libarchive reads `.rfmod` tar (ustar/PAX/GNU) from memory, crc32c replaces
# the hand-written container checksum table. Both are implementation details
# consumed PRIVATE below; public headers never include <archive.h> or
# <crc32c/crc32c.h>.
find_package(LibArchive CONFIG REQUIRED)
find_package(Crc32c CONFIG REQUIRED)
# Standalone Asio is the async/network runtime base. The CCI recipe is a
# header-only interface target (`asio::asio`) that also exports
# ASIO_STANDALONE, so no Boost headers are needed. Asio is a kernel
# implementation detail: only src/core/async.cpp includes it, and no public
# header may.
find_package(asio CONFIG REQUIRED)
# Optional Linux io_uring backend for Asio's file classes. liburing is
# Linux-only, so the option is forced OFF by the Conan toolchain elsewhere;
# sockets/timers then stay on Asio's epoll reactor and block-level reads use a
# project worker thread (Asio 1.38 exposes no file classes on Linux without
# ASIO_HAS_IO_URING).
if(REFRAIN_WITH_IO_URING)
    find_package(liburing CONFIG REQUIRED)
endif()
if(REFRAIN_BUILD_TESTS)
    find_package(ut CONFIG REQUIRED)
endif()
if(REFRAIN_BUILD_BENCH)
    find_package(benchmark CONFIG REQUIRED)
endif()

if(REFRAIN_WITH_CRASHPAD)
    find_package(crashpad CONFIG REQUIRED)
endif()

if(REFRAIN_WITH_WASM)
    find_package(wasmtime CONFIG REQUIRED)
endif()

if(REFRAIN_USE_REFLECTION)
    find_package(refl-cpp CONFIG REQUIRED)
    find_package(magic_enum CONFIG REQUIRED)
    find_package(EnTT CONFIG REQUIRED)

    # Reflection backend facade (include/refrain/reflect.hpp). Business code
    # only includes the facade; the backend libraries are consumed as imported
    # targets, i.e. their includes are treated as SYSTEM (-isystem), which
    # keeps refl-cpp 0.12.4's C++17-era trait usage from breaking the project's
    # -Werror build under C++26.
    add_library(refrain-reflect INTERFACE)
    target_link_libraries(refrain-reflect INTERFACE refl-cpp::refl-cpp
                                                   magic_enum::magic_enum)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang may still warn through -isystem for some refl-cpp
        # deprecations; scope the suppression to reflection consumers.
        target_compile_options(refrain-reflect INTERFACE
                               -Wno-deprecated-declarations)
    endif()
endif()

if(REFRAIN_WITH_MIMALLOC)
    find_package(mimalloc CONFIG REQUIRED)
endif()

# --- generated version ----------------------------------------------------
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(cmake/version.cpp.in "${CMAKE_BINARY_DIR}/generated/version.cpp"
               @ONLY)

# --- platform (the ONLY layer allowed to call OS APIs) ---------------------
if(WIN32)
    set(REFRAIN_PLATFORM_IMPL_DIR windows)
else()
    set(REFRAIN_PLATFORM_IMPL_DIR posix)
endif()

refrain_add_library(
    refrain-platform
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/dynamic_library.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/file.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/mmap.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/process.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/random_access_file.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/subprocess.cpp
    src/platform/${REFRAIN_PLATFORM_IMPL_DIR}/thread.cpp)
target_include_directories(refrain-platform PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
# dlopen lives in libc on glibc >= 2.34, separate libdl on older systems.
# fmt formats the random-access-file diagnostics (same convention as the rest
# of the engine); it is already a kernel dependency.
target_link_libraries(refrain-platform PRIVATE ${CMAKE_DL_LIBS} fmt::fmt)
add_library(Refrain::Platform ALIAS refrain-platform)

# --- kernel ---------------------------------------------------------------
refrain_add_library(
    refrain-kernel
    src/core/error.cpp
    src/core/log.cpp
    src/core/time.cpp
    src/core/memory.cpp
    src/core/global_allocator.cpp
    src/core/jobs.cpp
    src/core/event.cpp
    src/core/schedule.cpp
    src/core/config.cpp
    src/core/io.cpp
    src/core/async.cpp
    src/kernel/service_registry.cpp
    src/kernel/crash.cpp
    "${CMAKE_BINARY_DIR}/generated/version.cpp")

target_include_directories(refrain-kernel PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
# fmt is part of the kernel's public surface: include/refrain/core/log.hpp
# includes <fmt/format.h>, so every consumer needs its include directory.
# spdlog and glaze stay implementation details of src/core/log.cpp and
# src/core/config.cpp; on a static library CMake still propagates them to the
# link line as $<LINK_ONLY:...>, so consumers link without seeing their headers.
target_link_libraries(refrain-kernel PUBLIC fmt::fmt)
target_link_libraries(refrain-kernel PRIVATE spdlog::spdlog_header_only
                                             glaze::glaze)
target_compile_definitions(
    refrain-kernel
    PUBLIC REFRAIN_WITH_CRASHPAD=$<BOOL:${REFRAIN_WITH_CRASHPAD}>
           REFRAIN_USE_REFLECTION=$<BOOL:${REFRAIN_USE_REFLECTION}>
           REFRAIN_WITH_MIMALLOC=$<BOOL:${REFRAIN_WITH_MIMALLOC}>
           REFRAIN_MIMALLOC_OVERRIDE_NEW=$<BOOL:${REFRAIN_MIMALLOC_OVERRIDE_NEW}>)
if(REFRAIN_WITH_MIMALLOC)
    target_link_libraries(refrain-kernel PRIVATE mimalloc-static)
endif()
if(REFRAIN_WITH_CRASHPAD)
    target_link_libraries(refrain-kernel PRIVATE crashpad::client)
endif()
target_link_libraries(refrain-kernel PRIVATE refrain-platform)

# Asio is PRIVATE to the kernel; only src/core/async.cpp includes it
# and include/refrain/core/async.hpp exposes a PIMPL handle only. The CCI asio
# target already carries ASIO_STANDALONE; ASIO_NO_DEPRECATED keeps the
# deprecated Asio 1.x interfaces out of the build. ASIO_NO_TYPEID is required
# because the whole project builds with -fno-rtti (Asio's any_io_executor uses
# typeid to detect same-context executors; the portable branch tracks
# outstanding work instead). With io_uring enabled, ASIO_HAS_IO_URING switches
# Asio's stream_file/random_access_file service to the liburing backend (Linux
# only) - without it Asio does not expose its file classes on Linux at all.
# The defines are harmless for the other kernel TUs: they do not include Asio.
target_link_libraries(refrain-kernel PRIVATE asio::asio)
if(WIN32)
    # Standalone Asio's Windows backend calls Winsock (WSAStartup/WSACleanup);
    # the import library is not implicit with MinGW.
    target_link_libraries(refrain-kernel PRIVATE ws2_32)
endif()
target_compile_definitions(refrain-kernel PRIVATE ASIO_NO_DEPRECATED=1
                                                  ASIO_NO_TYPEID=1)
if(REFRAIN_WITH_IO_URING)
    target_link_libraries(refrain-kernel PRIVATE liburing::liburing)
    target_compile_definitions(refrain-kernel PRIVATE ASIO_HAS_IO_URING=1)
endif()

# Reflection is part of the kernel's public surface (PUBLIC linkage): any
# target linking refrain-kernel can include <refrain/reflect.hpp> without extra
# wiring, while backend headers never enter the kernel's own translation units.
if(REFRAIN_USE_REFLECTION)
    target_link_libraries(refrain-kernel PUBLIC refrain-reflect)
endif()

add_library(Refrain::Kernel ALIAS refrain-kernel)

# --- contracts (generated headers arrive with the schema toolchain) -------
add_library(refrain-contracts INTERFACE)
# The installed layout merges include/ and sdk/generated/{c,cpp} into
# ${CMAKE_INSTALL_INCLUDEDIR}, so it needs a single INSTALL_INTERFACE include
# directory; the generated headers are installed too, otherwise the headers
# would be unusable in the install tree.
target_include_directories(refrain-contracts INTERFACE
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/sdk/generated/c>
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/sdk/generated/cpp>
                           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(Refrain::Contracts ALIAS refrain-contracts)
set_target_properties(refrain-contracts PROPERTIES EXPORT_NAME Contracts)

# --- schema toolchain (hand-written RSC/RIDL parser + deterministic emitters)
refrain_add_library(
    refrain-schema
    tools/schema/diagnostic.cpp
    tools/schema/lexer.cpp
    tools/schema/parser.cpp
    tools/schema/sema.cpp
    tools/schema/emit_ridl.cpp
    tools/schema/emit_rsc.cpp
    tools/schema/generator.cpp)
target_include_directories(refrain-schema PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/tools>
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(refrain-schema PRIVATE xxHash::xxhash)
add_library(Refrain::Schema ALIAS refrain-schema)

# --- archive (zip reader + data manifest) ----------------------------------
refrain_add_library(refrain-archive
    src/archive/zip.cpp
    src/archive/manifest.cpp)
target_link_libraries(refrain-archive PRIVATE refrain-kernel libzip::zip
                                            simdjson::simdjson)
add_library(Refrain::Archive ALIAS refrain-archive)

# --- data layer: pack containers, VFS, JSON/NBT codecs, registry, cook ------
# New implementation files are picked up automatically: this target uses
# GLOB_RECURSE with CONFIGURE_DEPENDS (below), so a new `src/data/*.cpp` is
# compiled without editing this file.
file(GLOB_RECURSE REFRAIN_DATA_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/data/*.cpp")
refrain_add_library(refrain-data ${REFRAIN_DATA_SOURCES})
target_link_libraries(refrain-data PRIVATE refrain-kernel refrain-platform
                                              refrain-archive
                                              simdjson::simdjson
                                              zstd::libzstd_static
                                              BLAKE3::blake3 ZLIB::ZLIB
                                              # Ed25519 signatures for signed
                                              # `.rfmod` manifests: the CCI
                                              # libsodium package is consumed
                                              # PRIVATE; public headers never
                                              # include <sodium.h>.
                                              libsodium::libsodium
                                              # container/index/record checksums
                                              # (replaces the hand-written
                                              # CRC32C table); PRIVATE keeps
                                              # <crc32c/crc32c.h> out of the
                                              # public API.
                                              Crc32c::crc32c)
add_library(Refrain::Data ALIAS refrain-data)

# --- world storage: model, RWF v1 container, Anvil interop ------------------
# New `src/world/*.cpp` files are compiled without editing this file.
file(GLOB_RECURSE REFRAIN_WORLD_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/world/*.cpp")
refrain_add_library(refrain-world ${REFRAIN_WORLD_SOURCES})
# refrain-contracts carries the generated storage model (schema/storage/chunk.rsc)
# that include/refrain/world/model.hpp exposes; zstd/lz4/xxhash/crc32c/zlib stay
# PRIVATE so their headers never leak into the public API.
target_link_libraries(refrain-world PUBLIC refrain-kernel refrain-contracts
                                       PRIVATE refrain-data
                                               zstd::libzstd_static
                                               LZ4::lz4_static
                                               xxHash::xxhash
                                               Crc32c::crc32c ZLIB::ZLIB)
add_library(Refrain::World ALIAS refrain-world)

# --- simulation runtime: block registry, loaded chunks, tickets -------------
# New src/sim files are picked up without editing this file.
file(GLOB_RECURSE REFRAIN_SIM_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/sim/*.cpp")
refrain_add_library(refrain-sim ${REFRAIN_SIM_SOURCES})
target_link_libraries(refrain-sim PUBLIC refrain-kernel refrain-world
                                       refrain-data refrain-contracts)
add_library(Refrain::Sim ALIAS refrain-sim)

# --- mod host: loads built-in mods against the embedded archive ------------
# The host needs the data layer (RFSI/RFBT reader), so it is a separate library
# on top of kernel; the kernel itself stays data-free.
# src/mod sources are globbed, so a new file is compiled without editing this
# file (rfmod packaging, discovery/lock, provider wiring).
file(GLOB_RECURSE REFRAIN_MOD_HOST_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/mod/*.cpp")
refrain_add_library(refrain-mod-host ${REFRAIN_MOD_HOST_SOURCES})
# zstd: .rfmod tar.zst code includes <zstd.h> directly, so the dependency must
# be on this target (refrain-data keeps it PRIVATE and would not propagate the
# include directory on a runner without a system libzstd).
# libarchive: `.rfmod` tar read side (ustar/PAX/GNU), in-memory only. The CCI
# package is built with every compression filter off, so zstd decompression
# stays in our own bounded streaming stage. PRIVATE keeps <archive.h> out of
# the public headers.
target_link_libraries(refrain-mod-host PUBLIC refrain-kernel
                                             PRIVATE refrain-data refrain-contracts
                                                     zstd::libzstd_static
                                                     LibArchive::LibArchive)
# MinGW's libarchive uses BCryptGenRandom (Windows CNG); MSVC's import library
# pulls it in transitively, but the MinGW static archive needs bcrypt explicitly.
if(WIN32)
    target_link_libraries(refrain-mod-host PRIVATE bcrypt)
endif()
add_library(Refrain::ModHost ALIAS refrain-mod-host)

# --- host runtime API: sealed `refrain:host/core` adapter -------------------
# The adapter translates the generated C ABI table to the abstract host
# context. It deliberately depends on kernel/contracts only; the concrete
# context (service registry, config, builtin image) is supplied by the
# composition root, so tests and embedders can use a fake.
refrain_add_library(refrain-host src/host/core_api.cpp)
target_link_libraries(refrain-host PUBLIC refrain-contracts refrain-kernel)
add_library(Refrain::Host ALIAS refrain-host)

# --- first-party static mods -----------------------------------------------
# First-party mod code lives under mods/<id>/src/ and is statically linked into
# the host; manifests/data stay in the builtin RFSI content image. REFR_MOD_STATIC
# registrars run during static initialization.
file(GLOB_RECURSE REFRAIN_STATIC_MOD_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/mods/*/src/*.cpp")
# OBJECT library: static registrars (REFR_MOD_STATIC) run from static
# initializers, so the objects must be linked into every host binary even though
# no symbol from them is referenced directly. A STATIC archive would be dropped
# by the linker's --gc/needed-object heuristics.
add_library(refrain-first-party-mods OBJECT ${REFRAIN_STATIC_MOD_SOURCES})
target_include_directories(refrain-first-party-mods PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
# The server runtime mod is a thin adapter over the in-process `app` API, so the
# static-mod object library needs `refrain-app`. No cycle: refrain-app only
# depends on kernel/data/world/sim/mod-host/schema/archive/script and never
# links the first-party mods.
target_link_libraries(refrain-first-party-mods PUBLIC refrain-kernel
                                                    refrain-contracts
                                                    refrain-mod-host
                                                    refrain-sim
                                                    refrain-app
                                                    refrain-server-runtime)
refrain_apply_warnings(refrain-first-party-mods)
add_library(Refrain::FirstPartyMods ALIAS refrain-first-party-mods)

# --- first-party script runtimes (mods, not kernel) -------------------------
# The WASM runtime is a first-party script module that provides the
# `refrain:script` contract. The kernel does not link wasmtime; hosts/tools that
# want script support link this target explicitly.
refrain_add_library(refrain-script-wasm
    src/script/wasm.cpp)
target_include_directories(refrain-script-wasm PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(refrain-script-wasm PUBLIC
                           REFRAIN_WITH_WASM=$<BOOL:${REFRAIN_WITH_WASM}>)
if(REFRAIN_WITH_WASM)
    target_link_libraries(refrain-script-wasm PRIVATE wasmtime::wasmtime)
endif()
add_library(Refrain::ScriptWasm ALIAS refrain-script-wasm)

# --- application service layer ---------------------------------------------
# The command bodies shared by refrain-tools and refrain-server. Both mains
# only parse CLI arguments and call one app::*_service function.
file(GLOB REFRAIN_APP_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/app/*.cpp")
refrain_add_library(refrain-app ${REFRAIN_APP_SOURCES})
target_link_libraries(refrain-app PUBLIC refrain-kernel refrain-data
                                             refrain-world refrain-sim
                                             refrain-mod-host refrain-host
                                             refrain-schema refrain-archive
                                             refrain-script-wasm)
add_library(Refrain::App ALIAS refrain-app)

# --- headless server runtime ------------------------------------------------
# The tick loop, console and watchdog live above the shared app services: the
# runtime consumes them, tools never link this target.
file(GLOB REFRAIN_SERVER_RUNTIME_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/server/*.cpp")
refrain_add_library(refrain-server-runtime ${REFRAIN_SERVER_RUNTIME_SOURCES})
target_link_libraries(refrain-server-runtime PUBLIC refrain-app)
add_library(Refrain::ServerRuntime ALIAS refrain-server-runtime)

# --- tools -----------------------------------------------------------------
add_executable(refrain-tools src/main/tools.cpp)
target_link_libraries(refrain-tools PRIVATE refrain-app
                                        refrain-first-party-mods CLI11::CLI11)
refrain_apply_warnings(refrain-tools)

# --- built-in content image ------------------------------------------------
# One deterministic RFSI v1 image with every built-in mod's manifest/data/
# assets/scripts is generated at build time and embedded into the host.
# Built-in mod code is statically linked (refrain/tools +
# kernel/static_mod.hpp); the image is data only. Source of truth:
# mods/<id>/manifest.toml.
file(GLOB_RECURSE REFRAIN_MOD_TREE_FILES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/mods/*")
set(REFRAIN_BUILTIN_IMAGE "${CMAKE_BINARY_DIR}/generated/builtin.image")
set(REFRAIN_BUILTIN_CPP "${CMAKE_BINARY_DIR}/generated/builtin_image.cpp")
# Cross builds cannot execute the target-platform `refrain-tools`. The image is
# host-independent data, so a host tool may generate it; the target executable
# still embeds the same deterministic bytes.
if(NOT DEFINED REFRAIN_HOST_TOOLS)
    set(REFRAIN_HOST_TOOLS "")
endif()
if(CMAKE_CROSSCOMPILING)
    if(NOT REFRAIN_HOST_TOOLS)
        message(FATAL_ERROR
            "Cross builds need -DREFRAIN_HOST_TOOLS=<path to a native "
            "refrain-tools> to generate ${REFRAIN_BUILTIN_IMAGE}")
    endif()
    if(NOT EXISTS "${REFRAIN_HOST_TOOLS}")
        message(FATAL_ERROR
            "REFRAIN_HOST_TOOLS '${REFRAIN_HOST_TOOLS}' does not exist")
    endif()
    set(REFRAIN_BUILTIN_COMMAND "${REFRAIN_HOST_TOOLS}")
    set(REFRAIN_BUILTIN_DEPENDS ${REFRAIN_MOD_TREE_FILES})
else()
    set(REFRAIN_BUILTIN_COMMAND $<TARGET_FILE:refrain-tools>)
    set(REFRAIN_BUILTIN_DEPENDS refrain-tools ${REFRAIN_MOD_TREE_FILES})
endif()
add_custom_command(
    OUTPUT "${REFRAIN_BUILTIN_IMAGE}" "${REFRAIN_BUILTIN_CPP}"
    COMMAND "${REFRAIN_BUILTIN_COMMAND}" builtin image
            --mods "${PROJECT_SOURCE_DIR}/mods"
            -o "${REFRAIN_BUILTIN_IMAGE}"
            --emit-cpp "${REFRAIN_BUILTIN_CPP}"
    DEPENDS ${REFRAIN_BUILTIN_DEPENDS}
    COMMENT "[Refrain] building the built-in content image (builtin.image)"
    VERBATIM)
refrain_add_library(refrain-builtin "${REFRAIN_BUILTIN_CPP}")
add_library(Refrain::Builtin ALIAS refrain-builtin)

# --- server ----------------------------------------------------------------
if(REFRAIN_BUILD_SERVER)
    add_executable(refrain-server src/main/server.cpp)
    target_link_libraries(refrain-server PRIVATE refrain-app refrain-builtin
                                               refrain-first-party-mods
                                               refrain-server-runtime
                                               CLI11::CLI11)
    refrain_apply_warnings(refrain-server)
endif()

# --- tests -----------------------------------------------------------------
if(REFRAIN_BUILD_TESTS)
    enable_testing()
    file(GLOB_RECURSE REFRAIN_TEST_SOURCES CONFIGURE_DEPENDS
         "${PROJECT_SOURCE_DIR}/test/*.cpp")
    add_executable(refrain-tests ${REFRAIN_TEST_SOURCES})
    target_include_directories(refrain-tests PRIVATE
                               "${PROJECT_SOURCE_DIR}/test")
    target_link_libraries(refrain-tests PRIVATE refrain-app refrain-kernel
                                              refrain-schema refrain-contracts
                                              refrain-archive refrain-data
                                              refrain-world refrain-sim
                                              refrain-builtin refrain-mod-host
                                              refrain-host
                                              refrain-first-party-mods
                                              refrain-script-wasm libzip::zip
                                              LZ4::lz4_static
                                              xxHash::xxhash Boost::ut glaze::glaze
                                              EnTT::EnTT zstd::libzstd_static)
    target_compile_definitions(refrain-tests PRIVATE
                               REFRAIN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(REFRAIN_WITH_MIMALLOC)
        # Tests assert the global operator new/delete override is live via
        # mi_is_in_heap_region(), so they need the mimalloc headers/lib too.
        target_link_libraries(refrain-tests PRIVATE mimalloc-static)
    endif()
    refrain_apply_warnings(refrain-tests)
    add_test(NAME refrain-tests COMMAND refrain-tests)
endif()

# --- benchmarks ------------------------------------------------------------
if(REFRAIN_BUILD_BENCH)
    file(GLOB_RECURSE REFRAIN_BENCH_SOURCES CONFIGURE_DEPENDS
         "${PROJECT_SOURCE_DIR}/bench/src/*.cpp")
    add_executable(refrain-bench ${REFRAIN_BENCH_SOURCES})
    target_include_directories(refrain-bench PRIVATE "${PROJECT_SOURCE_DIR}/bench/include")
    target_link_libraries(refrain-bench PRIVATE refrain-kernel
                          refrain-contracts refrain-world refrain-sim
                          refrain-data benchmark::benchmark_main)
    refrain_apply_warnings(refrain-bench)

    # Library-comparison benchmarks: the sources are optional and live outside
    # this tree, so the target exists only when the option is enabled and
    # REFRAIN_BENCH_VARIANTS_DIR actually contains sources.
    if(REFRAIN_BUILD_BENCH_VARIANTS)
        if(NOT DEFINED REFRAIN_BENCH_VARIANTS_DIR)
            set(REFRAIN_BENCH_VARIANTS_DIR
                "${PROJECT_SOURCE_DIR}/bench/variants"
                CACHE PATH
                "Directory with the optional library-comparison benchmark sources")
        endif()
        file(GLOB REFRAIN_BENCH_VARIANT_SOURCES CONFIGURE_DEPENDS
             "${REFRAIN_BENCH_VARIANTS_DIR}/*.cpp")
        if(REFRAIN_BENCH_VARIANT_SOURCES)
            add_executable(refrain-bench-variants ${REFRAIN_BENCH_VARIANT_SOURCES})
            target_include_directories(refrain-bench-variants PRIVATE
                                       "${PROJECT_SOURCE_DIR}/bench/include")
            # This target includes <glaze/...> directly, so it needs the
            # dependency explicitly because refrain-kernel keeps glaze PRIVATE.
            target_link_libraries(refrain-bench-variants PRIVATE refrain-kernel
                                  benchmark::benchmark_main ZLIB::ZLIB
                                  glaze::glaze)
            if(REFRAIN_WITH_MIMALLOC)
                target_link_libraries(refrain-bench-variants PRIVATE
                                      mimalloc-static)
            endif()
            refrain_apply_warnings(refrain-bench-variants)
        else()
            message(STATUS "[Refrain] no library-comparison benchmark sources in "
                           "${REFRAIN_BENCH_VARIANTS_DIR}; "
                           "refrain-bench-variants is skipped")
        endif()
    endif()
endif()

# --- fuzz (opt-in, Clang only) --------------------------------------------
# The harness sources are optional and may live outside this tree; point
# REFRAIN_FUZZ_SOURCE_DIR at them when fuzzing locally.
if(REFRAIN_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "REFRAIN_BUILD_FUZZ requires Clang (libFuzzer)")
    endif()
    if(NOT DEFINED REFRAIN_FUZZ_SOURCE_DIR)
        set(REFRAIN_FUZZ_SOURCE_DIR "${PROJECT_SOURCE_DIR}/fuzz" CACHE PATH
            "Directory with the libFuzzer harness sources")
    endif()
    if(EXISTS "${REFRAIN_FUZZ_SOURCE_DIR}/fuzz_nbt.cpp"
       AND EXISTS "${REFRAIN_FUZZ_SOURCE_DIR}/fuzz_json.cpp")
        foreach(fuzz_name nbt json)
            add_executable(refrain-fuzz-${fuzz_name}
                           "${REFRAIN_FUZZ_SOURCE_DIR}/fuzz_${fuzz_name}.cpp")
            target_link_libraries(refrain-fuzz-${fuzz_name}
                                  PRIVATE refrain-data glaze::glaze)
            target_include_directories(refrain-fuzz-${fuzz_name}
                                       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
            target_compile_options(refrain-fuzz-${fuzz_name}
                                   PRIVATE -fsanitize=fuzzer,address
                                           -fno-omit-frame-pointer)
            target_link_options(refrain-fuzz-${fuzz_name}
                                PRIVATE -fsanitize=fuzzer,address)
            refrain_apply_warnings(refrain-fuzz-${fuzz_name})
        endforeach()
    else()
        message(STATUS "[Refrain] no libFuzzer harnesses in "
                       "${REFRAIN_FUZZ_SOURCE_DIR}; refrain-fuzz targets are "
                       "skipped")
    endif()
endif()

# --- install ---------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS refrain-kernel refrain-platform
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# The header-only contracts target is the public SDK surface: install it with
# an export set so consumers can `find_package(Refrain)`.
install(TARGETS refrain-contracts EXPORT RefrainContractsTargets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS refrain-tools RUNTIME DESTINATION bin)
if(REFRAIN_BUILD_SERVER)
    install(TARGETS refrain-server RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/refrain DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Generated C/C++ contract headers are part of the SDK; their internal include
# roots (`refrain/contracts/...`) merge into the include directory.
install(DIRECTORY sdk/generated/c/refrain
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY sdk/generated/cpp/refrain
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT RefrainContractsTargets
        FILE RefrainContractsTargets.cmake
        NAMESPACE Refrain::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Refrain)
configure_package_config_file(
    cmake/RefrainConfig.cmake.in
    "${CMAKE_BINARY_DIR}/RefrainConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Refrain)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/RefrainConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_BINARY_DIR}/RefrainConfig.cmake"
              "${CMAKE_BINARY_DIR}/RefrainConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Refrain)

message(STATUS "[Refrain] version: ${REFRAIN_VERSION} ${REFRAIN_BRANCH}:${REFRAIN_COMMIT}")
message(STATUS "[Refrain] crashpad=${REFRAIN_WITH_CRASHPAD} wasm=${REFRAIN_WITH_WASM} "
               "reflection=${REFRAIN_USE_REFLECTION} "
               "mimalloc=${REFRAIN_WITH_MIMALLOC} "
               "global-new=${REFRAIN_MIMALLOC_OVERRIDE_NEW} "
               "io-uring=${REFRAIN_WITH_IO_URING} "
               "tests=${REFRAIN_BUILD_TESTS} bench=${REFRAIN_BUILD_BENCH} "
               "bench-variants=${REFRAIN_BUILD_BENCH_VARIANTS}")

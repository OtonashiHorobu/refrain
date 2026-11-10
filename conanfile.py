# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

import os

from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.scm import Git


class RefrainConan(ConanFile):
    name = "refrain"
    version = "0.1.0-alpha"
    settings = "os", "compiler", "build_type", "arch"

    options = {
        "lto": [True, False],
        "client": [True, False],
        "with_crashpad": [True, False],
        "with_wasm": [True, False],
        "use_reflection": [True, False],
        "with_mimalloc": [True, False],
        "mimalloc_override_new": [True, False],
        "with_io_uring": [True, False],
        "build_tests": [True, False],
        "build_bench": [True, False],
    }
    default_options = {
        "lto": False,
        "client": False,
        "with_crashpad": True,
        "with_wasm": True,
        "use_reflection": True,
        "with_mimalloc": True,
        "mimalloc_override_new": True,
        "with_io_uring": True,
        "build_tests": True,
        "build_bench": True,
    }

    def requirements(self):
        # --- kernel / tools dependencies --------------------------------------
        # spdlog is compiled header-only: a single translation unit includes
        # it, and keeping a separate static library under LTO makes GCC see the
        # same sink vtables through the source and package include paths and
        # report ODR conflicts. The standard-library formatter
        # (`use_std_fmt=True`) avoids the CCI recipe's hard pin on fmt 12.1.0,
        # which would otherwise force a downgrade of the project's fmt.
        self.requires("spdlog/1.17.0",
                      options={"use_std_fmt": True, "header_only": True})
        self.requires("fmt/12.2.0")
        self.requires("cli11/2.7.2")
        # TOML 1.1: generic map-root DOM for gamepack/mod manifests and config
        # writes. JSON stays on simdjson.
        self.requires("glaze/9.1.0")
        # Schema toolchain content hash (xxh64) for generated-file drift checks.
        self.requires("xxhash/0.8.4")
        # --- async/network runtime --------------------------------------------
        # Standalone Asio is the kernel's async base for timers, block-wise
        # streaming reads and (later) the network server. The CCI recipe is
        # header-only BSL-1.0 and exports the `asio::asio` interface target
        # together with `ASIO_STANDALONE`, so no Boost is pulled in. Asio types
        # stay inside src/core/async.cpp (PIMPL): no public header may include
        # <asio/...>.
        self.requires("asio/1.38.2")
        # Optionally pulls in the io_uring file-I/O backend for Asio. liburing's
        # CCI recipe validates `settings.os == Linux`, so every other host drops
        # the requirement; without it Asio keeps epoll for sockets/timers but
        # exposes no file classes on Linux, so block-level reads stay on a
        # project worker thread.
        if self.options.with_io_uring and self.settings.os == "Linux":
            self.requires("liburing/2.15")
        # General-purpose allocator backend: the kernel wires its std::pmr
        # resources (and optionally the global operator new/delete pair) to
        # mimalloc instead of hand-rolling an allocator.
        if self.options.with_mimalloc:
            # In-repo recipe: upstream v3.6.0 (ConanCenter lags).
            self.requires("mimalloc/3.6.0")
        # test-only, header-only (used by refrain-tests); declared as a regular
        # requirement so that CMakeDeps always provides the include dir.
        # Skipped when `-o build_tests=False`; the option reaches both CMake and
        # the dependency graph instead of being a dead switch.
        if self.options.build_tests:
            self.requires("boost-ext-ut/2.3.1")
        # Google Benchmark (MIT) drives the `bench` target.
        if self.options.build_bench:
            # In-repo recipe: upstream v1.10.0 (ConanCenter lags).
            self.requires("benchmark/1.10.0")

        # --- annotation-based static reflection is glue only; wire/save/ABI
        # formats are owned by RSC/RIDL/RCD schema codegen.
        if self.options.use_reflection:
            self.requires("refl-cpp/0.12.4")
            self.requires("magic_enum/0.9.8")
            self.requires("entt/4.0.0")
        # --- JSON layer (data packs / admin / config; never wire). simdjson
        # owns both directions: DOM/ondemand parsing + builder-based
        # serialization. TOML uses glaze's generic map-root DOM (see above).
        # In-repo recipe: upstream v5.1.0 (ConanCenter lags).
        self.requires("simdjson/5.1.0")
        # zlib: the in-repo recipe is a zlib-ng 2.3.3 drop-in replacement
        # packaged as zlib/1.3.1. Declaring it directly pins the in-repo
        # revision even when the transitive ranges (libzip/libcurl/crashpad)
        # are resolved online.
        self.requires("zlib/1.3.1")
        # --- content container + content addressing.
        # zstd backs the 64 KiB block compression of the content store and the
        # `.rfmod` archive stream; BLAKE3 is the content hash for `.rfmod`, the
        # content-addressed blob store and the builtin RFSI image. Both are
        # ConanCenter upstream releases (freshness audited by
        # tools/dep_audit.py); default options keep the static libraries only
        # (no CLI tools).
        self.requires("zstd/1.5.7")
        # Anvil region compression id 4 (26.3 `region-file-compression=lz4`) is
        # lz4-java's LZ4BlockOutputStream, not the standard LZ4 frame. The C
        # library supplies only the block codec; the 13-byte block header and
        # XXHash32 checksum are handled in src/world/anvil_lz4.cpp. Import-only;
        # export stays zlib (vanilla accepts it). PRIVATE to refrain-world.
        self.requires("lz4/1.10.0")
        self.requires("blake3/1.8.7")
        # Ed25519 detached signatures for signed `.rfmod` manifests. libsodium
        # 1.0.22 is ConanCenter's latest and equals the upstream
        # 1.0.22-RELEASE tag, so CCI can be consumed directly (no in-repo
        # recipe). Only `crypto_sign_*` is used and
        # `include/refrain/data/signature.hpp` keeps the header out of our
        # public API; the CCI recipe builds a static library on Linux.
        self.requires("libsodium/1.0.22")
        # Zip archive reader/writer for `refrain-tools extract` (jar/asset
        # import into the user-local cooked cache); the archive reader is an
        # implementation detail of refrain-data. ConanCenter has current
        # recipes + Linux binaries.
        # Minecraft jars only use stored/deflate entries; skip bzip2/lzma/
        # zstd/crypto to keep the dependency graph small and reproducible.
        self.requires(
            "libzip/1.12.1",
            options={
                "with_bzip2": False,
                "with_lzma": False,
                "with_zstd": False,
                "crypto": False,
                "tools": False,
            },
        )
        # `.rfmod` tar read side: libarchive reads POSIX ustar/PAX/GNU headers
        # from memory, replacing the hand-written tar parser. The write side
        # keeps the deterministic strict-ustar writer.
        # Minimal capability set: tar format support is always compiled in
        # (`ENABLE_TAR` only controls the bsdtar CLI), so every filter/format
        # dependency is disabled. `.rfmod` decompression stays with our own
        # zstd stage (avoids double decompression and keeps the size caps in
        # one place). ACL/xattr/iconv are extraction-side features we do not
        # use; keeping them off also avoids system libacl/libiconv leaks.
        #
        # Version: 3.8.11 is upstream's current security and bugfix release,
        # while ConanCenter still ships 3.8.7. A security release must not lag
        # behind, so the recipe is vendored in-repo
        # (`conan/recipes/libarchive/`; sha256
        # 900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b).
        # Rejected: ConanCenter libarchive/3.8.7 (four releases behind, and it
        # predates the security fixes).
        self.requires(
            "libarchive/3.8.11",
            options={
                "shared": False,
                "with_acl": False,
                "with_xattr": False,
                "with_zlib": False,
                "with_bzip2": False,
                "with_lzma": False,
                "with_lz4": False,
                "with_lzo": False,
                "with_zstd": False,
                "with_openssl": False,
                "with_mbedtls": False,
                "with_nettle": False,
                "with_libb2": False,
                "with_libxml2": False,
                "with_expat": False,
                "with_iconv": False,
                "with_pcre2": False,
                "with_pcreposix": False,
                "with_cng": False,
            },
        )
        # Container/index/record checksums: Google's crc32c replaces the
        # hand-written 256-entry table in src/data/checksum.cpp. SSE4.2/ARMv8
        # CRC instructions with a software fallback. Static library, tests and
        # benchmarks are already forced off by the CCI recipe; `refrain-data`
        # consumes it PRIVATE so <crc32c/crc32c.h> never reaches public headers.
        self.requires("crc32c/1.1.2", options={"shared": False})

        # --- Conan-only vendoring: in-repo recipes under conan/recipes/.
        # Run `python3 tools/vendor_export.py --build` once before `conan install`.
        if self.options.with_wasm:
            self.requires("wasmtime/50.0.1")
        if self.options.with_crashpad:
            self.requires("crashpad/2026.10.26")

        # --- client-only stack (deferred). Server-first builds skip it.
        if self.options.client:
            self.requires("steam-audio/4.8.1")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["REFRAIN_LTO"] = "ON" if self.options.lto else "OFF"
        tc.variables["REFRAIN_BUILD_CLIENT"] = "ON" if self.options.client else "OFF"
        tc.variables["REFRAIN_WITH_CRASHPAD"] = (
            "ON" if self.options.with_crashpad else "OFF"
        )
        tc.variables["REFRAIN_WITH_WASM"] = "ON" if self.options.with_wasm else "OFF"
        tc.variables["REFRAIN_USE_REFLECTION"] = (
            "ON" if self.options.use_reflection else "OFF"
        )
        tc.variables["REFRAIN_WITH_MIMALLOC"] = (
            "ON" if self.options.with_mimalloc else "OFF"
        )
        tc.variables["REFRAIN_MIMALLOC_OVERRIDE_NEW"] = (
            "ON" if self.options.mimalloc_override_new else "OFF"
        )
        # liburing is Linux-only, so the CMake option mirrors the requirement
        # gate above instead of the raw user option.
        tc.variables["REFRAIN_WITH_IO_URING"] = (
            "ON"
            if self.options.with_io_uring and self.settings.os == "Linux"
            else "OFF"
        )
        tc.variables["REFRAIN_BUILD_TESTS"] = (
            "ON" if self.options.build_tests else "OFF"
        )
        tc.variables["REFRAIN_BUILD_BENCH"] = (
            "ON" if self.options.build_bench else "OFF"
        )

        # Conan 2's Git.run() expects a *string* command. Passing a list runs
        # `git ['symbolic-ref', ...]`, which fails; the ConanException must be
        # handled explicitly so branch/commit do not silently become empty in
        # every build. Detached HEAD is handled by `rev-parse` (works without a
        # branch); failures are reported as a warning instead of silently
        # degrading the version string.
        git = Git(self)
        branch = ""
        commit = ""
        try:
            branch = (git.run("symbolic-ref --short -q HEAD") or "").strip()
        except ConanException:
            branch = ""
        if not branch:
            # A detached checkout has no symbolic HEAD. CI still names the
            # source branch in the environment, so the version string keeps
            # the [branch@commit] metadata crash symbolication relies on.
            branch = (
                os.environ.get("GITHUB_HEAD_REF")
                or os.environ.get("GITHUB_REF_NAME")
                or ""
            ).strip()
        try:
            commit = (git.run("rev-parse --short HEAD") or "").strip()
        except ConanException:
            commit = ""
        if not commit:
            self.output.warning(
                "refrain: could not detect the git commit; the version string "
                "will omit [branch@commit] (crash symbolication relies on it)"
            )

        # Git version metadata must be cache variables (FORCE) rather than
        # plain toolchain variables: `tc.variables` does not override an
        # existing CMakeCache entry, so a rebuild would keep the stale empty
        # branch/commit. `cache_variables` forces the fresh values in.
        tc.cache_variables["REFRAIN_VERSION"] = self.version
        tc.cache_variables["REFRAIN_BRANCH"] = branch
        tc.cache_variables["REFRAIN_COMMIT"] = commit
        tc.generate()

        CMakeDeps(self).generate()

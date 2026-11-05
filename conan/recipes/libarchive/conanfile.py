# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for libarchive.
#
# ConanCenter ships libarchive/3.8.7 while upstream 3.8.11 is a security and
# bugfix release and the current upstream release. Refrain uses libarchive as
# the `.rfmod` tar read side (ustar/PAX/GNU), so a security release must not
# lag behind a CCI snapshot that predates it; the freshness policy therefore
# requires vendoring the current upstream release in-repo.
#
# The recipe builds the official release tarball (not the GitHub-generated
# archive: upstream publishes the Autotools/CMake `make dist` tarball with a
# stable sha256) and packages the minimal Refrain profile:
#
#   * tar format support is always compiled into libarchive (`ENABLE_TAR` only
#     controls the bsdtar CLI, which we do not ship);
#   * every compression filter (zlib/bzip2/lzma/lz4/lzo/zstd), crypto backend
#     (openssl/mbedtls/nettle/libb2/md) and XML/PCRE/ACL/xattr/iconv feature is
#     forced OFF, so the build never picks up an ambient system library;
#   * `.rfmod` decompression stays with the project's own bounded zstd stage
#     (avoids double decompression and keeps the size caps in one place).
#
# The CMake surface mirrors ConanCenter so the consumer stays unchanged:
# `find_package(LibArchive CONFIG REQUIRED)` -> `LibArchive::LibArchive`
# (ConanDeps generates LibArchiveConfig.cmake from `package_info`; upstream
# itself installs no CMake package config).
#
# Upgrade procedure:
#   1. https://github.com/libarchive/libarchive/releases -> new tag
#   2. bump `version` + `_url` + `_sha256` (sha256 of the official tarball)
#   3. python3 tools/vendor_export.py --build --only libarchive
#   4. refresh conan/locks/vendor.lock and run the `.rfmod` tar-read tests
#      (ustar / PAX / GNU fixtures) plus `refrain-tools mods verify`

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import collect_libs, copy, get, rmdir


class LibarchiveConan(ConanFile):
    name = "libarchive"
    version = "3.8.11"
    description = (
        "Multi-format archive and compression library (minimal tar-read "
        "profile: ustar/PAX/GNU, no compression filters)"
    )
    license = "BSD-2-Clause"
    author = "Refrain third-party vendoring"
    url = "https://github.com/libarchive/libarchive"
    homepage = "https://libarchive.org"
    topics = ("archive", "compression", "tar", "ustar", "pax", "gnu")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        # Static-only package: the option exists so a global `*:shared=False`
        # pattern keeps working; a shared build is deliberately not offered.
        "shared": [False],
        "fPIC": [True, False],
        # ConanCenter-compatible capability switches. Refrain pins the minimal
        # tar-read profile (all False, see `validate`); they are declared so
        # the consumer's explicit option dict keeps expressing "no filters".
        "with_acl": [True, False],
        "with_xattr": [True, False],
        "with_zlib": [True, False],
        "with_bzip2": [True, False],
        "with_lzma": [True, False],
        "with_lz4": [True, False],
        "with_lzo": [True, False],
        "with_zstd": [True, False],
        "with_openssl": [True, False],
        "with_mbedtls": [True, False],
        "with_nettle": [True, False],
        "with_libb2": [True, False],
        "with_libxml2": [True, False],
        "with_expat": [True, False],
        "with_iconv": [True, False],
        "with_pcre2": [True, False],
        "with_pcreposix": [True, False],
        "with_cng": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
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
    }

    # Official upstream release tarball (the `make dist` artifact published on
    # the release page), sha256-pinned.
    _url = (
        "https://github.com/libarchive/libarchive/releases/download/"
        "v3.8.11/libarchive-3.8.11.tar.gz"
    )
    _sha256 = "900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b"

    # Capability switches that the minimal package does not build. Keeping them
    # rejected (instead of silently ignoring them) makes a future "enable gzip
    # tar" request fail loudly rather than produce a wrong package.
    _minimal_only_options = (
        "with_acl",
        "with_xattr",
        "with_zlib",
        "with_bzip2",
        "with_lzma",
        "with_lz4",
        "with_lzo",
        "with_zstd",
        "with_openssl",
        "with_mbedtls",
        "with_nettle",
        "with_libb2",
        "with_libxml2",
        "with_expat",
        "with_iconv",
        "with_pcre2",
        "with_pcreposix",
        "with_cng",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        # libarchive is C: keep cppstd/libcxx out of the package_id/revision.
        self.settings.rm_safe("compiler.cppstd")
        self.settings.rm_safe("compiler.libcxx")

    def validate(self):
        unsupported = [
            option
            for option in self._minimal_only_options
            if self.options.get_safe(option)
        ]
        if unsupported:
            raise ConanInvalidConfiguration(
                "libarchive in-repo recipe packages only Refrain's minimal "
                "tar-read profile (tar format without compression filters / "
                "crypto / XML / PCRE / ACL / xattr / iconv). Unsupported "
                "option(s) enabled: " + ", ".join(unsupported)
            )

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"] = "OFF"
        # Never let upstream's auto-detection pick up an ambient system
        # library (the default for most of these options is ON).
        tc.variables["ENABLE_ZLIB"] = "OFF"
        tc.variables["ENABLE_BZip2"] = "OFF"
        tc.variables["ENABLE_LZMA"] = "OFF"
        tc.variables["ENABLE_LZ4"] = "OFF"
        tc.variables["ENABLE_LZO"] = "OFF"
        tc.variables["ENABLE_ZSTD"] = "OFF"
        tc.variables["ENABLE_OPENSSL"] = "OFF"
        tc.variables["ENABLE_MBEDTLS"] = "OFF"
        tc.variables["ENABLE_NETTLE"] = "OFF"
        tc.variables["ENABLE_LIBB2"] = "OFF"
        # FreeBSD libmd: only affects message-digest backends we do not use.
        tc.variables["ENABLE_MD"] = "OFF"
        tc.variables["ENABLE_LIBXML2"] = "OFF"
        tc.variables["ENABLE_EXPAT"] = "OFF"
        tc.variables["ENABLE_BSDXML"] = "OFF"
        tc.variables["ENABLE_WIN32_XMLLITE"] = "OFF"
        tc.variables["ENABLE_PCREPOSIX"] = "OFF"
        tc.variables["ENABLE_PCRE2POSIX"] = "OFF"
        tc.variables["ENABLE_CNG"] = "OFF"
        tc.variables["ENABLE_ACL"] = "OFF"
        tc.variables["ENABLE_XATTR"] = "OFF"
        tc.variables["ENABLE_ICONV"] = "OFF"
        # No bsdtar/bsdcpio/bsdcat/bsdunzip CLI, no test suite. `ENABLE_TAR`
        # only controls the CLI: the tar *format* reader/writer is always
        # compiled into libarchive.
        tc.variables["ENABLE_TAR"] = "OFF"
        tc.variables["ENABLE_CPIO"] = "OFF"
        tc.variables["ENABLE_CAT"] = "OFF"
        tc.variables["ENABLE_UNZIP"] = "OFF"
        tc.variables["ENABLE_TEST"] = "OFF"
        tc.variables["ENABLE_COVERAGE"] = "OFF"
        tc.variables["ENABLE_INSTALL"] = "ON"
        # Upstream's own -Werror gate stays off: this is third-party C code
        # built with the consumer toolchain, not first-party code.
        tc.variables["ENABLE_WERROR"] = "OFF"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "COPYING", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        # Consumers get their CMake metadata from CMakeDeps (LibArchiveConfig
        # generated from `package_info`); drop upstream's pkg-config file and
        # the man pages to keep the package minimal.
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        # Mirror the ConanCenter recipe so `find_package(LibArchive CONFIG)`
        # and the `LibArchive::LibArchive` target keep working unchanged.
        self.cpp_info.set_property("cmake_find_mode", "both")
        self.cpp_info.set_property("cmake_file_name", "LibArchive")
        self.cpp_info.set_property(
            "cmake_target_name", "LibArchive::LibArchive"
        )
        self.cpp_info.set_property("pkg_config_name", "libarchive")
        self.cpp_info.libs = collect_libs(self)
        if self.settings.compiler == "msvc":
            # archive.h uses __declspec(dllimport) unless this is defined for
            # a static build (mirrors ConanCenter).
            self.cpp_info.defines = ["LIBARCHIVE_STATIC"]

# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo drop-in recipe for zlib, implemented by zlib-ng 2.3.3 in
# `ZLIB_COMPAT=ON` mode.
#
# IMPORTANT IDENTITY NOTE: the package is called `zlib/1.3.1` and exports the
# zlib C API/ABI (`zlib.h`, `libz.a`, `ZLIB::ZLIB`, pkg-config `zlib`), but the
# implementation is zlib-ng (https://github.com/zlib-ng/zlib-ng). This lets the
# existing transitive consumers (libzip, libcurl, crashpad) pick up the faster
# implementation without vendoring them.
#
# Upgrade procedure:
#   1. https://github.com/zlib-ng/zlib-ng/releases -> new tag
#   2. bump `version` (compat level, e.g. 1.3.1) + `_url` + `_sha256`
#   3. python3 tools/vendor_export.py --build --only zlib
#   4. refresh conan/locks/vendor.lock and run the extract/crashpad smoke tests
#      plus an inflate benchmark

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import collect_libs, copy, get, rmdir


class ZlibConan(ConanFile):
    name = "zlib"
    version = "1.3.1"
    description = (
        "zlib-compatible compression library (implementation: zlib-ng 2.3.3, "
        "drop-in replacement for zlib)"
    )
    license = "Zlib"
    author = "Refrain third-party vendoring"
    url = "https://github.com/zlib-ng/zlib-ng"
    homepage = "https://github.com/zlib-ng/zlib-ng"
    topics = ("zlib", "zlib-ng", "compression", "deflate", "inflate")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    _url = "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz"
    _sha256 = "f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        # zlib API/ABI compatibility mode (zlib.h / libz / ZLIB::ZLIB).
        tc.variables["ZLIB_COMPAT"] = "ON"
        tc.variables["WITH_GZFILEOP"] = "ON"
        tc.variables["WITH_OPTIM"] = "ON"
        tc.variables["WITH_NEW_STRATEGIES"] = "ON"
        # Keep the binary portable: runtime CPU dispatch picks SIMD paths.
        tc.variables["WITH_NATIVE_INSTRUCTIONS"] = "OFF"
        tc.variables["WITH_RUNTIME_CPU_DETECTION"] = "ON"
        tc.variables["BUILD_SHARED_LIBS"] = self.options.shared
        tc.variables["BUILD_TESTING"] = "OFF"
        tc.variables["WITH_GTEST"] = "OFF"
        tc.variables["WITH_FUZZERS"] = "OFF"
        tc.variables["WITH_BENCHMARKS"] = "OFF"
        tc.variables["WITH_BENCHMARK_APPS"] = "OFF"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE.md", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "README.md", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        # The consumers get their CMake/pkg-config metadata from ConanDeps;
        # avoid leaking upstream's own package files into the package.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        # Mirror the ConanCenter zlib recipe so any consumer works unchanged.
        self.cpp_info.set_property("cmake_find_mode", "both")
        self.cpp_info.set_property("cmake_file_name", "ZLIB")
        self.cpp_info.set_property("cmake_target_name", "ZLIB::ZLIB")
        self.cpp_info.set_property("pkg_config_name", "zlib")
        self.cpp_info.libs = ["z"]

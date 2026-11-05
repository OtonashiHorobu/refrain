# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for mimalloc.
#
# ConanCenter ships mimalloc/2.1.9 (and up to 3.5.1) while upstream is at
# v3.6.0. The project pins the upstream v3 line: the C API used by the kernel
# (`mi_malloc`, `mi_malloc_aligned`, `mi_free`, `mimalloc-new-delete.h`) is
# unchanged in v3, and the CMake target names are preserved (`mimalloc-static`
# for the static library, `mimalloc` for shared).
#
# Upgrade procedure:
#   1. https://github.com/microsoft/mimalloc/releases -> new tag
#   2. bump `version` + `_url` + `_sha256` (sha256 of the GitHub archive)
#   3. python3 tools/vendor_export.py --build --only mimalloc
#   4. run refrain-tests (memory tests assert the override is live)

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import collect_libs, copy, get, rmdir


class MimallocConan(ConanFile):
    name = "mimalloc"
    version = "3.6.0"
    description = (
        "mimalloc is a compact general purpose allocator with excellent "
        "performance (upstream v3 line)"
    )
    license = "MIT"
    author = "Refrain third-party vendoring"
    url = "https://github.com/microsoft/mimalloc"
    homepage = "https://github.com/microsoft/mimalloc"
    topics = ("mimalloc", "allocator", "performance", "microsoft")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "secure": [True, False],
        "override": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "secure": False,
        "override": False,
    }

    _url = "https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.0.tar.gz"
    _sha256 = "c663101ba193b5b823c8541c1b719ab67e3ebf9976e425a02359a5cad5e52990"

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
        tc.variables["MI_BUILD_TESTS"] = "OFF"
        tc.variables["MI_BUILD_SHARED"] = self.options.shared
        tc.variables["MI_BUILD_STATIC"] = not self.options.shared
        tc.variables["MI_BUILD_OBJECT"] = "OFF"
        tc.variables["MI_OVERRIDE"] = "ON" if self.options.override else "OFF"
        tc.variables["MI_SECURE"] = "ON" if self.options.secure else "OFF"
        tc.variables["MI_INSTALL_TOPLEVEL"] = "ON"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "mimalloc")
        self.cpp_info.set_property(
            "cmake_target_name",
            "mimalloc" if self.options.shared else "mimalloc-static",
        )
        self.cpp_info.libs = collect_libs(self)
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.extend(["pthread", "rt"])
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs.extend(
                ["psapi", "shell32", "user32", "bcrypt"]
            )

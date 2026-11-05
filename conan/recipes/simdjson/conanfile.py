# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for simdjson.
#
# ConanCenter ships simdjson/5.0.5 while upstream is at v5.1.0. The project
# uses both the DOM/ondemand parser and the builder; the 5.x API is stable.
# Options and CMake/CMakeDeps properties mirror the ConanCenter recipe
# (`simdjson::simdjson`, pkg-config `simdjson`).
#
# Upgrade procedure:
#   1. https://github.com/simdjson/simdjson/releases -> new tag
#   2. bump `version` + `_url` + `_sha256`
#   3. python3 tools/vendor_export.py --build --only simdjson
#   4. run refrain-tests / refrain-tools extract smoke

import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir
from conan.tools.microsoft import is_msvc


class SimdjsonConan(ConanFile):
    name = "simdjson"
    version = "5.1.0"
    description = "Parsing gigabytes of JSON per second"
    license = ("Apache-2.0", "MIT")
    author = "Refrain third-party vendoring"
    url = "https://github.com/simdjson/simdjson"
    homepage = "https://github.com/simdjson/simdjson"
    topics = ("json", "parser", "simd", "format")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "threads": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "threads": True,
    }

    _url = "https://github.com/simdjson/simdjson/archive/refs/tags/v5.1.0.tar.gz"
    _sha256 = "f413941e66142ade07adfca7c277f092f020560ea971db33414d95170afe93c4"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate_build(self):
        check_min_cppstd(self, 17)

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["SIMDJSON_ENABLE_THREADS"] = self.options.threads
        tc.cache_variables["SIMDJSON_DEVELOPER_MODE"] = False
        cppstd = str(self.settings.compiler.cppstd).replace("gnu", "")
        tc.cache_variables["SIMDJSON_CXX_STANDARD"] = cppstd
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "LICENSE-MIT", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "simdjson")
        self.cpp_info.set_property("cmake_target_name", "simdjson::simdjson")
        self.cpp_info.set_property("pkg_config_name", "simdjson")
        self.cpp_info.libs = ["simdjson"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["m"]
        if self.options.threads:
            self.cpp_info.defines = ["SIMDJSON_THREADS_ENABLED=1"]
            if self.settings.os in ("Linux", "FreeBSD"):
                self.cpp_info.system_libs.append("pthread")
        if self.options.shared:
            self.cpp_info.defines.append("SIMDJSON_USING_LIBRARY=1")
            if is_msvc(self):
                self.cpp_info.defines.append(
                    "SIMDJSON_USING_WINDOWS_DYNAMIC_LIBRARY=1"
                )

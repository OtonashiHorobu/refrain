# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for Google Benchmark.
#
# ConanCenter ships benchmark/1.9.5 while upstream is at v1.10.0.
# CMake target names/components are mirrored from the ConanCenter recipe:
# `benchmark::benchmark` and `benchmark::benchmark_main` (the project links the
# latter for `refrain-bench`).
#
# Upgrade procedure:
#   1. https://github.com/google/benchmark/releases -> new tag
#   2. bump `version` + `_url` + `_sha256`
#   3. python3 tools/vendor_export.py --build --only benchmark
#   4. build refrain-bench and run it once

import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, replace_in_file, rmdir
from conan.tools.microsoft import is_msvc, check_min_vs


class BenchmarkConan(ConanFile):
    name = "benchmark"
    version = "1.10.0"
    description = "A microbenchmark support library (upstream 1.10.0)"
    license = "Apache-2.0"
    author = "Refrain third-party vendoring"
    url = "https://github.com/google/benchmark"
    homepage = "https://github.com/google/benchmark"
    topics = ("google", "microbenchmark")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "enable_lto": [True, False],
        "enable_exceptions": [True, False],
        "enable_libpfm": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "enable_lto": False,
        "enable_exceptions": True,
        "enable_libpfm": False,
    }

    _url = "https://github.com/google/benchmark/archive/refs/tags/v1.10.0.tar.gz"
    _sha256 = "acc5c7c0fbed855c4aaa2d7c3c36fb82b0c15c28f4e151e3f679c2fc0aa404af"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
        if self.settings.os != "Linux":
            del self.options.enable_libpfm

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, 17)
        check_min_vs(self, "190")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)
        # Do not let upstream force a C++ standard on the consumer build.
        replace_in_file(
            self,
            os.path.join(self.source_folder, "CMakeLists.txt"),
            "set(CMAKE_CXX_STANDARD",
            "#",
        )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BENCHMARK_ENABLE_TESTING"] = "OFF"
        tc.variables["BENCHMARK_ENABLE_GTEST_TESTS"] = "OFF"
        tc.variables["BENCHMARK_ENABLE_LTO"] = self.options.enable_lto
        tc.variables["BENCHMARK_ENABLE_EXCEPTIONS"] = (
            self.options.enable_exceptions
        )
        tc.variables["BENCHMARK_ENABLE_LIBPFM"] = self.options.get_safe(
            "enable_libpfm", False
        )
        tc.variables["BENCHMARK_ENABLE_WERROR"] = False
        tc.variables["BENCHMARK_FORCE_WERROR"] = False
        tc.variables["BENCHMARK_USE_LIBCXX"] = (
            self.settings.compiler.get_safe("libcxx") == "libc++"
            if self.settings.os != "Windows"
            else False
        )
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
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "benchmark")
        self.cpp_info.set_property("pkg_config_name", "benchmark")

        benchmark = self.cpp_info.components["_benchmark"]
        benchmark.set_property("cmake_target_name", "benchmark::benchmark")
        benchmark.libs = ["benchmark"]
        if not self.options.shared:
            benchmark.defines.append("BENCHMARK_STATIC_DEFINE")
        if self.settings.os in ("FreeBSD", "Linux"):
            benchmark.system_libs.extend(["pthread", "rt", "m"])
        elif self.settings.os == "Windows":
            benchmark.system_libs.append("shlwapi")
        elif self.settings.os == "SunOS":
            benchmark.system_libs.append("kstat")
        if self.options.get_safe("enable_libpfm"):
            benchmark.requires.append("libpfm4::libpfm4")

        main = self.cpp_info.components["benchmark_main"]
        main.set_property("cmake_target_name", "benchmark::benchmark_main")
        main.libs = ["benchmark_main"]
        main.requires = ["_benchmark"]

        # Workaround so CMakeDeps emits every component for downstream recipes.
        self.cpp_info.set_property(
            "cmake_target_name", "benchmark::benchmark_main"
        )
        if is_msvc(self):
            self.cpp_info.components["_benchmark"].system_libs.append(
                "shlwapi"
            )

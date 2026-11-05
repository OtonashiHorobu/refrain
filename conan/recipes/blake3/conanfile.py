# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for BLAKE3.
#
# ConanCenter ships blake3/1.8.5 while upstream is at 1.8.7. The project uses
# BLAKE3 as the content hash for `.rfmod`, the content-addressed store and the
# builtin RFSI image, so the freshness policy requires vendoring the current
# upstream release. The build follows the ConanCenter recipe: compile the
# official C sources from the `c/` subdirectory (static, no TBB) and expose the
# upstream `BLAKE3::blake3` CMake target.
#
# Upgrade procedure:
#   1. https://github.com/BLAKE3-team/BLAKE3/releases -> new tag
#   2. bump `version` + `_url` + `_sha256`
#   3. python3 tools/vendor_export.py --build --only blake3
#   4. refresh conan/locks/vendor.lock and run scripts/ci.sh

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir


class Blake3Conan(ConanFile):
    name = "blake3"
    version = "1.8.7"
    description = (
        "BLAKE3 cryptographic hash function (much faster than MD5/SHA-1/"
        "SHA-2/SHA-3/BLAKE2)"
    )
    license = "CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception"
    author = "Refrain third-party vendoring"
    url = "https://github.com/BLAKE3-team/BLAKE3"
    homepage = "https://github.com/BLAKE3-team/BLAKE3"
    topics = ("blake3", "hash", "crypto", "cryptography")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tbb": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tbb": False,
    }

    # GitHub-generated source archive for 1.8.7 (stable bytes, sha256-pinned).
    _url = "https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.7.tar.gz"
    _sha256 = "c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        if not self.options.with_tbb:
            self.settings.rm_safe("compiler.cppstd")
            self.settings.rm_safe("compiler.libcxx")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def requirements(self):
        if self.options.with_tbb:
            self.requires("onetbb/[>=2021.12.0 <2024.0.0]")

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["BLAKE3_USE_TBB"] = self.options.with_tbb
        tc.cache_variables["BLAKE3_FETCH_TBB"] = False
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder=os.path.join(self.source_folder, "c"))
        cmake.build()

    def package(self):
        for license_file in ("LICENSE_A2", "LICENSE_A2LLVM", "LICENSE_CC0"):
            copy(self, license_file, src=self.source_folder,
                 dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "blake3")
        self.cpp_info.set_property("cmake_target_name", "BLAKE3::blake3")
        self.cpp_info.set_property("pkg_config_name", "libblake3")
        self.cpp_info.libs = ["blake3"]
        if self.options.with_tbb:
            self.cpp_info.requires = ["onetbb::libtbb"]
            self.cpp_info.defines.append("BLAKE3_USE_TBB")
        if self.settings.os == "Windows" and self.options.shared:
            self.cpp_info.defines.append("BLAKE3_DLL")

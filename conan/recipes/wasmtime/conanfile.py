# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for Wasmtime (WebAssembly runtime).
#
# ConanCenter only ships wasmtime up to 37.0.0 and has no Linux binaries,
# so a source build there would require a Rust toolchain.  This recipe instead
# repackages the official prebuilt C API release artifacts (headers +
# static/shared library), which is the same artifact the Wasmtime project
# consumes from its C API.
#
# Upgrade procedure:
#   1. https://github.com/bytecodealliance/wasmtime/releases -> new tag
#   2. bump `version`
#   3. refresh the `_artifacts` table with the new asset sha256 digests
#   4. `python tools/vendor_export.py --build` then run the consumer smoke test

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class WasmtimeConan(ConanFile):
    name = "wasmtime"
    version = "50.0.1"
    description = (
        "Standalone JIT-style runtime for WebAssembly, using Cranelift "
        "(official prebuilt C API)"
    )
    license = "Apache-2.0 WITH LLVM-exception"
    author = "Refrain third-party vendoring"
    url = "https://github.com/bytecodealliance/wasmtime"
    homepage = "https://github.com/bytecodealliance/wasmtime"
    topics = ("webassembly", "wasm", "wasi", "wasmtime", "cranelift")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        # "min" artifact: C API built with a reduced feature set (no component
        # model / profiling etc.), smaller binary, see the release CI manifests.
        "minimal": [True, False],
        # Linux libc flavour of the prebuilt artifact.
        "libc": ["glibc", "musl"],
    }
    default_options = {
        "shared": True,
        "minimal": False,
        "libc": "glibc",
    }

    # Official GitHub release assets, pinned by sha256 (the x86_64-linux asset
    # is verified; other platforms are taken from the GitHub release API
    # `digest` field and not yet tested on Windows/macOS hardware).
    _artifacts = {
        ("Linux", "x86_64", "glibc"): (
            "x86_64-linux-c-api.tar.xz",
            "e2c0c26e1aef52f82f0b9918ad6f34cc30f4b557aee3e261cb127a9462be8f4d",
        ),
        ("Linux", "x86_64", "musl"): (
            "x86_64-musl-c-api.tar.xz",
            "579216d5979f3b2f0714c039e097de13f0e6af62bbaa96588a0de1b7de9c20cb",
        ),
        ("Linux", "armv8", "glibc"): (
            "aarch64-linux-c-api.tar.xz",
            "1a36140a016a9cc098f02a37bb0cc7be6451032861ba70049127928d2f31ef84",
        ),
        ("Linux", "armv8", "musl"): (
            "aarch64-musl-c-api.tar.xz",
            "3f9f1875a83a3764987e34138db364d149d75b573c3e725559b779ba781f0e2f",
        ),
        ("Linux", "x86", "glibc"): (
            "i686-linux-c-api.tar.xz",
            "6df3ae7df921adf423d84ee38690c23d26765b5167b9861eb58a8392c2498f6c",
        ),
        ("Linux", "armv7", "glibc"): (
            "armv7-linux-c-api.tar.xz",
            "cbd4597650bdc03b860a6dbfaac361d59a0bb35f05b3dcb0b92bdb5759792359",
        ),
        ("Linux", "riscv64", "glibc"): (
            "riscv64gc-linux-c-api.tar.xz",
            "c570299544dc867a9b489565e34cfd83ec37de0f2554dbbf191a0ddb7107912f",
        ),
        ("Linux", "s390x", "glibc"): (
            "s390x-linux-c-api.tar.xz",
            "516f5c4d454e6abaa93e5eeb8f962c903fb41d2ec3f4c7cde5911abb102cb2f3",
        ),
        ("Android", "x86_64", None): (
            "x86_64-android-c-api.tar.xz",
            "637f68d80b613f476a7d367d81333fba494db3696d9258a5f6647b257101fbea",
        ),
        ("Android", "armv8", None): (
            "aarch64-android-c-api.tar.xz",
            "611b6cd99939767684f48bd4cf3db559db4fde4992c949057be1e815ec2a9e70",
        ),
        ("Windows", "x86_64", None): (
            "x86_64-windows-c-api.zip",
            "8b461d9dc12b076d0414b1023433bbb2dc08d3b1c0f0a156853ede35f0147802",
        ),
        ("Windows", "x86", None): (
            "i686-windows-c-api.zip",
            "62cc765a9173beb5d01416d0ed1f8968330f6dec8b39e305d8e62b7a1034ae63",
        ),
        ("Windows", "armv8", None): (
            "aarch64-windows-c-api.zip",
            "84580636d4896df45d6a21d8c60ad9cf702d3934c6c70d98d49c21eba0470253",
        ),
        ("Windows", "x86_64-mingw", None): (
            "x86_64-mingw-c-api.zip",
            "9001a5df99305f6bc4a22d1e38f020f953b26cc88755840743dfabd44d9eaea4",
        ),
        ("Macos", "x86_64", None): (
            "x86_64-macos-c-api.tar.xz",
            "b6d436cba5c0964785c6ff7c9b5a743131555a0227b4f742185c4fb520c502c5",
        ),
        ("Macos", "armv8", None): (
            "aarch64-macos-c-api.tar.xz",
            "a49327ca596fae51cd92e230265132112bb7896c5a24e4d58cd05b56612e26bb",
        ),
    }

    def config_options(self):
        if self.settings.os != "Linux":
            del self.options.libc

    @property
    def _asset(self):
        os_ = str(self.settings.os)
        arch = str(self.settings.arch)
        if os_ == "Windows" and str(self.settings.compiler) == "gcc":
            arch = "x86_64-mingw"
        libc = str(self.options.libc) if os_ == "Linux" else None
        key = (os_, arch, libc) if os_ == "Linux" else (os_, arch, None)
        try:
            return self._artifacts[key]
        except KeyError:
            raise ConanInvalidConfiguration(
                f"wasmtime/{self.version}: no pinned prebuilt artifact for {key}. "
                "Add it to _artifacts before using this platform."
            )

    def configure(self):
        # Prebuilt C ABI library: it is compiler-independent. Keep the C++
        # stdlib requirement out of the package id so one binary serves all
        # consumers; the C++ wrapper header requires C++17.
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def package_id(self):
        # Artifacts only depend on platform (os/arch/libc) and options.
        del self.info.settings.compiler
        del self.info.settings.build_type

    @property
    def _artifact_dir(self):
        return os.path.join(self.build_folder, "artifact")

    def build(self):
        # `source()` cannot access settings in Conan 2 (one source folder is
        # shared by every configuration), so the platform-specific artifact is
        # fetched here and only repackaged. No compilation happens.
        name, sha256 = self._asset
        url = (
            f"https://github.com/bytecodealliance/wasmtime/releases/download/"
            f"v{self.version}/wasmtime-v{self.version}-{name}"
        )
        get(self, url=url, sha256=sha256, destination=self._artifact_dir,
            strip_root=True)

    def _variant_lib_dir(self):
        base = self._artifact_dir
        return os.path.join(base, "min", "lib") if self.options.minimal \
            else os.path.join(base, "lib")

    def package(self):
        copy(self, "LICENSE", src=self._artifact_dir,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "*.h", src=os.path.join(self._artifact_dir, "include"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.hh", src=os.path.join(self._artifact_dir, "include"),
             dst=os.path.join(self.package_folder, "include"))
        libdir = os.path.join(self.package_folder, "lib")
        bindir = os.path.join(self.package_folder, "bin")
        src_lib = self._variant_lib_dir()
        if self.options.shared:
            copy(self, "*.so*", src=src_lib, dst=libdir, keep_path=False)
            copy(self, "*.dylib", src=src_lib, dst=libdir, keep_path=False)
            copy(self, "*.dll", src=src_lib, dst=bindir, keep_path=False)
            copy(self, "*.lib", src=src_lib, dst=libdir, keep_path=False)
        else:
            copy(self, "*.a", src=src_lib, dst=libdir, keep_path=False)
            copy(self, "*.lib", src=src_lib, dst=libdir, keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "wasmtime")
        self.cpp_info.set_property("cmake_target_name", "wasmtime::wasmtime")
        self.cpp_info.libs = ["wasmtime"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs = ["pthread", "dl", "m"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32", "userenv", "bcrypt", "ntdll"]
        elif self.settings.os == "Macos":
            self.cpp_info.frameworks = ["Security", "SystemConfiguration"]

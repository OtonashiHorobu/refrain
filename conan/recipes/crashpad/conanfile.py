# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for Crashpad (crash reporting), building the
# getsentry/crashpad fork whose in-tree CMake support works on
# Linux/Windows/macOS.  Upstream chromium/crashpad only ships GN
# build files and needs the full Chromium toolchain; ConanCenter's
# crashpad/cci.20220219 is a frozen snapshot and has no GCC 16 binaries.
#
# The recipe pins the crashpad commit and its three git submodules
# (mini_chromium, chromium-zlib, chromium-linux-syscall-support) by commit
# SHA-256 of the GitHub archive tarballs; no git submodule checkout is done at
# build time (vendor policy forbids submodules).
#
# Upgrade procedure:
#   1. pick a new getsentry/crashpad default-branch commit
#   2. read its .gitmodules / gitlinks for the submodule commits
#   3. refresh `_sources` (url + sha256) and `version`
#   4. python tools/vendor_export.py --build, then the crash smoke test

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get


class CrashpadConan(ConanFile):
    name = "crashpad"
    version = "2026.10.26"
    description = (
        "Crashpad crash-reporting client + handler (getsentry fork, CMake build) "
        "used for local minidump capture on Linux and Windows"
    )
    license = "Apache-2.0"
    author = "Refrain third-party vendoring"
    url = "https://github.com/getsentry/crashpad"
    homepage = "https://github.com/getsentry/crashpad"
    topics = ("crash-reporting", "minidump", "breakpad", "crashpad")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "fPIC": [True, False],
        # "libcurl": Conan libcurl (minimal HTTP transport for the handler)
        # "system":  use the system libcurl found by CMake (not reproducible)
        "http_transport": ["libcurl", "system"],
    }
    default_options = {
        "fPIC": True,
        "http_transport": "libcurl",
    }

    exports = "refrain-crashpad.cmake"

    # Upstream sources, pinned commit + tarball sha256.
    _crashpad_sha = "db81331bc0dd5904cbf84de59f96e87e8366f182"
    _crashpad_arc_sha = "a72c357c0be734545bfb9d14a0ceaa62f344b4b713e56d1ac13789ee2de5ee90"
    _submodules = {
        "mini_chromium": (
            "third_party/mini_chromium/mini_chromium",
            "2f5c168fa462cf3836c4a7801b4f4955496d975d",
            "00f60d55f810dd66a64cf882cb8f4d241809e9e14c24baaaa96031b66ca79ac4",
        ),
        "zlib": (
            "third_party/zlib/zlib",
            "fef58692c1d7bec94c4ed3d030a45a1832a9615d",
            "d49b88d645b17ccbad19406f95cd495527784b104b8fba1347a2475b08346224",
        ),
        "lss": (
            "third_party/lss/lss",
            "9719c1e1e676814c456b55f5f070eabad6709d31",
            "a858367f0de0fd22a22172c562d750c29edb43f586cb6ff94be1941b77656bf3",
        ),
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def requirements(self):
        # The in-repo zlib recipe is a zlib-ng 2.3.3 drop-in
        # (zlib/1.3.1); a version range (same shape libzip/libcurl use) lets the
        # graph unify on it instead of pinning the ConanCenter zlib.
        self.requires("zlib/[>=1.2.11 <2]")
        if self.options.http_transport == "libcurl":
            # Deliberately minimal libcurl: no TLS/zstd/brotli/http2 so the
            # handler-only HTTP transport builds quickly and has no heavy
            # transitive graph.  Re-enable with_ssl when server-side upload over
            # TLS is enabled.
            self.requires("libcurl/8.23.0", options={
                "with_ssl": False,
                "with_zlib": False,
                "with_brotli": False,
                "with_zstd": False,
                "with_nghttp2": False,
                "with_libssh2": False,
                "with_ldap": False,
                "with_libidn": False,
                "with_libgsasl": False,
                "with_libpsl": False,
                "with_c_ares": False,
                "with_file": False,
                "with_ftp": False,
                "with_dict": False,
                "with_gopher": False,
                "with_imap": False,
                "with_mqtt": False,
                "with_pop3": False,
                "with_rtsp": False,
                "with_smtp": False,
                "with_telnet": False,
                "with_tftp": False,
                "with_ntlm": False,
                "with_cookies": False,
                "build_executable": False,
            })

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self,
            url=f"https://github.com/getsentry/crashpad/archive/{self._crashpad_sha}.tar.gz",
            sha256=self._crashpad_arc_sha,
            destination=self.source_folder, strip_root=True)
        for name, (subdir, sha, arc_sha) in self._submodules.items():
            get(self,
                url=f"https://github.com/getsentry/{self._repo_name(name)}/archive/{sha}.tar.gz",
                sha256=arc_sha,
                destination=os.path.join(self.source_folder, subdir),
                strip_root=True)

    @staticmethod
    def _repo_name(submodule):
        return {
            "mini_chromium": "mini_chromium",
            "zlib": "chromium-zlib",
            "lss": "chromium-linux-syscall-support",
        }[submodule]

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["CRASHPAD_ENABLE_INSTALL"] = "ON"
        tc.variables["CRASHPAD_ENABLE_INSTALL_DEV"] = "ON"
        tc.variables["CRASHPAD_ZLIB_SYSTEM"] = "ON"
        tc.variables["CRASHPAD_ENABLE_STACKTRACE"] = "OFF"
        tc.cache_variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(self.options.get_safe("fPIC", True))
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

        # The crashpad_handler executable is built but not covered by the
        # upstream install rules; ship it next to the client library.
        handler = None
        for candidate in (
            os.path.join(self.build_folder, "handler", "crashpad_handler"),
            os.path.join(self.build_folder, "handler", "crashpad_handler.exe"),
            os.path.join(self.build_folder, "handler", str(self.settings.build_type),
                         "crashpad_handler"),
            os.path.join(self.build_folder, "handler", str(self.settings.build_type),
                         "crashpad_handler.exe"),
        ):
            if os.path.isfile(candidate):
                handler = candidate
                break
        if handler is None:
            raise RuntimeError("crashpad_handler executable not found in build tree")
        copy(self, os.path.basename(handler), src=os.path.dirname(handler),
             dst=os.path.join(self.package_folder, "bin"))

        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "refrain-crashpad.cmake", src=self.recipe_folder,
             dst=os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "crashpad")
        # Installed upstream layout: include/crashpad/{client,util,mini_chromium}
        self.cpp_info.includedirs = ["include/crashpad", "include/crashpad/mini_chromium"]
        self.cpp_info.set_property(
            "cmake_build_modules", [os.path.join("lib", "cmake", "refrain-crashpad.cmake")])

        mini = self.cpp_info.components["mini_chromium"]
        mini.libs = ["mini_chromium"]
        mini.set_property("cmake_target_name", "crashpad::mini_chromium")
        mini.includedirs = ["include/crashpad/mini_chromium"]
        if self.settings.os == "Linux":
            mini.system_libs = ["pthread", "dl"]

        compat = self.cpp_info.components["compat"]
        compat.libs = ["crashpad_compat"]
        compat.set_property("cmake_target_name", "crashpad::compat")

        zlib = self.cpp_info.components["zlib"]
        zlib.set_property("cmake_target_name", "crashpad::zlib")
        zlib.requires = ["zlib::zlib"]

        mpack = self.cpp_info.components["mpack"]
        mpack.libs = ["crashpad_mpack"]
        mpack.set_property("cmake_target_name", "crashpad::mpack")

        util = self.cpp_info.components["util"]
        util.libs = ["crashpad_util"]
        util.set_property("cmake_target_name", "crashpad::util")
        util.requires = ["compat", "mpack", "zlib", "mini_chromium"]
        if self.settings.os == "Linux":
            util.requires.append("libcurl::libcurl")
            util.system_libs = ["pthread", "dl", "m"]
        elif self.settings.os == "Windows":
            util.system_libs = ["dbghelp", "rpcrt4", "winhttp"]

        # Dependency directions mirror the installed crashpad-targets.cmake
        # (the fork's static-link graph; keep it acyclic for Conan components):
        #   util     -> compat, mpack, zlib, mini_chromium (+ libcurl on Linux)
        #   client   -> compat, util, mini_chromium
        #   snapshot -> client, compat, util, mini_chromium
        #   minidump -> compat, snapshot, util, mini_chromium
        # NOTE: never add minidump->snapshot and snapshot->minidump at the same
        # time: CMakeDeps rejects cyclic component requires.
        minidump = self.cpp_info.components["minidump"]
        minidump.libs = ["crashpad_minidump"]
        minidump.set_property("cmake_target_name", "crashpad::minidump")
        minidump.requires = ["compat", "snapshot", "util", "mini_chromium"]

        client = self.cpp_info.components["client"]
        client.libs = ["crashpad_client"]
        client.set_property("cmake_target_name", "crashpad::client")
        client.requires = ["compat", "util", "mini_chromium"]

        snapshot = self.cpp_info.components["snapshot"]
        snapshot.libs = ["crashpad_snapshot"]
        snapshot.set_property("cmake_target_name", "crashpad::snapshot")
        snapshot.requires = ["client", "compat", "util", "mini_chromium"]

        tools = self.cpp_info.components["tools"]
        tools.libs = ["crashpad_tools"]
        tools.set_property("cmake_target_name", "crashpad::tools")
        tools.requires = ["compat", "util", "mini_chromium"]

        handler = self.cpp_info.components["handler"]
        handler.libs = ["crashpad_handler_lib"]
        handler.set_property("cmake_target_name", "crashpad::handler")
        handler.requires = ["compat", "minidump", "snapshot", "util",
                            "mini_chromium"]

        self.cpp_info.components["handler"].bindirs = ["bin"]

        # CMakeDeps does not inherit the package-level includedirs into the
        # component targets, and the installed header layout is
        # include/crashpad/{client,util,mini_chromium,...}; expose both roots
        # on every component so that
        #   #include "client/crashpad_client.h"  and  #include "base/..."
        # resolve for consumers.
        for _component in self.cpp_info.components.values():
            _component.includedirs = ["include/crashpad",
                                      "include/crashpad/mini_chromium"]

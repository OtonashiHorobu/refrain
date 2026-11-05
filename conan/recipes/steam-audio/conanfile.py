# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for Valve Steam Audio, the project's audio backend.
# ConanCenter has no `steam-audio` recipe, so this recipe repackages the
# official prebuilt SDK release.  The prebuilt SDK ships a single shared
# library per platform (`libphonon.so` / `phonon.dll` / `libphonon.dylib`); no
# static library is published, and no compilation is performed by this recipe.
#
# Upgrade procedure:
#   1. https://github.com/ValveSoftware/steam-audio/releases -> new tag
#   2. bump `version` and the sha256 in `source()`
#   3. re-check the platform->directory map in `_platform_dir`
#   4. `python tools/vendor_export.py --build` + consumer smoke test

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class SteamAudioConan(ConanFile):
    name = "steam-audio"
    version = "4.8.1"
    description = (
        "Valve Steam Audio: HRTF binaural rendering, ambisonics, occlusion, "
        "transmission, reflections and pathing (official prebuilt SDK)"
    )
    license = "Apache-2.0"
    author = "Refrain third-party vendoring"
    url = "https://github.com/ValveSoftware/steam-audio"
    homepage = "https://valvesoftware.github.io/steam-audio/"
    topics = ("audio", "hrtf", "spatial-audio", "occlusion", "ray-tracing")
    package_type = "shared-library"
    settings = "os", "arch", "compiler", "build_type"

    # Ship the Apache-2.0 license text with the recipe so the exported recipe
    # is self-contained (Conan only exports files listed here).
    exports = "LICENSE"

    options = {
        # The SDK is distributed as a shared library only; the option exists so
        # that a global `*:shared=True` pattern keeps working.
        "shared": [True],
        # Windows only: package the optional GPU ray-tracing helpers
        # (TrueAudioNext.dll / GPUUtilities.dll) used by the reflection
        # simulator in the far-future GPU path.
        "with_gpu_raytracing": [True, False],
    }
    default_options = {
        "shared": True,
        "with_gpu_raytracing": False,
    }

    _sha256 = "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449"
    _url = (
        "https://github.com/ValveSoftware/steam-audio/releases/download/"
        "v{version}/steamaudio_{version}.zip"
    )

    _platforms = {
        ("Linux", "x86_64"): "linux-x64",
        ("Linux", "x86"): "linux-x86",
        ("Windows", "x86_64"): "windows-x64",
        ("Windows", "x86"): "windows-x86",
        ("Macos", "x86_64"): "osx",
        ("Macos", "armv8"): "osx",
        ("Android", "x86_64"): "android-x64",
        ("Android", "x86"): "android-x86",
        ("Android", "armv7"): "android-armv7",
        ("Android", "armv8"): "android-armv8",
        ("iOS", "armv8"): "ios",
    }

    def config_options(self):
        if self.settings.os != "Windows":
            del self.options.with_gpu_raytracing

    def configure(self):
        # Prebuilt C ABI shared library, compiler-independent.
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def package_id(self):
        del self.info.settings.compiler
        del self.info.settings.build_type

    @property
    def _platform_dir(self):
        key = (str(self.settings.os), str(self.settings.arch))
        try:
            return self._platforms[key]
        except KeyError:
            raise ConanInvalidConfiguration(
                f"steam-audio/{self.version}: no prebuilt SDK directory for {key}"
            )

    def source(self):
        # The single release zip contains every platform (~173 MiB); it is
        # downloaded once per machine and shared by the Conan source cache /
        # download cache (`core.sources:download_cache`).
        get(self, url=self._url.format(version=self.version), sha256=self._sha256,
            destination=self.source_folder, strip_root=True)

    def package(self):
        licenses = os.path.join(self.package_folder, "licenses")
        copy(self, "LICENSE", src=self.recipe_folder, dst=licenses)
        copy(self, "THIRDPARTY.md", src=self.source_folder, dst=licenses)
        copy(self, "*.h", src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))

        plat = self._platform_dir
        lib_src = os.path.join(self.source_folder, "lib", plat)
        lib_dst = os.path.join(self.package_folder, "lib")
        bin_dst = os.path.join(self.package_folder, "bin")
        copy(self, "*.so*", src=lib_src, dst=lib_dst, keep_path=False)
        copy(self, "*.dylib", src=lib_src, dst=lib_dst, keep_path=False)
        copy(self, "*.a", src=lib_src, dst=lib_dst, keep_path=False)
        copy(self, "*.lib", src=lib_src, dst=lib_dst, keep_path=False)
        copy(self, "*.dll", src=lib_src, dst=bin_dst, keep_path=False)
        if self.settings.os == "Macos":
            # phonon.bundle is the runtime resource bundle referenced by the SDK.
            copy(self, "*", src=os.path.join(lib_src, "phonon.bundle"),
                 dst=os.path.join(lib_dst, "phonon.bundle"), keep_path=False)
        if self.settings.os == "Windows" and self.options.with_gpu_raytracing:
            copy(self, "TrueAudioNext.dll", src=lib_src, dst=bin_dst, keep_path=False)
            copy(self, "GPUUtilities.dll", src=lib_src, dst=bin_dst, keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "steam-audio")
        self.cpp_info.set_property("cmake_target_name", "steam-audio::steam-audio")
        self.cpp_info.libs = ["phonon"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.bindirs = ["bin"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs = ["pthread", "dl"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32", "user32"]
        elif self.settings.os == "Macos":
            self.cpp_info.frameworks = ["Foundation"]

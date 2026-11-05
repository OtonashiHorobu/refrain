# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# In-repo Conan 2 recipe for Glaze.
#
# ConanCenter ships glaze/8.4.0 while upstream is at 9.1.0. The TOML backend
# (`glaze/toml.hpp`) and the generic TOML DOM path used for gamepack/plugin
# manifests improved in 9.x, so the CCI package is repackaged from the upstream
# header-only source.
#
# Upgrade procedure:
#   1. https://github.com/stephenberry/glaze/releases -> new tag
#   2. bump `version` + `_url` + `_sha256` (sha256 of the GitHub archive)
#   3. python3 tools/vendor_export.py --build --only glaze
#   4. run the consumer smoke tests (refrain-tests + refrain-tools)

from conan import ConanFile
from conan.tools.files import copy, get
import os


class GlazeConan(ConanFile):
    name = "glaze"
    version = "9.1.0"
    description = (
        "Extremely fast, in-memory JSON/TOML/BEVE/BSON/CBOR/MessagePack "
        "serialization framework (header-only)"
    )
    license = "MIT"
    author = "Refrain third-party vendoring"
    url = "https://github.com/stephenberry/glaze"
    homepage = "https://github.com/stephenberry/glaze"
    topics = ("serialization", "json", "toml", "header-only", "cpp")
    package_type = "header-library"

    # GitHub-generated source archive for v9.1.0 (stable bytes, sha256-pinned).
    _url = "https://github.com/stephenberry/glaze/archive/refs/tags/v9.1.0.tar.gz"
    _sha256 = "55f8a675397c505f9bee6c160e1c26b0085868f185d04d7a7727531fdbff4bd4"

    def source(self):
        get(self, url=self._url, sha256=self._sha256, strip_root=True)

    def package(self):
        copy(self, "include/**", src=self.source_folder,
             dst=self.package_folder)
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )
        copy(
            self,
            "README.md",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "glaze")
        self.cpp_info.set_property("cmake_target_name", "glaze::glaze")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libs = []

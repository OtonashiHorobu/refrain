#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Build a release archive for one platform.
#
# The normal build and CI stay unhardened and without LTO. This script is the
# release path: it rebuilds every Conan dependency from source with the
# hardening flags in an isolated Conan home, then links the server and tools
# with hardening and LTO enabled, strips the binaries, collects third-party
# license texts, and writes a checksum next to the archive.
#
# The isolated Conan home is required because Conan build flags are not part of
# a package id; sharing the default cache would mix hardened and regular
# binaries under the same id.
#
# Usage:
#   scripts/dist.sh [linux|mingw|msvc]
#
# Environment:
#   REFRAIN_DIST_WORK         working directory (default: build/dist/<target>)
#   REFRAIN_DIST_CONAN_HOME   isolated Conan home (default: <work>/conan-home)
#   REFRAIN_DIST_HOST_CONAN_HOME  Conan home for host tools (default: ~/.conan2)
#   REFRAIN_DIST_BUILD        CMake build directory (default: <work>/build)
#   REFRAIN_DIST_OUT          archive directory (default: dist)
#   REFRAIN_DIST_VERSION      override the version from conanfile.py
#   REFRAIN_DIST_REUSE_BUILD  keep an existing CMake build directory
#   MSVC_VERSION              Conan msvc compiler.version (default: 194)
#   MSVC_WINE_ROOT            msvc-wine installation (default: /opt/msvc)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-linux}"
case "$TARGET" in
    linux|mingw|msvc) ;;
    *)
        echo "usage: $0 [linux|mingw|msvc]" >&2
        exit 2
        ;;
esac

WORK="${REFRAIN_DIST_WORK:-$ROOT/build/dist/$TARGET}"
CONAN_HOME="${REFRAIN_DIST_CONAN_HOME:-$WORK/conan-home}"
HOST_CONAN_HOME="${REFRAIN_DIST_HOST_CONAN_HOME:-$HOME/.conan2}"
BUILD="${REFRAIN_DIST_BUILD:-$WORK/build}"
OUT="${REFRAIN_DIST_OUT:-$ROOT/dist}"
DIST_ARCH="x86_64"

log() { printf '[dist] %s\n' "$*"; }
fail() {
    printf '[dist] error: %s\n' "$*" >&2
    exit 1
}

[[ -f "$ROOT/conanfile.py" ]] || fail "run this script from a Refrain checkout"

VERSION="${REFRAIN_DIST_VERSION:-}"
if [[ -z "$VERSION" ]]; then
    VERSION="$(python3 - "$ROOT/conanfile.py" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
match = re.search(r'^    version = "([^"]+)"$', text, re.MULTILINE)
if match is None:
    raise SystemExit("no version string in conanfile.py")
print(match.group(1))
PY
)" || fail "could not read the version from conanfile.py"
fi
BRANCH="$(git -C "$ROOT" symbolic-ref --short -q HEAD || true)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"

mkdir -p "$WORK" "$CONAN_HOME" "$OUT"
export CONAN_HOME
export CONAN_NON_INTERACTIVE=1

if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
else
    JOBS="${NUMBER_OF_PROCESSORS:-4}"
fi
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) DIST_HOST=windows ;;
    *) DIST_HOST=unix ;;
esac
BUILD_PROFILE=default
if [[ "$DIST_HOST" == unix ]]; then
    BUILD_PROFILE=dist-native
    command -v gcc >/dev/null 2>&1 ||
        fail "gcc is required to build the host tools"
    NATIVE_MAJOR="$(gcc -dumpversion | cut -d. -f1)"
fi
HARDEN_CFLAGS='["-O3","-DNDEBUG","-fstack-protector-strong","-fstack-clash-protection","-fcf-protection=full","-D_FORTIFY_SOURCE=3","-ffat-lto-objects","-flto=auto"]'
HARDEN_CXXFLAGS="$HARDEN_CFLAGS"
HARDEN_LINKFLAGS='["-Wl,-z,relro","-Wl,-z,now","-flto=auto"]'

write_profile() {
    local name="$1"
    local file="$CONAN_HOME/profiles/$name"
    mkdir -p "$CONAN_HOME/profiles"
    cat > "$file"
    log "wrote Conan profile $name"
}

# Native profile used for host tools and for the Linux release.
if [[ "$DIST_HOST" == unix ]]; then
    write_profile dist-native <<PROFILE
[settings]
os=Linux
arch=$DIST_ARCH
compiler=gcc
compiler.version=$NATIVE_MAJOR
compiler.cppstd=26
compiler.libcxx=libstdc++11
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
PROFILE
fi

case "$TARGET" in
    linux)
        PROFILE=dist-linux
        write_profile "$PROFILE" <<PROFILE
[settings]
os=Linux
arch=$DIST_ARCH
compiler=gcc
compiler.version=$NATIVE_MAJOR
compiler.cppstd=26
compiler.libcxx=libstdc++11
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
tools.build:cflags=$HARDEN_CFLAGS
tools.build:cxxflags=$HARDEN_CXXFLAGS
tools.build:sharedlinkflags=$HARDEN_LINKFLAGS
tools.build:exelinkflags=$HARDEN_LINKFLAGS
PROFILE
        ;;
    mingw)
        command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 ||
            fail "x86_64-w64-mingw32-g++ is required for the mingw target"
        MINGW_MAJOR="$(x86_64-w64-mingw32-g++ -dumpversion | cut -d. -f1)"
        MINGW_CFLAGS='["-O3","-DNDEBUG","-fstack-protector-strong","-fstack-clash-protection","-fcf-protection=full","-ffat-lto-objects","-flto=auto"]'
        MINGW_LINKFLAGS='["-static","-flto=auto"]'
        PROFILE=dist-mingw
        write_profile "$PROFILE" <<PROFILE
[settings]
os=Windows
arch=$DIST_ARCH
compiler=gcc
compiler.version=$MINGW_MAJOR
compiler.cppstd=26
compiler.libcxx=libstdc++11
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
tools.build:compiler_executables={"c":"x86_64-w64-mingw32-gcc","cpp":"x86_64-w64-mingw32-g++"}
tools.build:cflags=$MINGW_CFLAGS
tools.build:cxxflags=$MINGW_CFLAGS
tools.build:sharedlinkflags=$MINGW_LINKFLAGS
tools.build:exelinkflags=$MINGW_LINKFLAGS
PROFILE
        ;;
    msvc)
        if ! command -v cl >/dev/null 2>&1; then
            MSVC_WINE_ROOT="${MSVC_WINE_ROOT:-/opt/msvc}"
            if [[ -f "$MSVC_WINE_ROOT/bin/x64/msvcenv.sh" ]]; then
                # shellcheck disable=SC1091
                source "$MSVC_WINE_ROOT/bin/x64/msvcenv.sh"
                export PATH="$MSVC_WINE_ROOT/bin/x64:$PATH"
            fi
        fi
        command -v cl >/dev/null 2>&1 ||
            fail "cl is not in PATH; run this from a Visual Studio developer prompt"
        MSVC_VERSION="${MSVC_VERSION:-194}"
        PROFILE=dist-msvc
        write_profile "$PROFILE" <<PROFILE
[settings]
os=Windows
arch=$DIST_ARCH
compiler=msvc
compiler.version=$MSVC_VERSION
compiler.cppstd=23
compiler.runtime=dynamic
compiler.runtime_type=Release
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
tools.cmake.cmaketoolchain:extra_variables={"BUILD_OSSFUZZ":"OFF","BUILD_TOOLS":"OFF","BUILD_REGRESS":"OFF","BUILD_EXAMPLES":"OFF","BUILD_DOC":"OFF"}
PROFILE
        ;;
esac

python3 tools/vendor_export.py --profile "$PROFILE" >/dev/null

common_install=(
    conan install . -pr:h "$PROFILE" -pr:b "$BUILD_PROFILE"
    -of "$WORK/conan" --build=missing
    -o "&:with_crashpad=False" -o "&:with_wasm=False"
    -o "&:build_tests=False" -o "&:build_bench=False"
    --lockfile=conan/locks/vendor.lock
)

host_tools_path=""
build_host_tools=0
if [[ "$TARGET" == mingw ]]; then
    build_host_tools=1
elif [[ "$TARGET" == msvc ]]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) build_host_tools=0 ;;
        *) build_host_tools=1 ;;
    esac
fi
if [[ "$build_host_tools" == 1 ]]; then
    # Cross builds cannot run the target-platform schema compiler or the
    # built-in image generator. Build those tools once for the build host.
    log "building host tools"
    mkdir -p "$WORK/host"
    CONAN_HOME="$HOST_CONAN_HOME" python3 tools/vendor_export.py \
        --profile default >/dev/null
    CONAN_HOME="$HOST_CONAN_HOME" conan install . -pr:h default -pr:b default \
        -of "$WORK/host/conan" --build=missing \
        -o "&:with_crashpad=False" -o "&:with_wasm=False" \
        -o "&:build_tests=False" -o "&:build_bench=False" \
        --lockfile=conan/locks/vendor.lock >/dev/null
    host_toolchain="$(find "$WORK/host/conan" -name conan_toolchain.cmake -print -quit)"
    cmake -S . -B "$WORK/host/build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$host_toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DREFRAIN_VERSION="$VERSION" -DREFRAIN_BRANCH="$BRANCH" \
        -DREFRAIN_COMMIT="$COMMIT" \
        -DREFRAIN_WITH_CRASHPAD=OFF -DREFRAIN_WITH_WASM=OFF \
        -DREFRAIN_BUILD_SERVER=OFF -DREFRAIN_BUILD_TESTS=OFF \
        -DREFRAIN_BUILD_BENCH=OFF >/dev/null
    cmake --build "$WORK/host/build" -j "$JOBS" --target refrain-tools
    host_tools_path="$WORK/host/build/bin/refrain-tools"
fi

log "resolving $TARGET dependencies"
conan_args=("${common_install[@]}")
case "$TARGET" in
    linux|mingw) conan_args+=(-o "&:lto=True") ;;
    msvc) ;;
esac
if ! "${conan_args[@]}" --format=json >"$WORK/install.json" 2>"$WORK/install.log"; then
    tail -n 60 "$WORK/install.log" >&2 || true
    fail "conan install failed"
fi

if [[ -d "$BUILD" && "${REFRAIN_DIST_REUSE_BUILD:-0}" != 1 ]]; then
    case "$BUILD" in
        "$WORK"/*) rm -rf "$BUILD" ;;
        *) fail "refusing to remove build directory outside $WORK: $BUILD" ;;
    esac
fi
mkdir -p "$BUILD"
toolchain="$(find "$WORK/conan" -name conan_toolchain.cmake -print -quit)"
[[ -n "$toolchain" ]] || fail "Conan did not generate a toolchain"

cmake_flags=(
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    -DCMAKE_BUILD_TYPE=Release
    -DREFRAIN_VERSION="$VERSION"
    -DREFRAIN_BRANCH="$BRANCH"
    -DREFRAIN_COMMIT="$COMMIT"
    -DREFRAIN_WITH_CRASHPAD=OFF
    -DREFRAIN_WITH_WASM=OFF
    -DREFRAIN_BUILD_SERVER=ON
    -DREFRAIN_BUILD_TESTS=OFF
    -DREFRAIN_BUILD_BENCH=OFF
)
case "$TARGET" in
    linux)
        cmake_flags+=(-DREFRAIN_HARDEN=ON -DREFRAIN_HARDEN_MAX=ON -DREFRAIN_LTO=ON)
        ;;
    mingw)
        cmake_flags+=(-DREFRAIN_HARDEN=ON -DREFRAIN_HARDEN_MAX=ON -DREFRAIN_LTO=ON
                      -DCMAKE_EXE_LINKER_FLAGS=-static)
        ;;
    msvc)
        cmake_flags+=(-DREFRAIN_HARDEN=ON -DREFRAIN_HARDEN_MAX=OFF -DREFRAIN_LTO=OFF)
        ;;
esac
if [[ -n "$host_tools_path" ]]; then
    cmake_flags+=(-DREFRAIN_HOST_TOOLS="$host_tools_path")
fi

log "configuring and building"
cmake -S . -B "$BUILD" "${cmake_flags[@]}"
cmake --build "$BUILD" -j "$JOBS" --target refrain-server refrain-tools

case "$TARGET" in
    linux)
        version_line="$("$BUILD/bin/refrain-tools" version 2>/dev/null | head -n 1 || true)"
        ;;
    mingw)
        if command -v wine >/dev/null 2>&1; then
            version_line="$(WINEDEBUG=-all wine "$BUILD/bin/refrain-tools.exe" version \
                2>/dev/null | head -n 1 || true)"
        else
            log "wine is not available; skipping the version smoke check"
            version_line=""
        fi
        ;;
    msvc)
        version_line=""
        ;;
esac
if [[ -n "$version_line" && "$version_line" != *"$VERSION"* ]]; then
    fail "unexpected version string: $version_line"
fi

case "$TARGET" in
    linux) name="refrain-${VERSION}-linux-${DIST_ARCH}" ;;
    mingw) name="refrain-${VERSION}-windows-mingw-${DIST_ARCH}" ;;
    msvc) name="refrain-${VERSION}-windows-msvc-${DIST_ARCH}" ;;
esac
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/bin" "$stage/$name/gamepack" "$stage/$name/licenses"

exe_suffix=""
[[ "$TARGET" == linux ]] || exe_suffix=".exe"
cp "$BUILD/bin/refrain-server$exe_suffix" "$stage/$name/bin/"
cp "$BUILD/bin/refrain-tools$exe_suffix" "$stage/$name/bin/"
cp gamepack/gamepack.toml "$stage/$name/gamepack/"
cp -r gamepack/presets "$stage/$name/gamepack/"
cp LICENSE NOTICE.md README.md "$stage/$name/"

case "$TARGET" in
    linux)
        strip --strip-all "$stage/$name/bin/refrain-server" \
            "$stage/$name/bin/refrain-tools"
        cat > "$stage/$name/run-server.sh" <<'RUN'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/bin/refrain-server" --gamepack "$here/gamepack/gamepack.toml" "$@"
RUN
        chmod +x "$stage/$name/run-server.sh"
        ;;
    mingw)
        x86_64-w64-mingw32-strip --strip-all \
            "$stage/$name/bin/refrain-server.exe" \
            "$stage/$name/bin/refrain-tools.exe"
        cat > "$stage/$name/run-server.cmd" <<'RUN'
@echo off
"%~dp0bin\refrain-server.exe" --gamepack "%~dp0gamepack\gamepack.toml" %*
RUN
        cp -r "$ROOT/licenses/GCC-Runtime" "$ROOT/licenses/MinGW-w64" \
            "$stage/$name/licenses/"
        ;;
    msvc)
        cat > "$stage/$name/run-server.cmd" <<'RUN'
@echo off
"%~dp0bin\refrain-server.exe" --gamepack "%~dp0gamepack\gamepack.toml" %*
RUN
        msvc_redist=""
        if [[ -n "${VCToolsRedistDir:-}" ]]; then
            msvc_redist="$(printf '%s' "$VCToolsRedistDir" | sed 's|\\|/|g')"
            if command -v cygpath >/dev/null 2>&1; then
                msvc_redist="$(cygpath -u "$msvc_redist")"
            fi
        fi
        if [[ -z "$msvc_redist" || ! -d "$msvc_redist" ]]; then
            msvc_redist="${MSVC_WINE_ROOT:-/opt/msvc}/VC/Redist"
        fi
        crt_dir="$(find "$msvc_redist" -maxdepth 4 -type d -name 'Microsoft.VC*.CRT' \
            -path '*x64*' ! -path '*onecore*' 2>/dev/null | sort | tail -n 1)"
        [[ -n "$crt_dir" ]] || fail "could not locate the MSVC x64 runtime DLLs"
        for dll in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll \
                   vcruntime140_1.dll; do
            cp "$crt_dir/$dll" "$stage/$name/bin/"
        done
        cp -r "$ROOT/licenses/MSVC-Runtime" "$stage/$name/licenses/"
        ;;
esac

python3 - "$WORK/install.json" "$stage/$name/licenses" <<'PY'
import json
import os
import shutil
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
destination = sys.argv[2]
for node in data["graph"]["nodes"].values():
    if node.get("context") != "host":
        continue
    ref = node.get("ref") or ""
    if not ref or ref.startswith("refrain/"):
        continue
    package_folder = node.get("package_folder")
    if not package_folder:
        continue
    source = os.path.join(package_folder, "licenses")
    if not os.path.isdir(source):
        continue
    name = ref.split("#", 1)[0].replace("/", "-")
    target = os.path.join(destination, name)
    if os.path.isdir(target):
        continue
    shutil.copytree(source, target)
PY

archive="$OUT/$name.tar.gz"
if [[ "$TARGET" != linux ]]; then
    archive="$OUT/$name.zip"
fi
(
    cd "$stage"
    if [[ "$TARGET" == linux ]]; then
        cmake -E tar czf "$archive" -- "$name"
    else
        cmake -E tar cf "$archive" --format=zip -- "$name"
    fi
)
python3 - "$archive" <<'PY'
import hashlib
import sys

path = sys.argv[1]
digest = hashlib.sha256()
with open(path, "rb") as handle:
    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
        digest.update(chunk)
with open(path + ".sha256", "w", encoding="ascii") as handle:
    handle.write(f"{digest.hexdigest()}  {path.rsplit('/', 1)[-1]}\n")
PY

log "wrote $archive"

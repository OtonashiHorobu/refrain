#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Build a hardened, LTO'd Linux release archive.
#
# The regular build (and CI) stays unhardened and without LTO. This script is
# the release path: it builds every Conan dependency from source with the
# hardening and LTO flags in an isolated Conan home, then builds the server and
# tools with REFRAIN_HARDEN/REFRAIN_LTO enabled. The isolated home is required
# because Conan build flags are not part of the package id; sharing the default
# cache would mix hardened and regular binaries under the same id.
#
# Usage:
#   scripts/dist.sh
#   REFRAIN_DIST_OUT=/srv/artifacts scripts/dist.sh
#
# Environment:
#   REFRAIN_DIST_WORK        working directory (default: build/dist)
#   REFRAIN_DIST_CONAN_HOME  isolated Conan home (default: $WORK/conan-home)
#   REFRAIN_DIST_BUILD       CMake build directory (default: $WORK/build)
#   REFRAIN_DIST_OUT         archive directory (default: dist)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${REFRAIN_DIST_WORK:-$ROOT/build/dist}"
CONAN_HOME="${REFRAIN_DIST_CONAN_HOME:-$WORK/conan-home}"
BUILD="${REFRAIN_DIST_BUILD:-$WORK/build}"
OUT="${REFRAIN_DIST_OUT:-$ROOT/dist}"

mkdir -p "$WORK" "$CONAN_HOME" "$OUT"
export CONAN_HOME
cd "$ROOT"

if [[ ! -f "$CONAN_HOME/profiles/dist-linux" ]]; then
    conan profile detect --force >/dev/null
    gcc_major="$(gcc -dumpversion | cut -d. -f1)"
    cat > "$CONAN_HOME/profiles/dist-linux" <<PROFILE
[settings]
os=Linux
arch=x86_64
compiler=gcc
compiler.version=${gcc_major}
compiler.cppstd=26
compiler.libcxx=libstdc++11
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
tools.build:cflags=["-O3","-DNDEBUG","-fstack-protector-strong","-fstack-clash-protection","-fcf-protection=full","-D_FORTIFY_SOURCE=3","-ffat-lto-objects","-flto=auto"]
tools.build:cxxflags=["-O3","-DNDEBUG","-fstack-protector-strong","-fstack-clash-protection","-fcf-protection=full","-D_FORTIFY_SOURCE=3","-ffat-lto-objects","-flto=auto"]
tools.build:sharedlinkflags=["-Wl,-z,relro","-Wl,-z,now","-flto=auto"]
tools.build:exelinkflags=["-Wl,-z,relro","-Wl,-z,now","-flto=auto"]
PROFILE
fi

python3 tools/vendor_export.py --profile dist-linux >/dev/null

conan install . -pr:h dist-linux -pr:b default -of "$WORK/conan" --build=missing \
    -o "&:with_crashpad=False" -o "&:with_wasm=False" \
    -o "&:build_tests=False" -o "&:build_bench=False" -o "&:lto=True" \
    --lockfile=conan/locks/vendor.lock

toolchain="$(find "$WORK/conan" -name conan_toolchain.cmake -print -quit)"
test -n "$toolchain"

cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREFRAIN_HARDEN=ON -DREFRAIN_HARDEN_MAX=ON -DREFRAIN_LTO=ON \
    -DREFRAIN_WITH_CRASHPAD=OFF \
    -DREFRAIN_WITH_WASM=OFF -DREFRAIN_BUILD_SERVER=ON \
    -DREFRAIN_BUILD_TESTS=OFF -DREFRAIN_BUILD_BENCH=OFF
cmake --build "$BUILD" -j "$(nproc)" --target refrain-server refrain-tools

version="$("$BUILD/bin/refrain-tools" version | head -1 | awk '{print $2}')"
test -n "$version"
name="refrain-${version}-linux-x86_64"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/bin" "$stage/$name/gamepack"
cp "$BUILD/bin/refrain-server" "$BUILD/bin/refrain-tools" "$stage/$name/bin/"
cp gamepack/gamepack.toml "$stage/$name/gamepack/"
cp -r gamepack/presets "$stage/$name/gamepack/"
cp LICENSE NOTICE.md README.md "$stage/$name/"
cat > "$stage/$name/run-server.sh" <<'RUN'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/bin/refrain-server" --gamepack "$here/gamepack/gamepack.toml" "$@"
RUN
chmod +x "$stage/$name/run-server.sh"
tar -C "$stage" -czf "$OUT/$name.tar.gz" "$name"
echo "$OUT/$name.tar.gz"

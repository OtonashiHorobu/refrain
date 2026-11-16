#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Build one first-party mod profile and run its smoke checks.
#
#   scripts/mod-profile-smoke.sh <profile> [build-dir]
#
# Configuration comes from an existing Conan install: point
# REFRAIN_MOD_DEPS_DIR at the directory that holds `conan_toolchain.cmake`
# (default: build/Release/generators). The script configures its own build
# directory, builds the host binaries without tests/benchmarks and runs the
# profile-specific checks:
#
#   * the binary, the profile file and the built-in image describe the same mod
#     list (`--built` plus an independently generated image);
#   * the default gamepack matches the profile;
#   * the server starts its composition (`--check`);
#   * for `minimal`: a gamepack that pins a mod the profile does not build fails
#     with the profile diagnostic;
#   * for `dynamic`: the selected mods are built as modules and load through the
#     native loader (the data-layer provider is active).
#
# Used by the CI profile matrix and by `scripts/ci.sh --profile <name>`.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="${1:-}"
BUILD="${2:-}"
if [[ -z "$PROFILE" ]]; then
    echo "usage: $0 <vanilla|minimal|dynamic> [build-dir]" >&2
    exit 2
fi
case "$PROFILE" in
    [a-z0-9]*) ;;
    *)
        echo "invalid profile name: $PROFILE" >&2
        exit 2
        ;;
esac
BUILD="${BUILD:-build/profile-$PROFILE}"
DEPS_DIR="${REFRAIN_MOD_DEPS_DIR:-$ROOT/build/Release/generators}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

cd "$ROOT"

TOOLCHAIN="$DEPS_DIR/conan_toolchain.cmake"
[[ -f "$TOOLCHAIN" ]] || die "missing $TOOLCHAIN; run conan install first or set REFRAIN_MOD_DEPS_DIR"
RUN_ENV="$(find "$(dirname "$DEPS_DIR")" -maxdepth 2 -name conanrun.sh -print -quit 2>/dev/null || true)"
if [[ -n "$RUN_ENV" ]]; then
    # shellcheck disable=SC1090
    set +u
    source "$RUN_ENV"
    set -u
fi

say "configure profile '$PROFILE' in $BUILD"
cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DREFRAIN_MOD_PROFILE="$PROFILE" \
    -DREFRAIN_LTO=OFF \
    -DREFRAIN_ARCH_LEVEL=baseline \
    -DREFRAIN_BUILD_TESTS=OFF \
    -DREFRAIN_BUILD_BENCH=OFF >/dev/null

say "build profile '$PROFILE'"
cmake --build "$BUILD" -j "$JOBS"

BIN="$BUILD/bin"
IMAGE="$BUILD/generated/builtin.image"
TOOLS="$BIN/refrain-tools"
SERVER="$BIN/refrain-server"
[[ -x "$TOOLS" && -x "$SERVER" && -f "$IMAGE" ]] || die "profile build did not produce tools/server/image"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say "build identity and profile checks"
"$TOOLS" version >"$WORK/version.txt"
grep -q "profile: $PROFILE" "$WORK/version.txt" \
    || die "version output does not name profile '$PROFILE'"
grep -qE 'mods: +[0-9]+ built-in, [0-9]+ dynamic \(build [0-9a-f]{16}\)' "$WORK/version.txt" \
    || die "version output is missing the mod build id"
"$TOOLS" mods check --profile "$PROFILE" --built --image "$IMAGE" \
    >"$WORK/mods-check.txt" \
    || die "mods check --profile $PROFILE --built failed"
grep -q "mods check: profile '$PROFILE' OK" "$WORK/mods-check.txt" \
    || die "mods check did not confirm profile '$PROFILE'"
"$TOOLS" gamepack check --profile "$PROFILE" --built >"$WORK/gamepack-check.txt" \
    || die "gamepack check --profile $PROFILE --built failed"
grep -q "binary: $PROFILE build " "$WORK/gamepack-check.txt" \
    || die "gamepack check did not name the built profile"

say "image/source consistency"
# The build packs the CMake-resolved list; re-generating the image from the
# profile file through the tools must produce identical bytes.
"$TOOLS" builtin image --mods mods --profile "$PROFILE" \
    -o "$WORK/profile.image" >/dev/null
cmp "$IMAGE" "$WORK/profile.image" \
    || die "built-in image and profile '$PROFILE' mod list disagree"

case "$PROFILE" in
    minimal)
        say "minimal profile starts its data-only composition"
        "$SERVER" --gamepack gamepack/presets/data-only.toml --check \
            >"$WORK/server.txt" 2>&1 \
            || die "minimal profile failed to start the data-only composition"
        grep -q "with mod profile minimal" "$WORK/server.txt" \
            || die "startup log does not name profile 'minimal'"
        grep -q '\[host\]: loaded 2 mods' "$WORK/server.txt" \
            || die "minimal profile loaded an unexpected mod count"
        say "unbuilt pinned mod fails with the profile diagnostic"
        if "$SERVER" --gamepack gamepack/gamepack.toml --check \
            >"$WORK/unbuilt.txt" 2>&1; then
            die "minimal profile accepted a gamepack that pins unbuilt mods"
        fi
        grep -q "not built into this binary (profile=minimal" \
            "$WORK/unbuilt.txt" \
            || die "unbuilt-mod failure does not name the profile"
        grep -q "refrain.world.vanilla" "$WORK/unbuilt.txt" \
            || die "unbuilt-mod failure does not name the missing mod"
        ;;
    dynamic)
        say "dynamic profile loads modules through the native loader"
        MODULES="$BIN/modules"
        [[ -d "$MODULES" ]] || die "dynamic profile produced no modules directory"
        mapfile -t MODULE_FILES < <(find "$MODULES" -maxdepth 1 -type f -name 'refrain-mod-*.so')
        [[ "${#MODULE_FILES[@]}" -ge 1 ]] \
            || die "dynamic profile produced no first-party module"
        "$SERVER" --gamepack gamepack/gamepack.toml --check \
            >"$WORK/server.txt" 2>&1 \
            || die "dynamic profile failed to start the composition"
        grep -q "with mod profile dynamic" "$WORK/server.txt" \
            || die "startup log does not name profile 'dynamic'"
        grep -q '\[host\]: loaded 12 mods' "$WORK/server.txt" \
            || die "dynamic profile loaded an unexpected mod count"
        grep -q 'loaded mod refrain.data.vanilla 1.0.0 with ABI 1, code' \
            "$WORK/server.txt" \
            || die "dynamic mod did not activate its code through the loader"
        grep -q 'provides refrain:data.packs' "$WORK/server.txt" \
            || die "dynamic mod provider is missing from the loaded mod line"
        "$TOOLS" mods load -g gamepack/gamepack.toml --image "$IMAGE" \
            >"$WORK/mods-load.txt" \
            || die "dynamic profile failed the mod-host load smoke"
        grep -q 'mod host: 12 mod(s) loaded' "$WORK/mods-load.txt" \
            || die "dynamic mod-host smoke loaded an unexpected mod count"
        ;;
    *)
        say "start the profile's default composition"
        "$SERVER" --gamepack gamepack/gamepack.toml --check \
            >"$WORK/server.txt" 2>&1 \
            || die "profile '$PROFILE' failed to start its default composition"
        grep -q "with mod profile $PROFILE" "$WORK/server.txt" \
            || die "startup log does not name profile '$PROFILE'"
        ;;
esac

echo "  mod-profile smoke: '$PROFILE' OK"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Regenerate the world generation datapack snapshot from a server jar the user
# owns. The snapshot is game-derived data: it is never committed, it carries
# the notice in content/vanilla/NOTICE.md, and world generation tests skip when
# it is missing.
#
#   scripts/fetch-content.sh /path/to/server.jar
#   scripts/fetch-content.sh /path/to/server.jar --version 26.3 --data-version 5023
#
# The jar is read, never redistributed. Extraction uses the freshly built
# refrain-tools binary; set REFRAIN_TOOLS to use another one.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="${REFRAIN_TOOLS:-$ROOT/build/Release/bin/refrain-tools}"
VERSION="26.3"
DATA_VERSION="5023"

JAR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)      VERSION="${2:?--version needs a value}"; shift 2 ;;
        --data-version) DATA_VERSION="${2:?--data-version needs a value}"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)
            [[ -n "$JAR" ]] && { echo "unexpected argument: $1" >&2; exit 2; }
            JAR="$1"; shift ;;
    esac
done

[[ -n "$JAR" ]] || { echo "usage: $0 <server.jar> [--version V] [--data-version N]" >&2; exit 2; }
[[ -f "$JAR" ]] || { echo "server jar not found: $JAR" >&2; exit 1; }
[[ -x "$TOOLS" ]] || {
    echo "refrain-tools not found at $TOOLS" >&2
    echo "build it first: cmake --build --preset conan-release --target refrain-tools" >&2
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "extracting data entries from $JAR"
"$TOOLS" extract --jar "$JAR" --out "$WORK" --filter data --extract

[[ -d "$WORK/data" ]] || {
    echo "the jar contains no data/ tree: is it the server jar?" >&2
    exit 1
}

echo "packing the worldgen snapshot into content/vanilla/$VERSION"
python3 "$ROOT/tools/worldgen/pack_content.py" \
    --data-root "$WORK/data" \
    --out "$ROOT/content/vanilla/$VERSION" \
    --version "$VERSION" \
    --data-version "$DATA_VERSION"

echo "done: content/vanilla/$VERSION/data (not tracked by git)"

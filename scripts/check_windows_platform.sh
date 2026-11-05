#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Cross-syntax-check the Windows platform layer with MinGW. This is a
# compile-only gate (no Windows runtime, no upper-layer targets): it catches
# Win32 API/namespace/header breakage without a Windows runner.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++}"
STD="${WINDOWS_CXX_STD:-c++23}"

command -v "${CXX}" >/dev/null || {
    echo "missing ${CXX}; install g++-mingw-w64-x86-64" >&2
    exit 1
}

FMT_INCLUDE="/usr/include"
if [[ -f "${ROOT}/build/Release/compile_commands.json" ]]; then
    FOUND="$(python3 - "${ROOT}/build/Release/compile_commands.json" <<'PY'
import json, sys
for entry in json.load(open(sys.argv[1])):
    for arg in entry.get("command", "").split():
        if "fmt" in arg and arg.endswith("/include"):
            print(arg)
            raise SystemExit(0)
PY
)"
    if [[ -n "${FOUND}" ]]; then
        FMT_INCLUDE="${FOUND}"
    fi
fi

status=0
for source in "${ROOT}"/src/platform/windows/*.cpp; do
    echo "  windows-syntax: $(basename "${source}")"
    if ! "${CXX}" -std="${STD}" -fsyntax-only \
        -I"${ROOT}/include" -I"${ROOT}/sdk/generated/c" \
        -I"${ROOT}/sdk/generated/cpp" -isystem "${FMT_INCLUDE}" \
        "${source}"; then
        status=1
    fi
done
if [[ "${status}" -ne 0 ]]; then
    echo "windows platform syntax check failed" >&2
    exit 1
fi
echo "windows platform syntax check: OK"

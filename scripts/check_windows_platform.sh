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

# fmt headers are needed only when a platform source includes the logger. Use
# an explicit override, then the compile database, then a system libfmt install.
# The directory is added after the compiler's own system headers so the MinGW
# libc always wins over the host headers.
FMT_INCLUDE="${FMT_INCLUDE:-}"
if [[ -z "${FMT_INCLUDE}" && -f "${ROOT}/build/Release/compile_commands.json" ]]; then
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
if [[ -z "${FMT_INCLUDE}" && -f /usr/include/fmt/format.h ]]; then
    FMT_INCLUDE="/usr/include"
fi

status=0
for source in "${ROOT}"/src/platform/windows/*.cpp; do
    echo "  windows-syntax: $(basename "${source}")"
    ARGS=(-std="${STD}" -fsyntax-only -I"${ROOT}/include"
          -I"${ROOT}/sdk/generated/c" -I"${ROOT}/sdk/generated/cpp")
    if [[ -n "${FMT_INCLUDE}" ]]; then
        ARGS+=(-idirafter "${FMT_INCLUDE}")
    fi
    if ! "${CXX}" "${ARGS[@]}" "${source}"; then
        status=1
    fi
done
if [[ "${status}" -ne 0 ]]; then
    echo "windows platform syntax check failed" >&2
    exit 1
fi
echo "windows platform syntax check: OK"

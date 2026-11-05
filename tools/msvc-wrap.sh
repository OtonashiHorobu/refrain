#!/usr/bin/env bash
#
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# MSVC-wine driver for clang-cl: locates the MSVC toolchain under /opt/msvc,
# exports INCLUDE/LIB, and forces lld-link (the only linker that runs natively
# on Linux). Used as the C/C++ compiler in the `msvc` Conan profile.

set -euo pipefail

MSVC_ROOT="/opt/msvc"

vdir="$(ls -d "${MSVC_ROOT}"/VC/Tools/MSVC/* 2>/dev/null | head -1)"
sdkinc="$(ls -d "${MSVC_ROOT}"/Windows\ Kits/10/Include/* 2>/dev/null | head -1)"
sdklib="$(ls -d "${MSVC_ROOT}"/Windows\ Kits/10/Lib/* 2>/dev/null | head -1)"

if [ -z "${vdir}" ] || [ -z "${sdkinc}" ] || [ -z "${sdklib}" ]; then
    echo "msvc-wrap: cannot locate MSVC toolchain under ${MSVC_ROOT}" >&2
    exit 1
fi

export INCLUDE="${vdir}/include;${vdir}/atlmfc/include;${sdkinc}/ucrt;${sdkinc}/um;${sdkinc}/shared"
export LIB="${vdir}/lib/x64;${sdklib}/ucrt/x64;${sdklib}/um/x64"

exec clang-cl -fuse-ld=lld-link "$@"

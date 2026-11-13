#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Line coverage for the unit suite, built in its own directory.
#
#   scripts/dev/coverage.sh                 # build, run, report
#   scripts/dev/coverage.sh --report-only   # reuse the last build
#   REFRAIN_COVERAGE_MIN=60 scripts/dev/coverage.sh   # fail under a threshold
#
# The number is not the point. What matters is the list of files no test ever
# enters: on a tree this size that list is short and every entry is a lead.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="$ROOT/build/coverage"
OUT="$ROOT/build/coverage-report"
REPORT_ONLY=0
# Set outside the build branch: --report-only has no suite run of its own, and
# an unset variable would fail the script under `set -u`.
suite_status=0
for arg in "$@"; do
    case "$arg" in
        --report-only) REPORT_ONLY=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;36m==\033[0m %s\n' "$*"; }

mkdir -p "$OUT"

if [[ "$REPORT_ONLY" == "0" ]]; then
    if [[ ! -d "$BUILD" ]]; then
        say "configuring $BUILD"
        # `user_presets` is disabled: Conan rewrites the presets file at the
        # source root and would make it include both this directory and the
        # ordinary build directory, which CMake rejects as a duplicate preset.
        conan install . -pr gcc-release --build=missing \
            --lockfile=conan/locks/vendor.lock -of "$BUILD" \
            -c tools.cmake.cmaketoolchain:user_presets= >/dev/null
    fi
    toolchain="$(find "$BUILD" -name conan_toolchain.cmake -print -quit)"
    if [[ -z "$toolchain" ]]; then
        echo "no conan_toolchain.cmake under $BUILD" >&2
        exit 1
    fi
    build_dir="$(dirname "$(dirname "$toolchain")")"
    if [[ ! -f "$build_dir/build.ninja" ]]; then
        # The build type stays Release because that is what the Conan layout
        # generated its toolchain for; the coverage option lowers the
        # optimisation itself, so a Debug override would only desynchronise the
        # two and drop the dependency include paths.
        cmake -S . -B "$build_dir" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
            -DREFRAIN_COVERAGE=ON \
            -DREFRAIN_LTO=OFF \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    fi

    say "building"
    cmake --build "$build_dir" -j "$(nproc)"

    say "running the suite"
    # The suite's exit code is kept, not swallowed: a coverage build that also
    # changes optimisation can surface a timing-sensitive test, and hiding that
    # would make the report look healthier than the tree is. The report is
    # still generated, then the run fails.
    datapack="$ROOT/content/vanilla/26.3/data"
    suite_status=0
    if [[ -d "$datapack" ]]; then
        REFRAIN_VANILLA_WORLDGEN="$datapack" "$build_dir/bin/refrain-tests" \
            >"$OUT/suite.log" 2>&1 || suite_status=$?
    else
        "$build_dir/bin/refrain-tests" >"$OUT/suite.log" 2>&1 || suite_status=$?
    fi
    if [[ "$suite_status" != "0" ]]; then
        say "the suite failed under -O0 (exit $suite_status); see $OUT/suite.log"
    fi
else
    toolchain="$(find "$BUILD" -name conan_toolchain.cmake -print -quit)"
    if [[ -z "$toolchain" ]]; then
        echo "no conan_toolchain.cmake under $BUILD; run without --report-only" >&2
        exit 1
    fi
    build_dir="$(dirname "$(dirname "$toolchain")")"
fi

say "suite exit: $suite_status (log: $OUT/suite.log)"

say "report"
# gcov resolves the notes file from the object name and the source from the
# working directory, and gcovr does not reproduce that pairing on this tree.
# The tool walks the notes files itself, so it is the only writer of the report.
report_status=0
if python3 "$ROOT/tools/coverage_report.py" --build "$build_dir" --root "$ROOT" \
    >"$OUT/summary.txt" 2>"$OUT/summary.err"; then
    cat "$OUT/summary.txt"
    if [[ -n "${REFRAIN_COVERAGE_MIN:-}" ]]; then
        percent="$(sed -n 's/^lines:.*(\(.*\)%)$/\1/p' "$OUT/summary.txt")"
        if [[ -z "$percent" ]]; then
            say "cannot read the total line count from $OUT/summary.txt"
            report_status=1
        elif ! awk -v got="$percent" -v min="$REFRAIN_COVERAGE_MIN" \
            'BEGIN { exit !(got + 0 >= min + 0) }'; then
            say "line coverage ${percent}% is under REFRAIN_COVERAGE_MIN=$REFRAIN_COVERAGE_MIN"
            report_status=1
        fi
    fi
else
    say "the report failed; see $OUT/summary.err"
    report_status=1
fi

# A failing suite is the more urgent result, so it keeps its exit code.
if [[ "$suite_status" == "0" && "$report_status" != "0" ]]; then
    exit "$report_status"
fi
exit "$suite_status"

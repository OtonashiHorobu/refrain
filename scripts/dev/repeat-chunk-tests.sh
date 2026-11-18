#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Repeat the chunk streaming suites under CPU load. One pass is not evidence
# for a timing-sensitive suite: the same test can pass alone and fail when the
# host is busy. This loop keeps the spare cores busy and fails on the first
# repetition that fails, so a scheduling-dependent defect cannot hide behind a
# single green run.
#
#   scripts/dev/repeat-chunk-tests.sh [--repeats N] [--jobs N] [--reload]
#                                     [--full-suite] [--binary PATH]
#
# --reload adds the content-backed reload suite, which needs the locally
# generated datapack snapshot (scripts/fetch-content.sh <server.jar>) and fails
# when the snapshot is missing. The scheduled workflow runs the datapack-free
# default: the reload suite returns early in CI, so repeating it is a local
# gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPEATS=20
JOBS="$(( $(nproc 2>/dev/null || echo 4) / 2 ))"
BINARY="$ROOT/build/Release/bin/refrain-tests"
RELOAD=0
FULL_SUITE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeats) REPEATS="${2:?--repeats needs a count}"; shift 2 ;;
        --jobs) JOBS="${2:?--jobs needs a count}"; shift 2 ;;
        --binary) BINARY="${2:?--binary needs a path}"; shift 2 ;;
        --reload) RELOAD=1; shift ;;
        --full-suite) FULL_SUITE=1; shift ;;
        -h|--help) sed -n '6,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "chunk repeat: unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ "$REPEATS" =~ ^[1-9][0-9]*$ ]] \
    || { echo "chunk repeat: --repeats must be a positive integer" >&2; exit 2; }
[[ -x "$BINARY" ]] \
    || { echo "chunk repeat: $BINARY is missing; run cmake --build --preset conan-release" >&2; exit 2; }

# The binary filters by whole test name: a positional argument is a name
# pattern and a suite has no filter of its own. The helper discovers the test
# names from the binary itself and repeats each one by exact name, so a case
# added to these suites is picked up and a renamed case is reported instead of
# silently shrinking the gate.
selector='chunk stream|chunk cache|re-encoded|chunk service'
selector+='|generated entities follow|an item frame spawn'
selector+='|an entity type the static registry|chunk entities are announced'
if (( RELOAD == 1 )); then
    if ! compgen -G "$ROOT/content/vanilla/*/data/minecraft/worldgen" >/dev/null; then
        echo "chunk repeat: the datapack snapshot is missing; run scripts/fetch-content.sh <server.jar>, or drop --reload for the datapack-free gate" >&2
        exit 2
    fi
    selector+='|chunk generation reload'
fi

mapfile -t tests < <(
    "$BINARY" --list-tests 2>/dev/null |
        sed -n 's/^matching test: //p' |
        grep -E "$selector" || true
)
if (( ${#tests[@]} == 0 )); then
    echo "chunk repeat: the name selector matched no test; the suite names changed" >&2
    exit 2
fi

report="$(mktemp)"
cleanup() {
    rm -f "$report"
    if (( ${#load_pids[@]} > 0 )); then
        kill "${load_pids[@]}" 2>/dev/null || true
        wait "${load_pids[@]}" 2>/dev/null || true
    fi
}
load_pids=()
trap cleanup EXIT

# The assertion text of a failed run lives in the testcase's system-out block
# of the JUnit report; the per-run reports that passed are discarded.
show_failure() {
    grep -F 'status="FAILED"' "$report" || true
    sed -n '/status="FAILED"/,/<\/testcase>/p' "$report" | head -40
}
run_test() {
    "$BINARY" "$1" -r junit -o "$report" >/dev/null 2>&1
}

# A name that no longer executes leaves a run that exits zero without testing
# anything, so the preflight refuses it before any repetition counts.
for name in "${tests[@]}"; do
    if ! run_test "$name"; then
        echo "chunk repeat: '$name' failed in the preflight" >&2
        show_failure >&2
        exit 1
    fi
    if ! grep -F "name=\"$name\"" "$report" | grep -qF 'status="PASSED"'; then
        echo "chunk repeat: '$name' did not execute; the name selector is stale" >&2
        exit 2
    fi
done

if (( JOBS > 0 )); then
    for (( index = 0; index < JOBS; ++index )); do
        yes >/dev/null &
        load_pids+=("$!")
    done
    echo "chunk repeat: $REPEATS round(s) of ${#tests[@]} tests with $JOBS load worker(s)"
else
    echo "chunk repeat: $REPEATS round(s) of ${#tests[@]} tests without synthetic load"
fi

for (( round = 1; round <= REPEATS; ++round )); do
    for name in "${tests[@]}"; do
        if ! run_test "$name"; then
            echo "chunk repeat: round $round of $REPEATS failed on '$name'" >&2
            show_failure >&2
            exit 1
        fi
    done
    printf 'chunk repeat: round %d/%d passed\n' "$round" "$REPEATS"
done

if (( FULL_SUITE == 1 )); then
    if ! "$BINARY" -r junit -o "$report" >/dev/null 2>&1; then
        echo "chunk repeat: the full suite failed under load" >&2
        show_failure >&2
        exit 1
    fi
    echo "chunk repeat: full suite passed under load"
fi

echo "chunk repeat: $REPEATS round(s) of ${#tests[@]} tests passed"

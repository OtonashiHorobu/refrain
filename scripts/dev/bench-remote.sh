#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Run benchmarks on a dedicated host over SSH instead of the development
# machine, so a compile running in another terminal cannot move the numbers.
#
# The host is never committed: point REFRAIN_BENCH_HOST at it.
#
#   REFRAIN_BENCH_HOST=user@host scripts/dev/bench-remote.sh \
#       --targets terrain,full --chunks 16 --workers 1,8 --repeat 5
#
# Environment:
#   REFRAIN_BENCH_HOST       ssh target; unset means "run locally"
#   REFRAIN_BENCH_DIR        remote checkout, relative to $HOME (default Projects/refrain)
#   REFRAIN_BENCH_BUILD      remote build directory (default build/bench)
#   REFRAIN_BENCH_ARCH       remote architecture level (default x86-64-v3)
#   REFRAIN_BENCH_REQUIRE    set to 1 to fail instead of falling back locally
#
# Exit code 0 means the benchmark ran; the numbers are on stdout and in
# bench/results/.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

HOST="${REFRAIN_BENCH_HOST:-}"
REMOTE_DIR="${REFRAIN_BENCH_DIR:-Projects/refrain}"
REMOTE_BUILD="${REFRAIN_BENCH_BUILD:-build/bench}"
ARCH_LEVEL="${REFRAIN_BENCH_ARCH:-x86-64-v3}"
REQUIRE_REMOTE="${REFRAIN_BENCH_REQUIRE:-0}"

BENCH_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --targets|--chunks|--workers|--repeat|--seed|--format)
            BENCH_ARGS+=("$1" "$2")
            shift 2
            ;;
        *)
            BENCH_ARGS+=("$1")
            shift
            ;;
    esac
done

say() { printf '\033[1;36m==\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# A benchmark needs the machine to itself. Refuse to present numbers from a
# loaded host as comparable, but still run so the caller gets output.
check_remote_idle() {
    local load cores busy
    load="$(ssh -o BatchMode=yes "$HOST" 'cat /proc/loadavg | cut -d" " -f1')"
    cores="$(ssh -o BatchMode=yes "$HOST" 'nproc')"
    busy="$(awk -v l="$load" -v c="$cores" 'BEGIN { printf "%d", (l * 100) / c }')"
    say "remote load ${load} on ${cores} cores (${busy}%)"
    if [[ "$busy" -gt 25 ]]; then
        warn "the remote host is busy; treat these numbers as indicative only"
    fi
}

run_local() {
    warn "running locally: the numbers share the machine with every other job"
    local build="$ROOT/build/bench"
    if [[ ! -d "$build" ]]; then
        say "configuring $build (LTO on, ${ARCH_LEVEL})"
        cmake --preset conan-release \
            -B "$build" \
            -DREFRAIN_LTO=ON \
            -DREFRAIN_ARCH_LEVEL="$ARCH_LEVEL" \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    fi
    cmake --build "$build" --target refrain-bench-runtime -j "$(nproc)"
    "$build/bin/refrain-bench-runtime" "${BENCH_ARGS[@]}"
}

if [[ -z "$HOST" ]]; then
    run_local
    exit 0
fi

if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST" true 2>/dev/null; then
    if [[ "$REQUIRE_REMOTE" == "1" ]]; then
        die "cannot reach $HOST and REFRAIN_BENCH_REQUIRE=1"
    fi
    warn "cannot reach $HOST; falling back to the local machine"
    run_local
    exit 0
fi

check_remote_idle

say "syncing the working tree to $HOST:$REMOTE_DIR"
rsync -a --delete \
    --exclude=.git \
    --exclude='build*' \
    --exclude=.cache \
    --exclude=refrain-internal \
    --exclude=world \
    --exclude='install*' \
    --exclude=run \
    --exclude=AGENTS.local.md \
    ./ "$HOST:$REMOTE_DIR/"

say "building and running on $HOST"
ssh -o BatchMode=yes "$HOST" bash -lc "
set -euo pipefail
cd \"\$HOME/$REMOTE_DIR\"
export PATH=\"\$HOME/.venvs/conan/bin:\$PATH\"
if [ ! -d '$REMOTE_BUILD' ]; then
    conan install . -pr gcc-release --build=missing --lockfile=conan/locks/vendor.lock >/dev/null
    cmake --preset conan-release -B '$REMOTE_BUILD' \
        -DREFRAIN_LTO=ON \
        -DREFRAIN_ARCH_LEVEL='$ARCH_LEVEL' \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null
fi
cmake --build '$REMOTE_BUILD' --target refrain-bench-runtime -j \"\$(nproc)\" >/dev/null
'$REMOTE_BUILD'/bin/refrain-bench-runtime ${BENCH_ARGS[*]:-}
"

say "done: numbers above come from $HOST ($REMOTE_BUILD, LTO on, $ARCH_LEVEL)"

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
#   REFRAIN_BENCH_SSH_KEY    identity file for that target; unset uses the agent
#   REFRAIN_BENCH_LOCK       set to 0 to skip the host lock and run anyway
#   REFRAIN_BENCH_LOCK_WAIT  seconds to wait for the lock (default 3600)
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
SSH_KEY="${REFRAIN_BENCH_SSH_KEY:-}"
TAKE_LOCK="${REFRAIN_BENCH_LOCK:-1}"
LOCK_WAIT="${REFRAIN_BENCH_LOCK_WAIT:-3600}"

# A dedicated identity makes the run work without an unlocked agent, which is
# what an unattended benchmark needs.
SSH_OPTS=(-o BatchMode=yes)
if [[ -n "$SSH_KEY" ]]; then
    SSH_OPTS+=(-i "$SSH_KEY")
fi

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

if ! ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 "$HOST" true 2>/dev/null; then
    if [[ "$REQUIRE_REMOTE" == "1" ]]; then
        die "cannot reach $HOST and REFRAIN_BENCH_REQUIRE=1"
    fi
    warn "cannot reach $HOST; falling back to the local machine"
    run_local
    exit 0
fi

# The idle check runs inside the lock: a lane that waited along with two
# others must see the state it is about to measure, not the state at the
# moment it asked.

say "syncing the working tree to $HOST:$REMOTE_DIR"
# Anchored patterns: an unanchored `world` would also drop src/world.
# The host generates its own datapack once and keeps it. Syncing it would
# replace a real directory there with a lane's symlink and dangle. Lane
# worktrees are excluded for the same reason: they hold their own build
# trees, and copying a live one races with the lane that owns it.
rsync -a --delete -e "ssh ${SSH_OPTS[*]}" \
    --exclude=/.git \
    --exclude='/build*' \
    --exclude=/.cache \
    --exclude=/refrain-internal \
    --exclude=/.agents \
    --exclude=/world \
    --exclude='/install*' \
    --exclude=/run \
    --exclude='/content/vanilla/*/data' \
    --exclude=/AGENTS.local.md \
    ./ "$HOST:$REMOTE_DIR/"

say "building and running on $HOST"
# The remote login shell is not necessarily bash (fish here), so send the
# script on stdin instead of as an argument it would have to parse. The Conan
# layout decides where the build directory lands, so find the toolchain file
# instead of assuming the path.
ssh "${SSH_OPTS[@]}" "$HOST" bash -s <<REMOTE
set -euo pipefail
# One benchmark at a time on this host: two concurrent builds both skew the
# numbers and fight for the cores. The lock is on the host, so it covers
# lanes on different machines as well.
if [ "${TAKE_LOCK}" != "0" ]; then
    exec 9>"\$HOME/.refrain-bench.lock"
    if ! flock -w "${LOCK_WAIT}" 9; then
        echo "error: could not take the benchmark lock within ${LOCK_WAIT}s" >&2
        exit 1
    fi
fi
# Measured after the lock, so the load shown is the load this run will see.
load="\$(cut -d' ' -f1 /proc/loadavg)"
cores="\$(nproc)"
busy="\$(awk -v l="\$load" -v c="\$cores" 'BEGIN { printf "%d", (l * 100) / c }')"
echo "remote load \$load on \$cores cores (\$busy%)"
if [ "\$busy" -gt 25 ]; then
    echo "warning: the host is busy; treat these numbers as indicative only" >&2
fi
cd "\$HOME/${REMOTE_DIR}"
export PATH="\$HOME/.venvs/conan/bin:\$PATH"
# The chunk-generator benchmark needs the generated datapack; without it the
# binary reports the run as skipped.
export REFRAIN_VANILLA_WORLDGEN="\${REFRAIN_VANILLA_WORLDGEN:-\$PWD/content/vanilla/26.3/data}"
output='${REMOTE_BUILD}'
if [ ! -d "\$output" ]; then
    conan install . -pr gcc-release --build=missing \
        --lockfile=conan/locks/vendor.lock -of "\$output" >/dev/null
fi
toolchain="\$(find "\$output" -name conan_toolchain.cmake -print -quit)"
if [ -z "\$toolchain" ]; then
    echo "no conan_toolchain.cmake under \$output" >&2
    exit 1
fi
build="\$(dirname "\$(dirname "\$toolchain")")"
if [ ! -f "\$build/build.ninja" ]; then
    cmake -S . -B "\$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="\$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DREFRAIN_LTO=ON \
        -DREFRAIN_ARCH_LEVEL='${ARCH_LEVEL}' \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null
fi
cmake --build "\$build" --target refrain-bench-runtime -j "\$(nproc)" >/dev/null
"\$build/bin/refrain-bench-runtime" ${BENCH_ARGS[*]:-}
REMOTE

say "done: numbers above come from $HOST ($REMOTE_BUILD, LTO on, $ARCH_LEVEL)"

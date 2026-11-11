#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Load and stability soak: start a server on a synthetic world, open protocol
# bots that walk around, then report the join latency and the server tick
# times. The world is generated locally; the run needs the worldgen datapack
# (`scripts/fetch-content.sh <server.jar>`) for the vanilla world provider.
#
#   scripts/soak.sh                       # 100 bots, 120 s, port 25565
#   scripts/soak.sh --bots 20 --seconds 60 --port 25570
#
# Exit code 0 means every bot joined and the p99 tick work stayed under
# `--p99-ms` (default 15).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REFRAIN_BIN:-$ROOT/build/Release/bin}"
BOTS=100
SECONDS_=120
PORT=25565
WALK_RADIUS=8
VIEW_DISTANCE=10
P99_MS=15
KEEP=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bots)          BOTS="${2:?}"; shift 2 ;;
        --seconds)       SECONDS_="${2:?}"; shift 2 ;;
        --port)          PORT="${2:?}"; shift 2 ;;
        --walk-radius)   WALK_RADIUS="${2:?}"; shift 2 ;;
        --view-distance) VIEW_DISTANCE="${2:?}"; shift 2 ;;
        --p99-ms)        P99_MS="${2:?}"; shift 2 ;;
        --keep)          KEEP=1; shift ;;
        -h|--help)       sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -x "$BIN/refrain-server" ]] || { echo "refrain-server not built" >&2; exit 1; }
[[ -x "$BIN/refrain-tools" ]] || { echo "refrain-tools not built" >&2; exit 1; }
[[ -d "$ROOT/content/vanilla/26.3/data" ]] || {
    echo "worldgen datapack missing; run scripts/fetch-content.sh <server.jar>" >&2
    exit 1
}

WORK="$(mktemp -d)"
cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$KEEP" ]]; then
        echo "soak: kept $WORK" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

python3 "$ROOT/tools/make_test_world.py" gen --out "$WORK/anvil" --seed 1234 >/dev/null
"$BIN/refrain-tools" world import --from "$WORK/anvil" --to "$WORK/world" >/dev/null

mkdir -p "$WORK/config"
cat >"$WORK/config/network.toml" <<EOF
bind_host = "127.0.0.1"
port = $PORT
online_mode = false
view_distance = $VIEW_DISTANCE
simulation_distance = $VIEW_DISTANCE
status.max_players = $((BOTS + 20))
EOF

# Schedule the diagnostics and the stop on the tick grid so the run does not
# depend on console input timing.
TICKS=$((SECONDS_ * 20 + 100))
cat >"$WORK/script.txt" <<EOF
$((TICKS - 20)) tps
$((TICKS - 10)) net
$TICKS stop
EOF
"$BIN/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --world "$WORK/world" --config-dir "$WORK/config" \
    --script "$WORK/script.txt" --ticks $((TICKS + 20)) \
    >"$WORK/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 600); do
    grep -q "network: listening" "$WORK/server.log" && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done
if ! grep -q "network: listening" "$WORK/server.log"; then
    echo "soak: server did not start" >&2
    tail -20 "$WORK/server.log" >&2
    exit 1
fi

echo "soak: bots=$BOTS seconds=$SECONDS_ port=$PORT walk_radius=$WALK_RADIUS"
STARTED=$(date +%s)
set +e
if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -v "$BIN/refrain-tools" protocol load \
        --host "127.0.0.1:$PORT" --bots "$BOTS" \
        --duration-ms $((SECONDS_ * 1000)) --walk-radius "$WALK_RADIUS" \
        --walk-period-ms 100 2>"$WORK/load.txt"
    LOAD_RC=$?
    BOT_RSS_KB="$(awk '/Maximum resident set size/ {print $NF}' "$WORK/load.txt" | tail -1)"
    grep -E "^load:" "$WORK/load.txt" || true
else
    "$BIN/refrain-tools" protocol load --host "127.0.0.1:$PORT" --bots "$BOTS" \
        --duration-ms $((SECONDS_ * 1000)) --walk-radius "$WALK_RADIUS" \
        --walk-period-ms 100
    LOAD_RC=$?
    BOT_RSS_KB=""
fi
set -e
SERVER_RSS_KB="$(awk '/VmHWM/ {print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo "")"

# Shutdown time: the stop command fires on the tick grid, so record when the
# console command lands and how long the process needs to leave afterwards.
STOP_MS=""
for _ in $(seq 1 6000); do
    if grep -q "console: stop" "$WORK/server.log"; then
        STOP_MS=$(date +%s%3N)
        break
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.005
done
wait "$SERVER_PID" 2>/dev/null || true
EXIT_MS=$(date +%s%3N)
ELAPSED=$(( $(date +%s) - STARTED ))
SHUTDOWN_MS=""
if [[ -n "$STOP_MS" ]]; then
    SHUTDOWN_MS=$(( EXIT_MS - STOP_MS ))
fi

TICK_LINE="$(grep -E "tick: .*work_p99=" "$WORK/server.log" | tail -1 || true)"
NET_LINE="$(grep -E "net: players=" "$WORK/server.log" | tail -1 || true)"
echo "soak: elapsed=${ELAPSED}s"
if [[ -n "$SHUTDOWN_MS" ]]; then
    echo "soak: shutdown=${SHUTDOWN_MS}ms"
fi
if [[ -n "${SERVER_RSS_KB:-}" ]]; then
    echo "soak: server_peak_rss=$((SERVER_RSS_KB / 1024))MiB"
fi
if [[ -n "${BOT_RSS_KB:-}" ]]; then
    echo "soak: bots_peak_rss=$((BOT_RSS_KB / 1024))MiB"
fi
echo "soak: ${TICK_LINE:-tick: (no sample)}"
echo "soak: ${NET_LINE:-net: (no sample)}"

if [[ "$LOAD_RC" -ne 0 ]]; then
    echo "soak: bots failed to join" >&2
    exit "$LOAD_RC"
fi

P99="$(sed -n 's/.*work_p99=\([0-9.]*\)ms.*/\1/p' <<<"$TICK_LINE")"
if [[ -n "$P99" ]] && awk "BEGIN { exit !($P99 > $P99_MS) }"; then
    echo "soak: work_p99 ${P99}ms exceeds ${P99_MS}ms" >&2
    exit 1
fi

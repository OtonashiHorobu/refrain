#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Start a cross-built Windows server under Wine for a few ticks and require a
# clean exit plus the final replay hash. The fixture is offline: a synthetic
# Anvil world imported by the native host tools, and a game data directory
# assembled from the committed content tables. No game jar, datapack or
# network access is involved.
#
# The protocol registry has to be present for the server to reach the code
# path this smoke guards. The Linux runtime smoke starts from an empty
# synthetic data directory, which leaves the network layer reporting an
# unavailable registry, so a large-frame code generation fault in the server
# setup would go unnoticed there.
#
#   scripts/dev/mingw-v3-smoke.sh build/Release-v3/bin/refrain-server.exe \
#       build/host/build/Release/bin/refrain-tools
#
# Environment:
#   REFRAIN_MINGW_SMOKE_TICKS  ticks to run (default: 20)
#   REFRAIN_MINGW_SMOKE_KEEP   keep the fixture directory for inspection
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="${1:?usage: mingw-v3-smoke.sh <refrain-server.exe> <host refrain-tools> [ticks]}"
TOOLS="${2:?usage: mingw-v3-smoke.sh <refrain-server.exe> <host refrain-tools> [ticks]}"
TICKS="${3:-${REFRAIN_MINGW_SMOKE_TICKS:-20}}"

case "$SERVER" in
    /*) ;;
    *) SERVER="$ROOT/$SERVER" ;;
esac
case "$TOOLS" in
    /*) ;;
    *) TOOLS="$ROOT/$TOOLS" ;;
esac
[[ -f "$SERVER" ]] || { echo "server binary not found: $SERVER" >&2; exit 1; }
[[ -x "$TOOLS" ]] || { echo "host tools not found: $TOOLS" >&2; exit 1; }
command -v wine >/dev/null 2>&1 || { echo "wine is required" >&2; exit 1; }

WORK="$(mktemp -d)"
if [[ -n "${REFRAIN_MINGW_SMOKE_KEEP:-}" ]]; then
    echo "smoke fixture: $WORK"
else
    trap 'rm -rf "$WORK"' EXIT
fi

DATA="$WORK/game-data"
mkdir -p "$DATA/data/minecraft/worldgen" "$WORK/config"
cp "$ROOT/content/vanilla/26.3/packets.json" "$DATA/packets.json"
cp "$ROOT/content/vanilla/26.3/protocol_registry.json" "$DATA/protocol_registry.json"
cp "$ROOT/content/vanilla/26.3/tables.rvtb" "$DATA/tables.rvtb"
cp -R "$ROOT/content/vanilla/26.3/reports" "$DATA/reports"
printf '{\n  "schema": "refrain.game-data/1",\n  "game_version": "26.3",\n  "data_version": 5023,\n  "datapack": "data",\n  "source": "smoke fixture staged from the committed content tables"\n}\n' \
    >"$DATA/MANIFEST.json"
printf 'port = 0\n' >"$WORK/config/network.toml"

python3 "$ROOT/tools/make_test_world.py" gen --out "$WORK/anvil" --seed 1234 \
    >"$WORK/world-gen.txt"
"$TOOLS" world import --from "$WORK/anvil" --to "$WORK/rwf" \
    >"$WORK/world-import.txt"

dump_log() {
    echo "--- server log tail ---" >&2
    grep -a '^\[' "$LOG" | tail -20 >&2 || true
    grep -aE 'Unhandled page fault|wine:|refrain-server\+0x' "$LOG" | tail -6 >&2 || true
}

LOG="$WORK/server.log"
set +e
(
    cd "$WORK"
    xvfb-run -a wine "$SERVER" \
        --gamepack "$ROOT/gamepack/gamepack.toml" \
        --eula-file "$WORK/eula.toml" --accept-eula \
        --data-dir "$DATA" --no-data-download \
        --world "$WORK/rwf" --config-dir "$WORK/config" \
        --mods-lock "$WORK/mods.lock" \
        --hash-every 5 --ticks "$TICKS"
) >"$LOG" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
    echo "mingw server smoke: server exited with status $status" >&2
    dump_log
    exit 1
fi
if grep -q 'Unhandled page fault' "$LOG"; then
    echo "mingw server smoke: wine reported an unhandled page fault" >&2
    dump_log
    exit 1
fi

fail() {
    echo "mingw server smoke: $1" >&2
    dump_log
    exit 1
}
grep -q '\[net\]: listening on ' "$LOG" \
    || fail "the network layer did not start listening"
grep -q 'ready for Minecraft 26.3 clients' "$LOG" \
    || fail "the server did not report that it is ready"
grep -q "\[sim\]: final state hash at tick $TICKS is " "$LOG" \
    || fail "the final state hash for tick $TICKS is missing"
grep -q "\[server\]: ran $TICKS ticks" "$LOG" \
    || fail "the server did not report running $TICKS ticks"

hash="$(sed -n "s/.*\[sim\]: final state hash at tick $TICKS is //p" "$LOG")"
echo "mingw server smoke: ready, $TICKS ticks, final state hash $hash"

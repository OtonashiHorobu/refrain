#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Join N protocol bots and wait for their chunk view. The caller owns the
# server; this is the repeatable shape of the multi-client smoke.
#
#   scripts/dev/net-load.sh 127.0.0.1:25565 [count] [linger-ms]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${1:?usage: net-load.sh host:port [count] [linger-ms]}"
COUNT="${2:-20}"
LINGER_MS="${3:-10000}"
BIN="${REFRAIN_BIN:-$ROOT/build/Release/bin}"
LOG_DIR="$(mktemp -d)"
trap 'rm -rf "$LOG_DIR"' EXIT

pids=()
for ((i = 1; i <= COUNT; ++i)); do
    "$BIN/refrain-tools" protocol bot \
        --host "$TARGET" \
        --username "Load$i" \
        --timeout-ms 30000 \
        --wait-chunks 1 \
        --linger-ms "$LINGER_MS" \
        >"$LOG_DIR/bot-$i.log" 2>&1 &
    pids+=("$!")
done
status=0
for pid in "${pids[@]}"; do
    wait "$pid" || status=1
done

joined="$(grep -l 'joined as' "$LOG_DIR"/bot-*.log 2>/dev/null | wc -l)"
echo "net-load: $joined/$COUNT bots joined (linger ${LINGER_MS}ms)"
if [[ "$joined" -ne "$COUNT" ]]; then
    for log in "$LOG_DIR"/bot-*.log; do
        grep -E 'timeout|error|joined as' "$log" | sed "s|^|$(basename "$log"): |"
    done
    exit 1
fi
exit "$status"

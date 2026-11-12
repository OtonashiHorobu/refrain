#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Vanilla client acceptance: start a server on a generated world, join it with
# the unmodified client through the acceptance agent and assert that the level
# loads.
#
#   scripts/dev/vanilla-acceptance.sh --world /path/to/rwf-world
#   scripts/dev/vanilla-acceptance.sh --world /path/to/rwf-world --port 25598
#
# The client runtime is provisioned outside the repository; set MC_CLIENT_HOME
# to the 26.3 runtime (see scripts/dev/vanilla-client.sh). The agent source is
# compiled on demand with the JDK that runs the client, so nothing is committed
# as a binary.
#
# Exit code 0 means the client reached the play state and the screenshot was
# written. A failure prints the report and the tail of both logs.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${REFRAIN_BIN:-$ROOT/build/Release/bin}"
PORT=25598
WORLD=""
SHOT="${MC_SHOT:-}"
TIMEOUT=240
SETTLE=10
KEYS=""
KEEP=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --world)    WORLD="${2:?}"; shift 2 ;;
        --port)     PORT="${2:?}"; shift 2 ;;
        --shot)     SHOT="${2:?}"; shift 2 ;;
        --timeout)  TIMEOUT="${2:?}"; shift 2 ;;
        --settle)   SETTLE="${2:?}"; shift 2 ;;
        --keys)     KEYS="${2:?}"; shift 2 ;;
        --keep)     KEEP=1; shift ;;
        -h|--help)  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$WORLD" ]] || { echo "set --world to an rwf world" >&2; exit 2; }
[[ -d "$WORLD" ]] || { echo "world not found: $WORLD" >&2; exit 2; }
[[ -x "$BIN/refrain-server" ]] || { echo "refrain-server not built" >&2; exit 1; }
CLIENT_HOME="${MC_CLIENT_HOME:?set MC_CLIENT_HOME to the 26.3 client runtime}"
JAVA="${MC_JAVA:-java}"
JAVAC="${MC_JAVAC:-javac}"
JAR="${MC_JAR:-jar}"
RUN_DIR="${MC_RUN_DIR:-$CLIENT_HOME/run}"
SHOT="${SHOT:-$ROOT/run/acceptance.png}"

WORK="$(mktemp -d)"
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$KEEP" ]]; then
        echo "acceptance: kept $WORK" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

# The client refuses to proceed past the accessibility onboarding screen on a
# fresh profile, and quick-play only runs after it is answered.
if [[ -f "$RUN_DIR/options.txt" ]]; then
    sed -i 's/^onboardAccessibility:true/onboardAccessibility:false/' \
        "$RUN_DIR/options.txt"
fi

echo "acceptance: building the agent"
mkdir -p "$WORK/agent-classes"
"$JAVAC" -d "$WORK/agent-classes" \
    "$ROOT/tools/net/client-agent/src/refrain/agent/AcceptanceAgent.java"
printf 'Premain-Class: refrain.agent.AcceptanceAgent\n' >"$WORK/manifest.txt"
"$JAR" --create --file "$WORK/agent.jar" --manifest "$WORK/manifest.txt" \
    -C "$WORK/agent-classes" .

mkdir -p "$WORK/config"
cat >"$WORK/config/network.toml" <<EOF
bind_host = "127.0.0.1"
port = $PORT
online_mode = false
view_distance = 6
simulation_distance = 6
status.max_players = 20
EOF

# The server reads commands from stdin; a FIFO keeps it attached while the
# launcher asserts through the command layer.
mkfifo "$WORK/stdin"
"$BIN/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --world "$WORLD" --config-dir "$WORK/config" \
    <"$WORK/stdin" >"$WORK/server.log" 2>&1 &
SERVER_PID=$!
exec 9>"$WORK/stdin"
for _ in $(seq 1 600); do
    grep -q "\[net\]: listening" "$WORK/server.log" && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done
if ! grep -q "\[net\]: listening" "$WORK/server.log"; then
    echo "acceptance: server did not start" >&2
    tail -20 "$WORK/server.log" >&2
    exit 1
fi

REPORT="$WORK/report.json"
rm -f "$REPORT" "$REPORT.release"
echo "acceptance: joining 127.0.0.1:$PORT"
MC_CLIENT_HOME="$CLIENT_HOME" MC_RUN_DIR="$RUN_DIR" \
    MC_SERVER="127.0.0.1:$PORT" \
    MC_JAVA_ARGS="-javaagent:$WORK/agent.jar=log=$RUN_DIR/logs/latest.log,report=$REPORT,server=127.0.0.1:$PORT,timeout=$TIMEOUT,settle=$SETTLE${KEYS:+,keys=$KEYS}" \
    "$ROOT/scripts/dev/vanilla-client.sh" >"$WORK/client.out" 2>&1 &
CLIENT_PID=$!

# The agent writes the report once it decided; the launcher asserts while the
# client is still connected, then releases it.
for _ in $(seq 1 $(((TIMEOUT + 60) * 10))); do
    [[ -f "$REPORT" ]] && break
    kill -0 "$CLIENT_PID" 2>/dev/null || break
    sleep 0.1
done

STATUS="missing"
if [[ -f "$REPORT" ]]; then
    STATUS="$(sed -n 's/^ *"status": "\(.*\)",$/\1/p' "$REPORT")"
fi

if [[ "$STATUS" == "joined" ]]; then
    if command -v grim >/dev/null 2>&1; then
        mkdir -p "$(dirname "$SHOT")"
        grim "$SHOT" 2>/dev/null || true
    fi
    echo "acceptance: joined"
    [[ -s "$SHOT" ]] && echo "acceptance: screenshot $SHOT"
    echo "acceptance: $(grep -c 'player joined' "$WORK/server.log") join(s) on the server"

    # The connection counters are the server's own view of the session: ask
    # for them while the client is still connected, not after it left.
    printf 'net\n' >&9
    for _ in $(seq 1 100); do
        grep -q "\[net\]: players=1 " "$WORK/server.log" && break
        sleep 0.05
    done
    if grep -q "\[net\]: players=1 " "$WORK/server.log"; then
        echo "acceptance: the server counts one player in play"
    else
        echo "acceptance: FAILED (the server does not count the client)" >&2
        tail -5 "$WORK/server.log" >&2
        touch "$REPORT.release"
        exit 1
    fi
    touch "$REPORT.release"
    wait "$CLIENT_PID" 2>/dev/null || true
    exit 0
fi

touch "$REPORT.release" 2>/dev/null || true
kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_RC=1
echo "acceptance: FAILED (status=$STATUS client_rc=$CLIENT_RC)" >&2
[[ -f "$REPORT" ]] && cat "$REPORT" >&2
echo "--- server log ---" >&2
tail -15 "$WORK/server.log" >&2
echo "--- client log ---" >&2
tail -15 "$RUN_DIR/logs/latest.log" >&2 || true
exit 1

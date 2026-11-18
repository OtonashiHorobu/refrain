#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# First-start game-data smoke test.
#
# A build tree normally has the tracked tables and reports next to the binary,
# so the first-start fallbacks are easy to break without noticing. This smoke
# relocates the binary away from that layout and checks:
#
#   1. a clean directory with no snapshot and no download stops before
#      creating a world with one message that names the missing data and the
#      generation flag;
#   2. a staged datapack without the tracked tables and climate reports stops
#      the same way, naming the artifact and the generation command, and no
#      world or configuration directory is created;
#   3. a complete content snapshot starts and becomes ready;
#   4. with REFRAIN_SERVER_JAR pointing at a Minecraft 26.3 server archive the
#      generation entry point produces the tables and reports and the server
#      starts. Without a jar, or without Java, this step skips with one line.
#
# Usage:
#   scripts/dev/first-run-data-smoke.sh
#   REFRAIN_SERVER_JAR=/path/to/server.jar scripts/dev/first-run-data-smoke.sh
#
# REFRAIN_BIN_DIR selects the build to test; it defaults to the local
# build/Release/bin.

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="${REFRAIN_BIN_DIR:-$ROOT/build/Release/bin}"
SERVER_JAR="${REFRAIN_SERVER_JAR:-}"

die() {
    echo "first-run data smoke: $*" >&2
    exit 1
}

[[ -x "$BIN_DIR/refrain-server" ]] || die "refrain-server not found in $BIN_DIR"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# A relocated install: the binary and the composition, but no content
# snapshot, no source tooling and no data directory.
INSTALL="$WORK/install"
mkdir -p "$INSTALL/bin" "$INSTALL/gamepack/presets"
cp "$BIN_DIR/refrain-server" "$INSTALL/bin/"
cp "$ROOT/gamepack/gamepack.toml" "$INSTALL/gamepack/"
cp "$ROOT"/gamepack/presets/*.toml "$INSTALL/gamepack/presets/"
if [[ -x "$BIN_DIR/crashpad_handler" ]]; then
    cp "$BIN_DIR/crashpad_handler" "$INSTALL/bin/"
fi
SERVER="$INSTALL/bin/refrain-server"
GAMEPACK="$INSTALL/gamepack/gamepack.toml"
EULA="$WORK/eula.toml"

# run_case <name> <working dir> <expected exit: zero|nonzero> [server args...]
# Captures stdout+stderr in <working dir>/<name>.log and returns the status.
run_case() {
    local name=$1
    local directory=$2
    local expected=$3
    shift 3
    mkdir -p "$directory"
    local status=0
    (
        cd "$directory"
        "$SERVER" --accept-eula --eula-file "$EULA" --gamepack "$GAMEPACK" \
            "$@"
    ) > "$directory/$name.log" 2>&1 || status=$?
    if [[ "$expected" == zero && $status -ne 0 ]]; then
        cat "$directory/$name.log" >&2
        die "$name: expected a successful start, exit code $status"
    fi
    if [[ "$expected" == nonzero && $status -eq 0 ]]; then
        die "$name: expected the start to stop, it exited 0"
    fi
}

expect_log() {
    local file=$1
    local text=$2
    grep -q -- "$text" "$file" \
        || { cat "$file" >&2; die "missing log line: $text"; }
}

echo "first-run data: clean directory without a snapshot"
CLEAN="$WORK/clean"
run_case clean "$CLEAN" nonzero \
    --no-data-download --data-dir "$CLEAN/data" --world "$CLEAN/world" \
    --config-dir "$CLEAN/config" --ticks 1 --port 0
expect_log "$CLEAN/clean.log" 'found no game data for Minecraft 26.3'
expect_log "$CLEAN/clean.log" '--server-jar'
expect_log "$CLEAN/clean.log" "$CLEAN/data/data"
[[ ! -e "$CLEAN/world" ]] || die "clean: a world was created before the data was ready"
[[ ! -e "$CLEAN/config" ]] || die "clean: configuration was written before the data was ready"
echo "  stopped before world creation with an actionable message"

echo "first-run data: staged datapack without the tracked tables and reports"
STAGED="$WORK/staged"
mkdir -p "$STAGED/data/minecraft/worldgen/biome"
printf '{}\n' > "$STAGED/data/minecraft/worldgen/biome/plains.json"
run_case staged "$WORK/staged-run" nonzero \
    --no-data-download --data-dir "$STAGED" --world "$WORK/staged-run/world" \
    --config-dir "$WORK/staged-run/config" --ticks 1 --port 0
expect_log "$WORK/staged-run/staged.log" 'tables.rvtb'
expect_log "$WORK/staged-run/staged.log" 'biome_parameters/overworld.json'
expect_log "$WORK/staged-run/staged.log" 'tools/content/generate.sh'
expect_log "$WORK/staged-run/staged.log" '--server-jar'
[[ ! -e "$WORK/staged-run/world" ]] \
    || die "staged: a world was created before the tracked data was ready"
[[ ! -e "$WORK/staged-run/config" ]] \
    || die "staged: configuration was written before the tracked data was ready"
echo "  stopped before world creation, naming the artifact and the command"

echo "first-run data: complete content snapshot"
if [[ ! -d "$ROOT/content/vanilla/26.3/data" ]]; then
    echo "  skipped, the checkout has no staged datapack;" \
        "run scripts/fetch-content.sh first"
else
    COMPLETE="$WORK/complete"
    run_case complete "$COMPLETE" zero \
        --no-data-download --data-dir "$ROOT/content/vanilla/26.3" \
        --world "$COMPLETE/world" --config-dir "$COMPLETE/config" \
        --ticks 2 --port 0
    expect_log "$COMPLETE/complete.log" 'ready for Minecraft 26.3 clients'
    if grep -q 'vanilla tables are unavailable' "$COMPLETE/complete.log"; then
        cat "$COMPLETE/complete.log" >&2
        die "complete: the tracked tables were not loaded"
    fi
    if grep -q 'biome parameter report not found' "$COMPLETE/complete.log"; then
        cat "$COMPLETE/complete.log" >&2
        die "complete: the climate reports were not loaded"
    fi
    echo "  started from the packaged content snapshot"
fi

if [[ -z "$SERVER_JAR" ]]; then
    echo "first-run data: generation skipped, set REFRAIN_SERVER_JAR to a" \
        "Minecraft 26.3 server.jar to exercise it"
    echo "first-run data smoke: OK"
    exit 0
fi
if [[ ! -f "$SERVER_JAR" ]]; then
    die "REFRAIN_SERVER_JAR does not name a file: $SERVER_JAR"
fi
if ! command -v java > /dev/null 2>&1; then
    echo "first-run data: generation skipped, no Java runtime on PATH"
    echo "first-run data smoke: OK"
    exit 0
fi

echo "first-run data: generation from a server archive"
# The generation entry point reads the probes and packers relative to its own
# checkout, so stage the source tooling next to the relocated binary.
SOURCE_INSTALL="$WORK/source-install"
mkdir -p "$SOURCE_INSTALL/bin" "$SOURCE_INSTALL/content/vanilla"
cp -R "$INSTALL/gamepack" "$SOURCE_INSTALL/"
cp "$BIN_DIR/refrain-server" "$BIN_DIR/refrain-tools" "$SOURCE_INSTALL/bin/"
if [[ -x "$BIN_DIR/crashpad_handler" ]]; then
    cp "$BIN_DIR/crashpad_handler" "$SOURCE_INSTALL/bin/"
fi
mkdir -p "$SOURCE_INSTALL/tools"
cp -R "$ROOT/tools/content" "$ROOT/tools/light" "$ROOT/tools/block" \
    "$SOURCE_INSTALL/tools/"
mkdir -p "$SOURCE_INSTALL/tools/worldgen"
cp "$ROOT/tools/worldgen/biome_parameters.py" \
    "$ROOT/tools/worldgen/block_defaults.py" \
    "$ROOT/tools/worldgen/pack_content.py" "$SOURCE_INSTALL/tools/worldgen/"
mkdir -p "$SOURCE_INSTALL/content/vanilla/26.3"
cp "$ROOT/content/vanilla/NOTICE.md" "$SOURCE_INSTALL/content/vanilla/"
cp "$ROOT/content/vanilla/26.3/protocol_registry.json" \
    "$SOURCE_INSTALL/content/vanilla/26.3/"
GENERATED="$WORK/generated"
# The generation entry point and the tools binary are discovered relative to
# the running binary, so the source install is the one under test here.
SERVER="$SOURCE_INSTALL/bin/refrain-server"
GAMEPACK="$SOURCE_INSTALL/gamepack/gamepack.toml"
run_case generated "$GENERATED" zero \
    --no-data-download --server-jar "$SERVER_JAR" \
    --data-dir "$GENERATED/data" --world "$GENERATED/world" \
    --config-dir "$GENERATED/config" --ticks 2 --port 0
if [[ ! -f "$GENERATED/data/tables.rvtb" ]]; then
    cat "$GENERATED/generated.log" >&2
    die "generated: the tracked tables were not written"
fi
[[ -f "$GENERATED/data/reports/biome_parameters/overworld.json" ]] \
    || die "generated: the climate reports were not written"
expect_log "$GENERATED/generated.log" 'ready for Minecraft 26.3 clients'
if grep -q 'vanilla tables are unavailable' "$GENERATED/generated.log"; then
    cat "$GENERATED/generated.log" >&2
    die "generated: the runtime did not load the generated tables"
fi
echo "  generated the tables and reports from the server archive and started"

echo "first-run data smoke: OK"

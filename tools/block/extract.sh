#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Extract the block mining report from a locally owned 26.3 server jar.
#
# The report records, for every block, the destroy time the game caches on its
# states, the correct-tool-for-drops flag, the tool families that mine the
# block efficiently and the drop tier it requires; and for every item that
# carries a tool component, its family, material, tier speed, damage per
# block, stack limit, durability and rules. The game classes and the vanilla
# datapack tags in the jar are the source of truth: the probe bootstraps the
# game registries, binds the jar's tags and item components and derives the
# report from them, never from a decompiled tree or from memory.
#
# The reports directory must already hold the data generator's `blocks.json`
# and `minecraft/components/item/` reports (run
#   java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports
# first); the packer cross-checks the probe against both.
#
# The output is game-derived data: it is not covered by the repository license,
# carries its own notice and is regenerated from the jar.
#
# The Java runtime defaults to `java` on PATH; set JAVA to use
# another one (the data generation entry point does this when it
# is given --java).
#
# Usage: tools/block/extract.sh /path/to/server.jar /path/to/generated/reports
#
# Then copy `reports/block_mining` into the tracked content snapshot and import
# the tables again:
#   cp -r <reports>/block_mining content/vanilla/26.3/reports/
#   build/Release/bin/refrain-tools vanilla import \
#       -r <reports> -o content/vanilla/26.3 --force

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 /path/to/server.jar /path/to/generated/reports" >&2
    exit 2
fi

jar=$1
reports=$2
if [[ ! -f "$jar" ]]; then
    echo "error: '$jar' is not a file" >&2
    exit 1
fi
if [[ ! -f "$reports/blocks.json" ]]; then
    echo "error: '$reports/blocks.json' is missing: run the data generator" \
         "with --reports first" >&2
    exit 1
fi

script_dir=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# 26.3 ships a bundler jar: the game classes live in the versioned inner jar
# and `META-INF/classpath-joined` lists every classpath entry relative to the
# unpacked root.
unzip -q "$jar" -d "$work"
if [[ ! -f "$work/META-INF/classpath-joined" ]]; then
    echo "error: '$jar' has no bundler metadata" >&2
    exit 1
fi
mv "$work"/META-INF/libraries "$work"/libraries
mv "$work"/META-INF/versions "$work"/versions

classpath=$(tr ';' ':' < "$work/META-INF/classpath-joined")

(
    cd "$work"
    "${JAVA:-java}" -cp "$classpath" "$script_dir/MiningProbe.java" \
        "$work/block-mining-probe.json"
)

python3 "$script_dir/pack_report.py" \
    --probe "$work/block-mining-probe.json" \
    --blocks "$reports/blocks.json" \
    --reports "$reports" \
    --server-jar "$jar" \
    --out "$reports/block_mining"

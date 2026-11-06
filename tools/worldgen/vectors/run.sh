#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Regenerates the random-source vectors from a locally owned 26.3 server jar.
#
# The game classes are the source of truth: this runs LegacyRandomSource,
# XoroshiroRandomSource, WorldgenRandom, RandomSupport and Mth from the jar and
# prints the draws. Compare the output with the constants in
# test/worldgen/test_rng.cpp after a version bump.
#
# Usage: tools/worldgen/vectors/run.sh /path/to/server.jar

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 /path/to/server.jar [class]" >&2
    exit 2
fi

jar=$1
if [[ ! -f "$jar" ]]; then
    echo "error: '$jar' is not a file" >&2
    exit 1
fi

script_dir=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# 26.3 ships a bundler jar: the game classes live in
# META-INF/versions/<version>/<version>/server-<version>.jar and
# META-INF/classpath-joined lists every entry relative to the unpacked root.
unzip -q "$jar" -d "$work"
if [[ ! -f "$work/META-INF/classpath-joined" ]]; then
    echo "error: '$jar' has no bundler metadata" >&2
    exit 1
fi
mv "$work"/META-INF/libraries "$work"/libraries
mv "$work"/META-INF/versions "$work"/versions

classpath=$(tr ';' ':' < "$work/META-INF/classpath-joined")

cd "$work"
java -cp "$classpath" "$script_dir/${2:-Vectors}.java"

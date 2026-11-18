#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Generate the tracked game data the server needs from a server archive the
# operator owns: the worldgen datapack, the climate reports and the tracked
# tables. This is the one entry point behind the individual tools; the server
# invokes it on a first start when the install does not ship the tracked data
# and a complete source checkout is available.
#
# The steps are the documented pipeline and reuse the existing tooling:
#
#   1. the vanilla data generator writes its reports (`--reports`);
#   2. `tools/light/extract.sh` derives the block light report from the jar;
#   3. `tools/block/extract.sh` derives the block mining report from the jar;
#   4. `tools/worldgen/block_defaults.py` reduces the default block-state map;
#   5. `tools/worldgen/biome_parameters.py` packs the climate parameter reports;
#   6. `refrain-tools vanilla import` packs the reports into `tables.rvtb`.
#
# Requirements beyond this checkout are a Java runtime (25 or newer for 26.3),
# Python 3, `unzip` and the `refrain-tools` binary of the same build. The
# generated data is game-derived: it carries its own notice, is not covered by
# the repository license and is never committed.
#
# Usage:
#   tools/content/generate.sh --server-jar /path/to/server.jar \
#       --out content/vanilla/26.3 --tools build/Release/bin/refrain-tools \
#       --force
#
# The checkout's `content/vanilla/<version>` already carries the tracked
# tables and reports, so replacing that snapshot needs `--force`. Without it
# the entry point refuses before running the pipeline and says so.
#
# Options:
#   --server-jar FILE     the official bundler server archive (required)
#   --out DIR             data directory to fill (required)
#   --tools FILE          refrain-tools binary (default: found next to this
#                         script's checkout, then PATH)
#   --java FILE           Java executable (default: java from PATH)
#   --version VERSION     game version (default: 26.3)
#   --data-version N      DataVersion (default: 5023)
#   --work DIR            scratch directory to keep (default: a temporary one)
#   --force               replace existing tables and reports; required when
#                         --out already carries the tracked snapshot
#   --no-datapack         do not generate the datapack, only the tracked data
#
# Exit codes: 0 success, 2 usage, 3 a prerequisite is missing, 4 a generation
# step failed. The server turns 3 into "generation unavailable" and 4 into
# "generation failed" diagnostics.

set -euo pipefail

usage() {
    sed -n '26,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"

jar=""
out=""
tools="${REFRAIN_TOOLS:-}"
java="${JAVA:-java}"
version="26.3"
data_version="5023"
work=""
force=0
datapack=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server-jar)   jar="${2:?--server-jar needs a value}"; shift 2 ;;
        --out)          out="${2:?--out needs a value}"; shift 2 ;;
        --tools)        tools="${2:?--tools needs a value}"; shift 2 ;;
        --java)         java="${2:?--java needs a value}"; shift 2 ;;
        --version)      version="${2:?--version needs a value}"; shift 2 ;;
        --data-version) data_version="${2:?--data-version needs a value}"; shift 2 ;;
        --work)         work="${2:?--work needs a value}"; shift 2 ;;
        --force)        force=1; shift ;;
        --no-datapack)  datapack=0; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$jar" || -z "$out" ]]; then
    echo "error: --server-jar and --out are required" >&2
    usage
    exit 2
fi
if [[ ! -f "$jar" ]]; then
    echo "error: server archive '$jar' is not a file" >&2
    exit 3
fi
if [[ -z "$tools" ]]; then
    for candidate in \
        "$repo_root/build/Release/bin/refrain-tools" \
        "$repo_root/build/RelWithDebInfo/bin/refrain-tools" \
        "$repo_root/build/Debug/bin/refrain-tools"; do
        if [[ -x "$candidate" ]]; then
            tools="$candidate"
            break
        fi
    done
fi
if [[ -z "$tools" && -n "$(command -v refrain-tools || true)" ]]; then
    tools="$(command -v refrain-tools)"
fi
if [[ -z "$tools" ]]; then
    echo "error: refrain-tools was not found; pass --tools <path>" >&2
    exit 3
fi
if [[ ! -x "$tools" ]]; then
    echo "error: refrain-tools '$tools' is not executable" >&2
    exit 3
fi
for required in unzip python3 bash; do
    if ! command -v "$required" > /dev/null 2>&1; then
        echo "error: '$required' is required to generate the game data" >&2
        exit 3
    fi
done
if ! "$java" -version > /dev/null 2>&1; then
    echo "error: a Java runtime is required to run the data generator; set" \
        "JAVA or pass --java (Minecraft 26.3 needs Java 25 or newer)" >&2
    exit 3
fi
if ! python3 - "$jar" "$version" <<'PY'
import json
import sys
import zipfile

jar, expected = sys.argv[1], sys.argv[2]
try:
    with zipfile.ZipFile(jar) as archive:
        names = set(archive.namelist())
        if "META-INF/classpath-joined" not in names:
            print(
                f"error: '{jar}' is not an official bundler server archive "
                "(no META-INF/classpath-joined); use the server.jar from the "
                "Minecraft download",
                file=sys.stderr,
            )
            raise SystemExit(1)
        if "version.json" in names:
            version = json.loads(archive.read("version.json"))
            actual = version.get("id", "")
            if actual and actual != expected:
                print(
                    f"error: '{jar}' is Minecraft {actual}, expected {expected}",
                    file=sys.stderr,
                )
                raise SystemExit(1)
except zipfile.BadZipFile:
    print(f"error: '{jar}' is not a zip archive", file=sys.stderr)
    raise SystemExit(1)
PY
then
    exit 3
fi

# Replacement and prerequisite checks run before the multi-minute pipeline:
# a command that cannot install its output must fail in a second, not after
# the probes. The documented command passes --force because a checkout's
# content snapshot already holds the tracked tables and reports.
if [[ -f "$out/tables.rvtb" && "$force" != 1 ]]; then
    echo "error: '$out/tables.rvtb' already exists; pass --force to replace it" \
        >&2
    exit 2
fi
if [[ -d "$out/reports" && "$force" != 1 ]]; then
    echo "error: '$out/reports' already exists; pass --force to replace it" \
        >&2
    exit 2
fi
protocol_registry="$repo_root/content/vanilla/$version/protocol_registry.json"
if [[ ! -f "$out/protocol_registry.json" && ! -f "$protocol_registry" ]]; then
    echo "error: '$out/protocol_registry.json' is missing and this checkout" \
        "ships no '$protocol_registry'; copy it from a release archive" >&2
    exit 3
fi

if [[ -n "$work" ]]; then
    # The server passes a fixed scratch directory and sweeps it before every
    # attempt, so an interrupted run cannot leak the datapack extraction.
    mkdir -p "$work"
    cleanup=0
else
    work="$(mktemp -d)"
    cleanup=1
fi
cleanup_work() {
    if [[ "$cleanup" == 1 ]]; then
        rm -rf "$work"
    fi
}
trap cleanup_work EXIT

reports="$work/generated/reports"
report_tree="$work/report-tree"

# A datapack is complete when its manifest records the number of files below
# data/ and at least one namespace carries worldgen documents. A partial tree
# left by an interrupted pack fails the count and is extracted again, so a
# retry cannot start world generation on half-written documents.
datapack_complete() {
    python3 - "$out" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
data = root / "data"
manifest_path = root / "MANIFEST.json"
if not data.is_dir() or not manifest_path.is_file():
    raise SystemExit(1)
try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)
expected = manifest.get("entry_count")
if not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0:
    raise SystemExit(1)
actual = sum(1 for path in data.rglob("*") if path.is_file())
if actual != expected:
    raise SystemExit(1)
if not any(path.is_dir() for path in data.glob("*/worldgen")):
    raise SystemExit(1)
PY
}
have_datapack=0
if [[ "$datapack" == 1 ]] && datapack_complete; then
    have_datapack=1
fi

if [[ "$datapack" == 1 && "$have_datapack" != 1 ]]; then
    echo "  extracting the datapack"
    # The outer archive is a bundler: the real server jar lives under
    # META-INF/versions. `unzip` reads the inner tree directly; the extraction
    # tool re-reads the whole inner archive per entry, which is too slow for a
    # startup path.
    inner_jar="$(unzip -Z1 "$jar" \
        | grep -E '^META-INF/versions/.*/server-.*\.jar$' | head -n 1 || true)"
    if [[ -z "$inner_jar" ]]; then
        echo "error: '$jar' carries no META-INF/versions server jar" >&2
        exit 4
    fi
    rm -rf "$work/bundle" "$work/extract"
    mkdir -p "$work/bundle" "$work/extract"
    unzip -q "$jar" "$inner_jar" -d "$work/bundle" || {
        echo "error: the server archive could not be unpacked" >&2
        exit 4
    }
    unzip -q "$work/bundle/$inner_jar" 'data/*' -d "$work/extract" || {
        echo "error: the server archive carries no readable data/ tree" >&2
        exit 4
    }
    if [[ ! -d "$work/extract/data" ]]; then
        echo "error: the server archive carries no data/ tree" >&2
        exit 4
    fi
    python3 "$repo_root/tools/worldgen/pack_content.py" \
        --data-root "$work/extract/data" \
        --out "$out" \
        --version "$version" \
        --data-version "$data_version" || {
        echo "error: the datapack could not be packed" >&2
        exit 4
    }
    if ! datapack_complete; then
        echo "error: the packed datapack is incomplete" >&2
        exit 4
    fi
    # Structure templates are binary assets the download path also keeps; the
    # template manager expects <root>/<namespace>/structure/<path>.nbt. A
    # template tree is only added when none is present, so a retry keeps the
    # one the download path extracted.
    if [[ ! -d "$out/templates" ]]; then
        mkdir -p "$out/templates"
        for namespace in "$work/extract/data"/*/; do
            if [[ -d "$namespace/structure" ]]; then
                name="$(basename "$namespace")"
                mkdir -p "$out/templates/$name"
                cp -R "$namespace/structure" "$out/templates/$name/structure"
            fi
        done
    fi
fi

export JAVA="$java"

echo "generating Minecraft $version game data from '$jar'"
echo "  [1/6] running the vanilla data generator"
(
    cd "$work"
    "$java" -DbundlerMainClass=net.minecraft.data.Main -jar "$jar" --reports
) || {
    echo "error: the vanilla data generator failed" >&2
    exit 4
}
if [[ ! -f "$reports/blocks.json" ]]; then
    echo "error: the data generator wrote no reports under '$reports'" >&2
    exit 4
fi

echo "  [2/6] deriving the block light report"
bash "$repo_root/tools/light/extract.sh" "$jar" "$reports" || {
    echo "error: the block light probe failed" >&2
    exit 4
}

echo "  [3/6] deriving the block mining report"
bash "$repo_root/tools/block/extract.sh" "$jar" "$reports" || {
    echo "error: the block mining probe failed" >&2
    exit 4
}

echo "  [4/6] reducing the block default-state report"
python3 "$repo_root/tools/worldgen/block_defaults.py" \
    --blocks "$reports/blocks.json" \
    --out "$reports/block_defaults" || {
    echo "error: the block default-state reduction failed" >&2
    exit 4
}

echo "  [5/6] packing the biome parameter reports"
rm -rf "$report_tree"
mkdir -p "$report_tree/biome_parameters"
python3 "$repo_root/tools/worldgen/biome_parameters.py" \
    --datagen "$reports/biome_parameters/minecraft" \
    --out "$report_tree/biome_parameters" \
    --game-version "$version" \
    --data-version "$data_version" || {
    echo "error: the biome parameter reports could not be packed" >&2
    exit 4
}

echo "  [6/6] packing the reports into tables.rvtb"
rm -rf "$work/tables"
"$tools" vanilla import -r "$reports" -o "$work/tables" \
    --game-version "$version" --data-version "$data_version" --force || {
    echo "error: refrain-tools could not import the reports" >&2
    exit 4
}
if [[ ! -f "$work/tables/tables.rvtb" ]]; then
    echo "error: refrain-tools wrote no tables.rvtb" >&2
    exit 4
fi

cp "$work/tables/tables.rvtb" "$out/tables.rvtb.tmp"
mv "$out/tables.rvtb.tmp" "$out/tables.rvtb"
echo "  wrote '$out/tables.rvtb'"

# Assemble the report tree the runtime reads: the packed climate reports plus
# the three reports exactly as the probes wrote them.
for report in block_defaults block_light block_mining; do
    if [[ ! -d "$reports/$report" ]]; then
        echo "error: the pipeline produced no '$report' report" >&2
        exit 4
    fi
    cp -R "$reports/$report" "$report_tree/$report"
done
rm -rf "$out/reports"
cp -R "$report_tree" "$out/reports"
echo "  wrote the reports under '$out/reports'"

if [[ -f "$repo_root/content/vanilla/NOTICE.md" ]]; then
    cp "$repo_root/content/vanilla/NOTICE.md" "$out/NOTICE.md"
fi

# The protocol registry is versioned with the repository rather than derived
# from the server archive; the prerequisite check above guarantees that either
# the snapshot already holds it or the checkout can install it.
if [[ ! -f "$out/protocol_registry.json" ]]; then
    cp "$protocol_registry" "$out/protocol_registry.json"
    echo "  installed the protocol registry"
fi

echo "generated the Minecraft $version game data in '$out'"

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
#       --out content/vanilla/26.3 --tools build/Release/bin/refrain-tools
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
#   --force               overwrite existing tables and reports
#   --no-datapack         do not generate the datapack, only the tracked data
#
# Exit codes: 0 success, 2 usage, 3 a prerequisite is missing, 4 a generation
# step failed. The server turns 3 into "generation unavailable" and 4 into
# "generation failed" diagnostics.

set -euo pipefail

usage() {
    sed -n '26,44p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
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
        -h|--help)      usage ;;
        *) echo "error: unknown option '$1'" >&2; usage ;;
    esac
done

if [[ -z "$jar" || -z "$out" ]]; then
    echo "error: --server-jar and --out are required" >&2
    usage
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

if [[ -n "$work" ]]; then
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

mkdir -p "$out"
if [[ -f "$out/tables.rvtb" && "$force" != 1 ]]; then
    echo "error: '$out/tables.rvtb' already exists; pass --force to replace it" \
        >&2
    exit 2
fi
cp "$work/tables/tables.rvtb" "$out/tables.rvtb.tmp"
mv "$out/tables.rvtb.tmp" "$out/tables.rvtb"
echo "  wrote '$out/tables.rvtb'"

# Assemble the report tree the runtime reads: the data generator's climate
# report is packed into the tracked layout, the other three reports are copied
# as their probes wrote them.
report_tree="$work/report-tree"
rm -rf "$report_tree"
mkdir -p "$report_tree"
for report in block_defaults block_light block_mining; do
    if [[ ! -d "$reports/$report" ]]; then
        echo "error: the pipeline produced no '$report' report" >&2
        exit 4
    fi
    cp -R "$reports/$report" "$report_tree/$report"
done
if [[ ! -d "$reports/biome_parameters/minecraft" ]]; then
    echo "error: the data generator wrote no biome parameter reports" >&2
    exit 4
fi
python3 "$repo_root/tools/worldgen/biome_parameters.py" \
    --datagen "$reports/biome_parameters/minecraft" \
    --out "$report_tree/biome_parameters" \
    --game-version "$version" \
    --data-version "$data_version" || {
    echo "error: the biome parameter reports could not be packed" >&2
    exit 4
}
rm -rf "$out/reports"
cp -R "$report_tree" "$out/reports"
echo "  wrote the reports under '$out/reports'"

if [[ -f "$repo_root/content/vanilla/NOTICE.md" ]]; then
    cp "$repo_root/content/vanilla/NOTICE.md" "$out/NOTICE.md"
fi

# The protocol registry is the network registry catalog of the tracked content:
# it is versioned with the repository rather than derived from the server
# archive, so a source checkout installs its copy next to the generated data.
protocol_registry="$repo_root/content/vanilla/$version/protocol_registry.json"
if [[ ! -f "$out/protocol_registry.json" ]]; then
    if [[ -f "$protocol_registry" ]]; then
        cp "$protocol_registry" "$out/protocol_registry.json"
        echo "  installed the protocol registry"
    else
        echo "error: the checkout ships no '$protocol_registry'; copy it from a" \
            "release archive or generate the data there" >&2
        exit 3
    fi
fi

if [[ "$datapack" == 1 && ! -d "$out/data" ]]; then
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
    # Structure templates are binary assets the download path also keeps; the
    # template manager expects <root>/<namespace>/structure/<path>.nbt.
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

echo "generated the Minecraft $version game data in '$out'"

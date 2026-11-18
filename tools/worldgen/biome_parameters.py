#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Pack the data generator's biome parameters into the tracked report format.

The vanilla data generator writes its climate report as
`biome_parameters/minecraft/<dimension>.json`: a list of entries with a biome
id and the seven climate parameters as floats or float intervals. World
generation reads a compact report with the same entries quantized exactly as
the game compares them, one document per noise dimension:

    reports/biome_parameters/
      overworld.json   overworld climate parameter points, in selection order
      nether.json      nether climate parameter points, in selection order
      MANIFEST.json    game/data version, counts and sha256 of each report
      NOTICE.md        the game-derived data notice

Quantization mirrors `parse_biomes` in `refrain-tools vanilla import`: the
value is rounded to single precision, multiplied by 10000 in single precision
and truncated toward zero, so a generated report compares exactly like the
container the same reports produce.

Usage:
    biome_parameters.py --datagen generated/reports/biome_parameters/minecraft \
        --out content/vanilla/26.3/reports/biome_parameters \
        --game-version 26.3 --data-version 5023
    biome_parameters.py --check content/vanilla/26.3/reports/biome_parameters
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct

VERSION = "26.3"
DATA_VERSION = 5023

# Dimension id the loader records, keyed by the data generator's file stem.
DIMENSIONS = {
    "overworld": "minecraft:overworld",
    "nether": "minecraft:the_nether",
}

PARAMETERS = (
    "temperature",
    "humidity",
    "continentalness",
    "erosion",
    "depth",
    "weirdness",
)

NOTICE = """# Notice for biome parameter reports

The JSON documents in this directory are derived from Minecraft Java Edition
and are Copyright (c) Mojang Studios / Microsoft Corporation. They are **not**
covered by the MIT license that applies to the rest of this repository.

The tables record the climate parameter points that the game's overworld and
nether biome presets apply. They are included only so this server can
interoperate with a locally owned copy of the game.

See `../../../NOTICE.md` for the notice that covers all of `content/vanilla/`.
"""


def single_precision(value: float) -> float:
    """Round a double to the nearest single-precision float."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def quantize(value: float) -> int:
    """Quantize one climate coordinate the way the import does."""
    return int(single_precision(single_precision(value) * 10000.0))


def decode_range(value: object) -> list[int]:
    """Quantize a scalar point or a two-element interval into a pair."""
    if isinstance(value, list):
        if len(value) != 2:
            raise SystemExit("biome parameter interval is not two elements")
        return [quantize(float(value[0])), quantize(float(value[1]))]
    point = quantize(float(value))
    return [point, point]


def convert(
    dimension_file: pathlib.Path, game_version: str, data_version: int
) -> dict:
    """Convert one data generator report into the tracked document."""
    stem = dimension_file.stem
    dimension = DIMENSIONS.get(stem)
    if dimension is None:
        raise SystemExit(f"unexpected biome parameter report: {dimension_file}")
    try:
        raw = json.loads(dimension_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read {dimension_file}: {error}")
    biomes = raw.get("biomes")
    if not isinstance(biomes, list) or not biomes:
        raise SystemExit(f"{dimension_file} holds no biome entries")
    entries = []
    for index, entry in enumerate(biomes):
        biome = entry.get("biome")
        parameters = entry.get("parameters")
        if not isinstance(biome, str) or not isinstance(parameters, dict):
            raise SystemExit(
                f"{dimension_file} entry {index} has no biome or parameters"
            )
        converted = {"biome": biome}
        for name in PARAMETERS:
            if name not in parameters:
                raise SystemExit(
                    f"{dimension_file} entry {index} has no {name}"
                )
            converted[name] = decode_range(parameters[name])
        offset = parameters.get("offset")
        if offset is None:
            raise SystemExit(f"{dimension_file} entry {index} has no offset")
        converted["offset"] = quantize(float(offset))
        if dimension == "minecraft:the_nether":
            # The nether's noise router samples no weirdness (it is constant
            # zero), so vanilla's nether preset stores each entry's
            # distinguishing value in the offset field. The packed report keeps
            # it where selection compares it: weirdness, with offset zero.
            converted["weirdness"] = decode_range(offset)
            converted["offset"] = 0
        entries.append(converted)
    return {
        "format": 1,
        "game_version": game_version,
        "data_version": data_version,
        "dimension": dimension,
        "entry_count": len(entries),
        "entries": entries,
    }


def render(document: dict) -> bytes:
    """Render the tracked layout: a header object with one entry per line."""
    header = (
        "{\n"
        f'  "format": {document["format"]},\n'
        f'  "game_version": {json.dumps(document["game_version"])},\n'
        f'  "data_version": {document["data_version"]},\n'
        f'  "dimension": {json.dumps(document["dimension"])},\n'
        f'  "entry_count": {document["entry_count"]},\n'
        '  "entries": [\n'
    )
    lines = []
    for entry in document["entries"]:
        lines.append("    " + json.dumps(entry) + ",\n")
    if lines:
        lines[-1] = lines[-1][:-2] + "\n"
    return (header + "".join(lines) + "]\n}\n").encode("utf-8")


def check_report(directory: pathlib.Path) -> int:
    """Validate a packed report directory against its own manifest."""
    manifest_path = directory / "MANIFEST.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read {manifest_path}: {error}")
    if manifest.get("kind") != "biome_parameters":
        raise SystemExit(f"{manifest_path}: kind is not biome_parameters")
    files = manifest.get("files")
    expected = {"overworld.json", "nether.json"}
    if not isinstance(files, dict) or set(files) != expected:
        raise SystemExit(
            f"{manifest_path}: expected entries {sorted(expected)}"
        )
    game_version = manifest.get("game_version")
    data_version = manifest.get("data_version")
    total_entries = 0
    for name in sorted(expected):
        path = directory / name
        try:
            data = path.read_bytes()
        except OSError as error:
            raise SystemExit(f"cannot read {path}: {error}")
        metadata = files[name]
        if metadata.get("bytes") != len(data):
            raise SystemExit(f"{path}: size does not match its manifest")
        if metadata.get("sha256") != hashlib.sha256(data).hexdigest():
            raise SystemExit(f"{path}: digest does not match its manifest")
        document = json.loads(data)
        if document.get("format") != 1:
            raise SystemExit(f"{path}: format is not supported")
        if document.get("game_version") != game_version:
            raise SystemExit(f"{path}: game_version does not match the manifest")
        if document.get("data_version") != data_version:
            raise SystemExit(f"{path}: data_version does not match the manifest")
        dimension = DIMENSIONS[name.removesuffix(".json")]
        if document.get("dimension") != dimension:
            raise SystemExit(f"{path}: dimension is not {dimension}")
        entries = document.get("entries")
        if not isinstance(entries, list) or document.get("entry_count") != len(
            entries
        ):
            raise SystemExit(f"{path}: entry_count does not match entries")
        if metadata.get("entry_count") != len(entries):
            raise SystemExit(f"{path}: manifest entry_count does not match")
        if metadata.get("dimension") != dimension:
            raise SystemExit(f"{path}: manifest dimension does not match")
        total_entries += len(entries)
        for entry in entries:
            for field in (*PARAMETERS, "offset"):
                value = entry.get(field)
                if field == "offset":
                    if not isinstance(value, int) or isinstance(value, bool):
                        raise SystemExit(f"{path}: offset is not an integer")
                    continue
                if (
                    not isinstance(value, list)
                    or len(value) != 2
                    or any(
                        not isinstance(item, int) or isinstance(item, bool)
                        for item in value
                    )
                ):
                    raise SystemExit(
                        f"{path}: {field} is not an integer interval"
                    )
    if not (directory / "NOTICE.md").is_file():
        raise SystemExit(f"{directory}: NOTICE.md is missing")
    print(f"{directory}: {total_entries} entries OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--datagen", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    parser.add_argument("--game-version", default=VERSION)
    parser.add_argument("--data-version", type=int, default=DATA_VERSION)
    args = parser.parse_args()

    if args.check is not None:
        return check_report(args.check.resolve())
    if args.datagen is None or args.out is None:
        parser.error("--datagen and --out are required unless --check is used")
    datagen = args.datagen.resolve()
    if not datagen.is_dir():
        raise SystemExit(f"data generator reports not found: {datagen}")

    files: dict[str, dict] = {}
    args.out.mkdir(parents=True, exist_ok=True)
    for stem in DIMENSIONS:
        source = datagen / f"{stem}.json"
        if not source.is_file():
            raise SystemExit(f"the data generator wrote no {source.name}")
        document = convert(source, args.game_version, args.data_version)
        data = render(document)
        target = args.out / f"{stem}.json"
        target.write_bytes(data)
        files[target.name] = {
            "bytes": len(data),
            "dimension": document["dimension"],
            "entry_count": document["entry_count"],
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    manifest = {
        "data_version": args.data_version,
        "files": files,
        "game_version": args.game_version,
        "kind": "biome_parameters",
        "notice": "../../../NOTICE.md",
    }
    (args.out / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.out / "NOTICE.md").write_text(NOTICE, encoding="utf-8")
    print(
        f"{args.out}/overworld.json and nether.json "
        f"({sum(item['entry_count'] for item in files.values())} entries)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

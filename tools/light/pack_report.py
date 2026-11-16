#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Pack and validate the block light report of one content version.

The Java probe (`BlockLightProbe.java`) reads the light behaviour of every
block state from the user's own server jar. This tool checks that the probe
output covers exactly the states of the data generator's `blocks.json` report,
then writes the tracked report:

    reports/block_light/light.json       the per-state light table
    reports/block_light/MANIFEST.json    game version, counts and digests
    reports/block_light/NOTICE.md        the game-derived data notice

The report is game-derived data: it is not covered by the repository license
and is regenerated from the jar, never transcribed from a decompiled tree.

Usage:
    pack_report.py --probe probe.json --blocks reports/blocks.json \
        --server-jar /path/to/server.jar --out reports/block_light
    pack_report.py --check reports/block_light
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib

FORMAT = "refrain.block-light/1"
VERSION = "26.3"
DATA_VERSION = 5023

# A state that occludes and asks the lighting engine to use its shape is the
# only kind with an occlusion shape; the flags in the report carry both bits.
FLAG_CAN_OCCLUDE = 1
FLAG_USE_SHAPE = 2
BOTH_SHAPE_FLAGS = FLAG_CAN_OCCLUDE | FLAG_USE_SHAPE

MAX_BOXES_PER_FACE = 16
MAX_LIGHT_LEVEL = 15
# A shape may extend beyond its block cell (a piston head's arm does); the
# coverage test clips the face to the block, so the bound only rejects data
# that is clearly not a block shape.
COORD_MIN = -4.0
COORD_MAX = 5.0
COORD_EPSILON = 1.0e-9

NOTICE = """# Notice for game-derived data

The files in this directory are derived from Minecraft Java Edition and are

    Copyright (c) Mojang Studios / Microsoft Corporation.

They are **not** covered by the MIT license that applies to the rest of this
repository. No license is granted to copy, redistribute, sublicense or reuse
this data. It is included only so that this server can interoperate with a
locally owned copy of the game, and it should be removed if you do not own one.

`light.json` records, for every block state, the block light level the state
emits, the light levels a path loses when it enters the state, and the six face
shapes the lighting engine uses for shape-based light occlusion. `MANIFEST.json`
records the game version, the data version, the counts, the digest of the
report and the digest of the server archive it was extracted from.

The report is derived from the block registry of the jar named by
`MANIFEST.json`; it is not part of the repository license and is not shipped as
part of any game asset.
"""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fail(message: str) -> None:
    raise SystemExit(f"block-light report: {message}")


def load_blocks(path: pathlib.Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read '{path}': {error}")


def block_state_ids(blocks: dict) -> list[int]:
    ids: list[int] = []
    for name, block in blocks.items():
        states = block.get("states")
        if not isinstance(states, list) or not states:
            fail(f"block '{name}' has no states in the data generator report")
        for state in states:
            state_id = state.get("id")
            if not isinstance(state_id, int) or isinstance(state_id, bool):
                fail(f"block '{name}' has a state without an integer id")
            ids.append(state_id)
    return ids


def number(value: object, what: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        fail(f"{what} is not a number")
    result = float(value)
    if not math.isfinite(result):
        fail(f"{what} is not finite")
    return result


def integer(value: object, what: str, lower: int, upper: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        fail(f"{what} is not an integer")
    if value < lower or value > upper:
        fail(f"{what} is outside {lower}..{upper}")
    return value


def validate_shapes(probe: dict) -> list[list[list[list[float]]]]:
    shapes = probe.get("shapes")
    if not isinstance(shapes, list):
        fail("the probe output has no 'shapes' array")
    if len(shapes) != probe.get("shape_count"):
        fail("'shape_count' does not match the shapes array")
    if probe.get("faces") != [
        "down",
        "up",
        "north",
        "south",
        "west",
        "east",
    ]:
        fail("the probe output does not name the six face order")
    parsed: list[list[list[list[float]]]] = []
    for index, shape in enumerate(shapes):
        if not isinstance(shape, list) or len(shape) != 6:
            fail(f"shape {index} does not carry six face shapes")
        faces: list[list[list[float]]] = []
        for face_index, face in enumerate(shape):
            if not isinstance(face, list):
                fail(f"shape {index} face {face_index} is not a box list")
            if len(face) > MAX_BOXES_PER_FACE:
                fail(
                    f"shape {index} face {face_index} holds more than "
                    f"{MAX_BOXES_PER_FACE} boxes"
                )
            boxes: list[list[float]] = []
            for box_index, box in enumerate(face):
                if not isinstance(box, list) or len(box) != 6:
                    fail(
                        f"shape {index} face {face_index} box {box_index} "
                        "is not six coordinates"
                    )
                bounds = [
                    number(
                        value,
                        f"shape {index} face {face_index} box {box_index} "
                        "coordinate",
                    )
                    for value in box
                ]
                for value in bounds:
                    if value < COORD_MIN - COORD_EPSILON:
                        fail(
                            f"shape {index} face {face_index} box "
                            f"{box_index} is below the shape bounds"
                        )
                    if value > COORD_MAX + COORD_EPSILON:
                        fail(
                            f"shape {index} face {face_index} box "
                            f"{box_index} is above the shape bounds"
                        )
                if any(
                    bounds[axis] > bounds[axis + 3] for axis in range(3)
                ):
                    fail(
                        f"shape {index} face {face_index} box {box_index} "
                        "has min above max"
                    )
                boxes.append(bounds)
            faces.append(boxes)
        if not any(faces):
            fail(f"shape {index} is empty; an empty light shape is encoded as -1")
        parsed.append(faces)
    return parsed


def validate_states(
    probe: dict, shape_count: int, expected_ids: list[int]
) -> list[list[int]]:
    states = probe.get("states")
    if not isinstance(states, list):
        fail("the probe output has no 'states' array")
    if len(states) != probe.get("state_count"):
        fail("'state_count' does not match the states array")
    if len(states) != len(expected_ids):
        fail(
            f"the probe carries {len(states)} states but the data generator "
            f"report carries {len(expected_ids)}"
        )
    parsed: list[list[int]] = []
    referenced: set[int] = set()
    for position, row in enumerate(states):
        if not isinstance(row, list) or len(row) != 5:
            fail(f"state row {position} is not five values")
        state_id = integer(row[0], f"state row {position} id", 0, 1 << 16)
        emission = integer(
            row[1], f"state row {position} emission", 0, MAX_LIGHT_LEVEL
        )
        dampening = integer(
            row[2], f"state row {position} dampening", 0, MAX_LIGHT_LEVEL
        )
        flags = integer(row[3], f"state row {position} flags", 0, 3)
        shape = integer(
            row[4], f"state row {position} shape", -1, shape_count - 1
        )
        if state_id != position:
            fail(
                f"state row {position} has id {state_id}: ids must be dense "
                "and ascending"
            )
        if shape != -1 and flags != BOTH_SHAPE_FLAGS:
            fail(
                f"state row {position} has an occlusion shape but does not "
                "occlude with its shape"
            )
        if shape != -1:
            referenced.add(shape)
        parsed.append([state_id, emission, dampening, flags, shape])
    missing = set(range(shape_count)) - referenced
    if missing:
        fail(f"shapes {sorted(missing)} are never referenced by a state")
    if [row[0] for row in parsed] != list(range(len(parsed))):
        fail("state ids are not dense and ascending")
    if sorted(expected_ids) != list(range(len(expected_ids))):
        fail(
            "the data generator report's state ids are not dense; the probe "
            "cannot align its states with the report"
        )
    return parsed


def canonical_json(document: dict) -> bytes:
    return (
        json.dumps(document, separators=(",", ":"), ensure_ascii=False) + "\n"
    ).encode("utf-8")


def pack(args: argparse.Namespace) -> int:
    probe = json.loads(pathlib.Path(args.probe).read_text(encoding="utf-8"))
    blocks = load_blocks(pathlib.Path(args.blocks))

    block_count = probe.get("block_count")
    if block_count != len(blocks):
        fail(
            f"the probe carries {block_count} blocks but the data generator "
            f"report carries {len(blocks)}"
        )
    expected_ids = block_state_ids(blocks)
    shapes = validate_shapes(probe)
    states = validate_states(probe, len(shapes), expected_ids)

    document = {
        "format": FORMAT,
        "game_version": args.game_version,
        "data_version": args.data_version,
        "block_count": len(blocks),
        "state_count": len(states),
        "shape_count": len(shapes),
        "states": states,
        "shapes": shapes,
    }
    payload = canonical_json(document)
    light_path = pathlib.Path(args.out) / "light.json"
    light_path.parent.mkdir(parents=True, exist_ok=True)
    light_path.write_bytes(payload)

    jar_path = pathlib.Path(args.server_jar)
    if not jar_path.is_file():
        fail(f"server jar '{jar_path}' is not a file")
    manifest = {
        "data_version": args.data_version,
        "extractor": "tools/light/extract.sh",
        "files": {
            "light.json": {
                "block_count": document["block_count"],
                "bytes": len(payload),
                "format": FORMAT,
                "sha256": sha256(payload),
                "shape_count": document["shape_count"],
                "state_count": document["state_count"],
            }
        },
        "game_version": args.game_version,
        "kind": "block_light",
        "notice": "../../../NOTICE.md",
        "server_jar_sha256": sha256(jar_path.read_bytes()),
        "source": "official server.jar block registry",
    }
    (pathlib.Path(args.out) / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (pathlib.Path(args.out) / "NOTICE.md").write_text(NOTICE, encoding="utf-8")
    print(
        f"{light_path} ({len(states)} states, {len(shapes)} occlusion "
        f"shapes, {len(payload)} bytes)"
    )
    return 0


def check(path: pathlib.Path) -> int:
    manifest_path = path / "MANIFEST.json"
    light_path = path / "light.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read '{manifest_path}': {error}")
    try:
        payload = light_path.read_bytes()
        document = json.loads(payload)
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read '{light_path}': {error}")
    files = manifest.get("files", {})
    entry = files.get("light.json")
    if not isinstance(entry, dict):
        fail("the manifest does not describe 'light.json'")
    if entry.get("sha256") != sha256(payload):
        fail("'light.json' does not match the digest in the manifest")
    if entry.get("bytes") != len(payload):
        fail("'light.json' does not match the size in the manifest")
    if document.get("format") != FORMAT:
        fail("'light.json' has an unexpected format")
    if manifest.get("game_version") != document.get("game_version"):
        fail("the manifest and the report disagree on the game version")
    if manifest.get("data_version") != document.get("data_version"):
        fail("the manifest and the report disagree on the data version")
    for key in ("block_count", "state_count", "shape_count"):
        if entry.get(key) != document.get(key):
            fail(f"the manifest and the report disagree on '{key}'")
    if len(document.get("states", [])) != document.get("state_count"):
        fail("the report's state count does not match its state table")
    if len(document.get("shapes", [])) != document.get("shape_count"):
        fail("the report's shape count does not match its shape table")
    print(f"{path}: block light report is consistent")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--blocks", type=pathlib.Path)
    parser.add_argument("--server-jar", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--game-version", default=VERSION)
    parser.add_argument("--data-version", default=DATA_VERSION, type=int)
    parser.add_argument(
        "--check",
        type=pathlib.Path,
        metavar="REPORT_DIR",
        help="verify a tracked report against its manifest (no jar needed)",
    )
    args = parser.parse_args()
    if args.check is not None:
        return check(args.check)
    for name in ("probe", "blocks", "server_jar", "out"):
        if getattr(args, name) is None:
            parser.error(f"--{name.replace('_', '-')} is required without --check")
    return pack(args)


if __name__ == "__main__":
    raise SystemExit(main())

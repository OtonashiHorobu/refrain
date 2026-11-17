#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Pack and validate the block mining report of one content version.

The Java probe (`MiningProbe.java`) reads the destroy time, the tool
requirement and the tool component behaviour of every block and tool item from
the user's own server jar. This tool checks the probe output against the data
generator's block and item component reports, re-derives every compressed
field from the tool rules and block tags the probe carries, then writes the
tracked report:

    reports/block_mining/mining.json     the per-block and per-tool tables
    reports/block_mining/MANIFEST.json   game version, counts and digests
    reports/block_mining/NOTICE.md       the game-derived data notice

The report is game-derived data: it is not covered by the repository license
and is regenerated from the jar, never transcribed from a decompiled tree.

Usage:
    pack_report.py --probe probe.json --reports generated/reports \\
        --server-jar /path/to/server.jar --out reports/block_mining
    pack_report.py --check reports/block_mining
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import struct

FORMAT = "refrain.block-mining/1"
VERSION = "26.3"
DATA_VERSION = 5023

# Families of the report, in bit order. The block mask and the direct-correct
# mask of a block use the bit position of the family in this list.
TOOL_TYPES = ["pickaxe", "axe", "shovel", "hoe", "shears", "sword"]
TIER_TYPES = {"pickaxe", "axe", "shovel", "hoe"}
SWORD_BIT = 1 << TOOL_TYPES.index("sword")
MATERIALS = ["wood", "stone", "copper", "iron", "diamond", "netherite", "gold"]

FLAG_REQUIRES_CORRECT_TOOL = 1
FLAG_AIR = 2
FLAG_FLUID = 4
ALL_FLAGS = FLAG_REQUIRES_CORRECT_TOOL | FLAG_AIR | FLAG_FLUID
ALL_MASK_BITS = (1 << len(TOOL_TYPES)) - 1
SHEARS_OR_SWORD = (1 << TOOL_TYPES.index("shears")) | SWORD_BIT

NOTICE = """# Notice for game-derived data

The files in this directory are derived from Minecraft Java Edition and are

    Copyright (c) Mojang Studios / Microsoft Corporation.

They are **not** covered by the MIT license that applies to the rest of this
repository. No license is granted to copy, redistribute, sublicense or reuse
this data. It is included only so that this server can interoperate with a
locally owned copy of the game, and it should be removed if you do not own one.

`mining.json` records, for every block, the destroy time the game caches on
its states, the correct-tool-for-drops flag, the tool families that mine the
block efficiently, the drop tier it requires and the shears and sword mining
speeds; and for every item that carries a tool component, its family,
material, tier speed, damage per block, stack limit, durability and rules.
`MANIFEST.json` records the game version, the data version, the counts, the
digest of the report and the digest of the server archive it was extracted
from.

The report is derived from the block registry, the item registry and the
vanilla datapack tags of the jar named by `MANIFEST.json`; it is not part of
the repository license and is not shipped as part of any game asset.
"""


def fail(message: str) -> None:
    raise SystemExit(f"block-mining report: {message}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def f32(value: float) -> float:
    """Rounds a JSON number to the float32 the game uses."""
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def load_json(path: pathlib.Path, what: str) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read '{path}': {error}")


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


def text(value: object, what: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{what} is not a non-empty string")
    return value


def object_of(value: object, what: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{what} is not an object")
    return value


def array_of(value: object, what: str) -> list:
    if not isinstance(value, list):
        fail(f"{what} is not an array")
    return value


def rule_matches(rule: dict, block: str, tags: dict[str, list[str]]) -> bool:
    if "tag" in rule:
        return block in tags[rule["tag"]]
    return block in rule["blocks"]


def rule_speed(tool: dict, block: str, tags: dict[str, list[str]]) -> float:
    for rule in tool["rules"]:
        if "speed" in rule and rule_matches(rule, block, tags):
            return f32(rule["speed"])
    return f32(tool["default_mining_speed"])


def rule_correct(tool: dict, block: str, tags: dict[str, list[str]]) -> bool:
    for rule in tool["rules"]:
        if "correct_for_drops" in rule and rule_matches(rule, block, tags):
            return rule["correct_for_drops"]
    return False


def classify_tool(tool: dict) -> tuple[str, str | None]:
    """Re-derives the family and material of one tool from its rule list."""
    family = None
    material = None
    shears = False
    sword = False
    for rule in tool["rules"]:
        tag = rule.get("tag")
        if tag is None:
            continue
        path = tag.split(":", 1)[-1]
        if (
            rule.get("correct_for_drops") is True
            and "speed" in rule
            and path.startswith("mineable/")
        ):
            found = path[len("mineable/") :]
            if family is not None and family != found:
                fail(f"'{tool['id']}' mines efficiently with two families")
            family = found
        elif (
            rule.get("correct_for_drops") is False
            and path.startswith("incorrect_for_")
            and path.endswith("_tool")
        ):
            spelling = path[len("incorrect_for_") : -len("_tool")]
            material = {"wooden": "wood"}.get(spelling, spelling)
        elif path.startswith("shears_"):
            shears = True
        elif path.startswith("sword_"):
            sword = True
    if family is not None:
        if family not in TIER_TYPES or material is None:
            fail(f"'{tool['id']}' is not a tiered tool")
        return family, material
    if shears:
        if material is not None:
            fail(f"'{tool['id']}' is shears and names a material")
        return "shears", None
    if sword:
        if material is not None:
            fail(f"'{tool['id']}' is a sword and names a material")
        return "sword", None
    if tool["rules"]:
        fail(f"'{tool['id']}' carries a tool component of unknown shape")
    return "none", None


def validate_tags(probe: dict, block_names: set[str]) -> dict[str, list[str]]:
    tags = object_of(probe.get("tags"), "tags")
    parsed: dict[str, list[str]] = {}
    for name, entries in tags.items():
        text(name, "tag name")
        values = array_of(entries, f"tag '{name}'")
        parsed[name] = []
        previous = None
        for entry in values:
            block = text(entry, f"tag '{name}' entry")
            if block not in block_names:
                fail(f"tag '{name}' names unknown block '{block}'")
            if previous is not None and block <= previous:
                fail(f"tag '{name}' is not sorted and unique")
            previous = block
            parsed[name].append(block)
    return parsed


def validate_tools(
    probe: dict, tags: dict[str, list[str]], reports: pathlib.Path
) -> tuple[list[dict], dict[str, int]]:
    raw_tools = array_of(probe.get("tools"), "tools")
    if probe.get("tool_count") != len(raw_tools):
        fail("'tool_count' does not match the tools array")
    tools: list[dict] = []
    seen: set[str] = set()
    for index, raw in enumerate(raw_tools):
        tool = object_of(raw, f"tool {index}")
        item = text(tool.get("id"), f"tool {index} id")
        if item in seen:
            fail(f"duplicate tool '{item}'")
        seen.add(item)
        if tool.get("tool_type") not in TOOL_TYPES + ["none"]:
            fail(f"'{item}' has an unknown tool type")
        default_speed = number(
            tool.get("default_mining_speed"), f"'{item}' default mining speed"
        )
        if default_speed < 0.0:
            fail(f"'{item}' has a negative default mining speed")
        damage = integer(
            tool.get("damage_per_block"), f"'{item}' damage per block", 0, 65535
        )
        creative = tool.get("can_destroy_blocks_in_creative")
        if not isinstance(creative, bool):
            fail(f"'{item}' creative flag is not a bool")
        max_damage = integer(
            tool.get("max_damage"), f"'{item}' max damage", 0, 1 << 31
        )
        max_stack = integer(
            tool.get("max_stack_size"), f"'{item}' max stack size", 1, 99
        )
        rules = array_of(tool.get("rules"), f"'{item}' rules")
        parsed_rules: list[dict] = []
        for rule_index, raw_rule in enumerate(rules):
            rule = object_of(raw_rule, f"'{item}' rule {rule_index}")
            entry: dict = {}
            if "tag" in rule and "blocks" in rule:
                fail(f"'{item}' rule {rule_index} has two block sets")
            if "tag" in rule:
                tag = text(rule["tag"], f"'{item}' rule {rule_index} tag")
                if tag not in tags:
                    fail(f"'{item}' rule {rule_index} names unknown tag")
                entry["tag"] = tag
            elif "blocks" in rule:
                blocks = array_of(
                    rule["blocks"], f"'{item}' rule {rule_index} blocks"
                )
                entry["blocks"] = [
                    text(block, f"'{item}' rule {rule_index} block")
                    for block in blocks
                ]
                if entry["blocks"] != sorted(set(entry["blocks"])):
                    fail(f"'{item}' rule {rule_index} blocks are not sorted")
            else:
                fail(f"'{item}' rule {rule_index} has no block set")
            if "speed" in rule:
                speed = number(rule["speed"], f"'{item}' rule {rule_index} speed")
                if speed <= 0.0:
                    fail(f"'{item}' rule {rule_index} has a non-positive speed")
                entry["speed"] = f32(speed)
            if "correct_for_drops" in rule:
                correct = rule["correct_for_drops"]
                if not isinstance(correct, bool):
                    fail(
                        f"'{item}' rule {rule_index} correct flag is not a bool"
                    )
                entry["correct_for_drops"] = correct
            parsed_rules.append(entry)
        family, material = classify_tool({"id": item, "rules": parsed_rules})
        if family != tool.get("tool_type"):
            fail(f"'{item}' tool type does not match its rules")
        if family in TIER_TYPES:
            if tool.get("material") != material:
                fail(f"'{item}' material does not match its rules")
            level = integer(
                tool.get("harvest_level"), f"'{item}' harvest level", 0, 3
            )
            tier_speed = number(tool.get("tier_speed"), f"'{item}' tier speed")
            if f32(tier_speed) <= 0.0:
                fail(f"'{item}' has a non-positive tier speed")
            efficient = [
                rule
                for rule in parsed_rules
                if rule.get("correct_for_drops") is True
                and "speed" in rule
                and rule.get("tag", "").split(":", 1)[-1].startswith("mineable/")
            ]
            if len(efficient) != 1:
                fail(f"'{item}' does not have exactly one efficient rule")
            if f32(efficient[0]["speed"]) != f32(tier_speed):
                fail(f"'{item}' tier speed does not match its rules")
        else:
            if "material" in tool or "harvest_level" in tool:
                fail(f"'{item}' is not tiered but names a material")
            if "tier_speed" in tool:
                fail(f"'{item}' is not tiered but carries a tier speed")
            level = None
        tools.append(
            {
                "id": item,
                "tool_type": family,
                "material": material if family in TIER_TYPES else None,
                "harvest_level": level,
                "tier_speed": f32(tool["tier_speed"])
                if family in TIER_TYPES
                else None,
                "default_mining_speed": f32(default_speed),
                "damage_per_block": damage,
                "can_destroy_blocks_in_creative": creative,
                "max_damage": max_damage,
                "max_stack_size": max_stack,
                "rules": parsed_rules,
            }
        )

    # Item component reports are an independent serialization of the same
    # registry data; the probe must agree with them field by field.
    component_dir = reports / "minecraft" / "components" / "item"
    if not component_dir.is_dir():
        fail(f"missing item component directory: {component_dir}")
    item_components: dict[str, dict] = {}
    for path in sorted(component_dir.glob("*.json")):
        document = object_of(load_json(path, "item components"), str(path))
        components = object_of(
            document.get("components"), f"'{path.name}' components"
        )
        if "minecraft:tool" in components:
            item_components["minecraft:" + path.stem] = components
    tool_items = {tool["id"] for tool in tools}
    if tool_items != set(item_components):
        missing = sorted(item_components.keys() - tool_items)
        extra = sorted(tool_items - item_components)
        fail(f"tool items disagree with the item reports: {missing + extra}")

    for tool in tools:
        components = item_components[tool["id"]]
        expected_stack = integer(
            components.get("minecraft:max_stack_size", 64),
            f"'{tool['id']}' component stack size",
            1,
            99,
        )
        expected_damage = integer(
            components.get("minecraft:max_damage", 0),
            f"'{tool['id']}' component max damage",
            0,
            1 << 31,
        )
        if tool["max_stack_size"] != expected_stack:
            fail(f"'{tool['id']}' stack size does not match its components")
        if tool["max_damage"] != expected_damage:
            fail(f"'{tool['id']}' max damage does not match its components")
        component = object_of(
            components["minecraft:tool"], f"'{tool['id']}' tool component"
        )
        if f32(component.get("default_mining_speed", 1.0)) != f32(
            tool["default_mining_speed"]
        ):
            fail(f"'{tool['id']}' default mining speed differs from its report")
        if integer(
            component.get("damage_per_block", 1),
            f"'{tool['id']}' component damage per block",
            0,
            65535,
        ) != tool["damage_per_block"]:
            fail(f"'{tool['id']}' damage per block differs from its report")
        if component.get("can_destroy_blocks_in_creative", True) != tool[
            "can_destroy_blocks_in_creative"
        ]:
            fail(f"'{tool['id']}' creative flag differs from its report")
        expected_rules = [
            normalize_component_rule(rule) for rule in component.get("rules", [])
        ]
        if expected_rules != tool["rules"]:
            fail(f"'{tool['id']}' rules differ from its component report")

    return tools, material_levels(tools, tags)


def normalize_component_rule(rule: object) -> dict:
    entry = object_of(rule, "component rule")
    blocks = entry.get("blocks")
    parsed: dict = {}
    if isinstance(blocks, str):
        if blocks.startswith("#"):
            parsed["tag"] = blocks[1:]
        else:
            parsed["blocks"] = [blocks]
    elif isinstance(blocks, list):
        parsed["blocks"] = sorted(
            text(block, "component rule block") for block in blocks
        )
    else:
        fail("a tool component rule has no block set")
    if "speed" in entry:
        parsed["speed"] = f32(number(entry["speed"], "component rule speed"))
    if "correct_for_drops" in entry:
        if not isinstance(entry["correct_for_drops"], bool):
            fail("a tool component rule has a non-bool correct flag")
        parsed["correct_for_drops"] = entry["correct_for_drops"]
    return parsed


def material_levels(tools: list[dict], tags: dict[str, list[str]]) -> dict[str, int]:
    """Derives each material's harvest level from its incorrect-for tag set.

    The level is the number of distinct denial sets that strictly contain the
    material's own set: a material incorrect for more blocks sits below the
    material incorrect for fewer.
    """
    denied: dict[str, set[str]] = {}
    for tool in tools:
        material = tool["material"]
        if material is None or material in denied:
            continue
        for rule in tool["rules"]:
            if (
                "tag" in rule
                and rule.get("correct_for_drops") is False
                and rule["tag"].split(":", 1)[-1].startswith("incorrect_for_")
            ):
                denied[material] = set(tags[rule["tag"]])
                break
    if set(denied) != set(MATERIALS):
        fail(f"the report does not carry the seven tool materials: {denied}")
    distinct: list[set[str]] = []
    for blocks in denied.values():
        if blocks not in distinct:
            distinct.append(blocks)
    for outer in distinct:
        for inner in distinct:
            if len(outer) > len(inner) and not outer >= inner:
                fail("the incorrect-for tag sets are not nested")
    levels: dict[str, int] = {}
    for material, blocks in denied.items():
        levels[material] = sum(
            1
            for other in distinct
            if len(other) > len(blocks) and other >= blocks
        )
    return levels


def validate_blocks(
    probe: dict,
    blocks_document: dict,
    tools: list[dict],
    levels: dict[str, int],
    tags: dict[str, list[str]],
) -> list[list[object]]:
    block_names = set(blocks_document)
    state_count = 0
    for name, entry in blocks_document.items():
        states = array_of(
            object_of(entry, f"block '{name}'").get("states"),
            f"block '{name}' states",
        )
        if not states:
            fail(f"block '{name}' has no states")
        state_count += len(states)

    raw_blocks = array_of(probe.get("blocks"), "blocks")
    if probe.get("block_count") != len(raw_blocks):
        fail("'block_count' does not match the blocks array")
    if len(raw_blocks) != len(block_names):
        fail(
            f"the probe carries {len(raw_blocks)} blocks but the data "
            f"generator report carries {len(block_names)}"
        )
    if probe.get("state_count") != state_count:
        fail("'state_count' does not match the data generator report")

    efficient = {}
    shears = None
    sword = None
    for tool in tools:
        if tool["tool_type"] in TIER_TYPES:
            for rule in tool["rules"]:
                if (
                    rule.get("correct_for_drops") is True
                    and "speed" in rule
                    and rule.get("tag", "").split(":", 1)[-1].startswith(
                        "mineable/"
                    )
                ):
                    family = rule["tag"].split(":", 1)[-1][len("mineable/") :]
                    known = efficient.setdefault(family, rule["tag"])
                    if known != rule["tag"]:
                        fail("two items name different efficient tags")
        elif tool["tool_type"] == "shears":
            shears = tool
        elif tool["tool_type"] == "sword":
            sword = tool
    for family in TIER_TYPES:
        if family not in efficient:
            fail(f"no tool item mines efficiently with {family}")
    if shears is None or sword is None:
        fail("the report carries no shears or sword tool")

    deny_tags = {}
    for material in MATERIALS:
        for tool in tools:
            if tool["material"] != material:
                continue
            for rule in tool["rules"]:
                if (
                    "tag" in rule
                    and rule.get("correct_for_drops") is False
                    and rule["tag"].split(":", 1)[-1].startswith(
                        "incorrect_for_"
                    )
                ):
                    deny_tags[material] = rule["tag"]
        if material not in deny_tags:
            fail(f"no incorrect-for tag for material {material}")

    parsed: list[list[object]] = []
    seen: set[str] = set()
    for position, raw in enumerate(raw_blocks):
        row = array_of(raw, f"block row {position}")
        if len(row) != 8:
            fail(f"block row {position} is not eight values")
        name = text(row[0], f"block row {position} name")
        if name in seen:
            fail(f"duplicate block '{name}'")
        seen.add(name)
        if name not in block_names:
            fail(f"block row names unknown block '{name}'")
        hardness = number(row[1], f"'{name}' hardness")
        if not (f32(hardness) == -1.0 or f32(hardness) >= 0.0):
            fail(f"'{name}' has an invalid hardness")
        flags = integer(row[2], f"'{name}' flags", 0, ALL_FLAGS)
        mask = integer(row[3], f"'{name}' tool mask", 0, ALL_MASK_BITS)
        if mask & SWORD_BIT:
            fail(f"'{name}' names the sword as an efficient family")
        needed = integer(
            row[4], f"'{name}' needed tier", 0, len(MATERIALS)
        )
        shears_speed = f32(number(row[5], f"'{name}' shears speed"))
        sword_speed = f32(number(row[6], f"'{name}' sword speed"))
        direct = integer(row[7], f"'{name}' direct-correct mask", 0, ALL_MASK_BITS)
        if direct & ~SHEARS_OR_SWORD:
            fail(f"'{name}' has a direct-correct family outside shears/sword")

        expected_mask = 0
        for family, tag in efficient.items():
            if name in tags[tag]:
                expected_mask |= 1 << TOOL_TYPES.index(family)
        if mask != expected_mask:
            fail(f"'{name}' tool mask does not match the block tags")

        expected_needed = 0
        for material, tag in deny_tags.items():
            if name in tags[tag]:
                expected_needed = max(expected_needed, levels[material] + 1)
        if needed > max(levels.values()):
            fail(f"'{name}' needs a tier the report cannot carry")
        if needed != expected_needed:
            fail(f"'{name}' needed tier does not match its tags")

        if f32(rule_speed(shears, name, tags)) != shears_speed:
            fail(f"'{name}' shears speed does not match the shears rules")
        if f32(rule_speed(sword, name, tags)) != sword_speed:
            fail(f"'{name}' sword speed does not match the sword rules")
        expected_direct = 0
        if rule_correct(shears, name, tags):
            expected_direct |= 1 << TOOL_TYPES.index("shears")
        if rule_correct(sword, name, tags):
            expected_direct |= SWORD_BIT
        if direct != expected_direct:
            fail(f"'{name}' direct-correct mask does not match the sword rules")

        parsed.append(
            [
                name,
                f32(hardness),
                flags,
                mask,
                needed,
                shears_speed,
                sword_speed,
                direct,
            ]
        )
    if seen != block_names:
        fail("the probe does not cover every block of the data generator report")
    return parsed


def validate_probe(
    probe: dict,
    blocks_document: dict,
    reports: pathlib.Path,
) -> dict:
    if probe.get("tool_types") != TOOL_TYPES:
        fail("the probe does not name the tool families of the report format")
    block_names = set(blocks_document)
    tags = validate_tags(probe, block_names)
    tools, levels = validate_tools(probe, tags, reports)
    if sorted(levels) != sorted(MATERIALS):
        fail("the probe does not carry the seven tool materials")
    for tool in tools:
        if tool["material"] is None:
            continue
        if tool["harvest_level"] != levels[tool["material"]]:
            fail(
                f"'{tool['id']}' harvest level does not match the "
                "incorrect-for tag sets"
            )
    max_level = max(levels.values())
    for level in levels.values():
        integer(level, "a material harvest level", 0, max_level)
    blocks = validate_blocks(probe, blocks_document, tools, levels, tags)

    materials = []
    for name in MATERIALS:
        materials.append({"name": name, "harvest_level": levels[name]})

    # The report carries the tools as records; the fields are emitted below
    # from the validated values so an inconsistent probe cannot smuggle one
    # through.
    return {
        "block_count": probe["block_count"],
        "state_count": probe["state_count"],
        "tool_count": probe["tool_count"],
        "tool_types": TOOL_TYPES,
        "materials": materials,
        "blocks": blocks,
        "tools": [
            {
                "id": tool["id"],
                "tool_type": tool["tool_type"],
                **(
                    {
                        "material": tool["material"],
                        "harvest_level": tool["harvest_level"],
                        "tier_speed": tool["tier_speed"],
                    }
                    if tool["material"] is not None
                    else {}
                ),
                "default_mining_speed": tool["default_mining_speed"],
                "damage_per_block": tool["damage_per_block"],
                "can_destroy_blocks_in_creative": tool[
                    "can_destroy_blocks_in_creative"
                ],
                "max_damage": tool["max_damage"],
                "max_stack_size": tool["max_stack_size"],
                "rules": tool["rules"],
            }
            for tool in tools
        ],
        "tags": {name: tags[name] for name in sorted(tags)},
    }


def canonical_json(document: dict) -> bytes:
    return (
        json.dumps(document, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")


def pack(args: argparse.Namespace) -> int:
    probe = object_of(
        load_json(pathlib.Path(args.probe), "probe"), "probe output"
    )
    blocks = object_of(
        load_json(pathlib.Path(args.blocks), "blocks"), "blocks.json"
    )
    if probe.get("format") is not None:
        fail("the probe output must not name the report format")
    document = validate_probe(probe, blocks, pathlib.Path(args.reports))
    document["format"] = FORMAT
    document["game_version"] = args.game_version
    document["data_version"] = args.data_version
    payload = canonical_json(document)
    mining_path = pathlib.Path(args.out) / "mining.json"
    mining_path.parent.mkdir(parents=True, exist_ok=True)
    mining_path.write_bytes(payload)

    jar_path = pathlib.Path(args.server_jar)
    if not jar_path.is_file():
        fail(f"server jar '{jar_path}' is not a file")
    manifest = {
        "data_version": args.data_version,
        "extractor": "tools/block/extract.sh",
        "files": {
            "mining.json": {
                "block_count": document["block_count"],
                "bytes": len(payload),
                "format": FORMAT,
                "sha256": sha256(payload),
                "state_count": document["state_count"],
                "tool_count": document["tool_count"],
            }
        },
        "game_version": args.game_version,
        "kind": "block_mining",
        "notice": "../../../NOTICE.md",
        "server_jar_sha256": sha256(jar_path.read_bytes()),
        "source": "official server.jar block registry, item components and block tags",
    }
    (pathlib.Path(args.out) / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (pathlib.Path(args.out) / "NOTICE.md").write_text(NOTICE, encoding="utf-8")
    print(
        f"{mining_path} ({document['block_count']} blocks, "
        f"{document['tool_count']} tools, {len(payload)} bytes)"
    )
    return 0


def check(path: pathlib.Path) -> int:
    manifest_path = path / "MANIFEST.json"
    mining_path = path / "mining.json"
    manifest = object_of(
        load_json(manifest_path, "manifest"), "manifest"
    )
    payload = b""
    try:
        payload = mining_path.read_bytes()
        document = json.loads(payload)
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read '{mining_path}': {error}")
    document = object_of(document, "mining.json")
    files = object_of(manifest.get("files"), "manifest files")
    entry = object_of(files.get("mining.json"), "manifest mining.json entry")
    if entry.get("sha256") != sha256(payload):
        fail("'mining.json' does not match the digest in the manifest")
    if entry.get("bytes") != len(payload):
        fail("'mining.json' does not match the size in the manifest")
    if document.get("format") != FORMAT:
        fail("'mining.json' has an unexpected format")
    if manifest.get("game_version") != document.get("game_version"):
        fail("the manifest and the report disagree on the game version")
    if manifest.get("data_version") != document.get("data_version"):
        fail("the manifest and the report disagree on the data version")
    if document.get("tool_types") != TOOL_TYPES:
        fail("'mining.json' does not name the tool families of its format")
    if [material.get("name") for material in document.get("materials", [])] != MATERIALS:
        fail("'mining.json' does not carry the seven tool materials")
    for key in ("block_count", "state_count", "tool_count"):
        if entry.get(key) != document.get(key):
            fail(f"the manifest and the report disagree on '{key}'")
    if len(document.get("blocks", [])) != document.get("block_count"):
        fail("the report's block count does not match its block table")
    if len(document.get("tools", [])) != document.get("tool_count"):
        fail("the report's tool count does not match its tool table")
    print(f"{path}: block mining report is consistent")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--blocks", type=pathlib.Path)
    parser.add_argument("--reports", type=pathlib.Path)
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
    for name in ("probe", "blocks", "reports", "server_jar", "out"):
        if getattr(args, name) is None:
            parser.error(f"--{name.replace('_', '-')} is required without --check")
    return pack(args)


if __name__ == "__main__":
    raise SystemExit(main())

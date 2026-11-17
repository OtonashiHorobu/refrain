#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Negative tests for the block mining report packer.

The packer accepts a raw probe dump and writes the tracked report, so every
inconsistent dump must be rejected before it reaches the repository. The
fixture below is synthetic: it is small enough to read and it exercises the
same field layout as the real report.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).with_name("pack_report.py")

PICKAXE_BLOCKS = [
    "minecraft:diamond_ore",
    "minecraft:iron_ore",
    "minecraft:obsidian",
    "minecraft:stone",
]
DENIED = {
    "wood": ["minecraft:diamond_ore", "minecraft:iron_ore", "minecraft:obsidian"],
    "gold": ["minecraft:diamond_ore", "minecraft:iron_ore", "minecraft:obsidian"],
    "stone": ["minecraft:diamond_ore", "minecraft:obsidian"],
    "copper": ["minecraft:diamond_ore", "minecraft:obsidian"],
    "iron": ["minecraft:obsidian"],
    "diamond": [],
    "netherite": [],
}
LEVELS = {
    "wood": 0,
    "gold": 0,
    "stone": 1,
    "copper": 1,
    "iron": 2,
    "diamond": 3,
    "netherite": 3,
}
MATERIAL_ORDER = ["wood", "stone", "copper", "iron", "diamond", "netherite", "gold"]
TIER_SPEEDS = {
    "wood": 2.0,
    "stone": 4.0,
    "copper": 5.0,
    "iron": 6.0,
    "diamond": 8.0,
    "netherite": 9.0,
    "gold": 12.0,
}

TOOL_TYPES = ["pickaxe", "axe", "shovel", "hoe", "shears", "sword"]


def bit(family: str) -> int:
    return 1 << TOOL_TYPES.index(family)


def tool_rule(deny_tag: str, efficient_tag: str, speed: float) -> list[dict]:
    return [
        {"tag": deny_tag, "correct_for_drops": False},
        {"tag": efficient_tag, "speed": speed, "correct_for_drops": True},
    ]


def valid_probe() -> dict:
    tags = {
        "minecraft:incorrect_for_wooden_tool": DENIED["wood"],
        "minecraft:incorrect_for_stone_tool": DENIED["stone"],
        "minecraft:incorrect_for_copper_tool": DENIED["copper"],
        "minecraft:incorrect_for_iron_tool": DENIED["iron"],
        "minecraft:incorrect_for_diamond_tool": [],
        "minecraft:incorrect_for_netherite_tool": [],
        "minecraft:incorrect_for_gold_tool": DENIED["gold"],
        "minecraft:mineable/pickaxe": list(PICKAXE_BLOCKS),
        "minecraft:mineable/axe": [],
        "minecraft:mineable/shovel": [],
        "minecraft:mineable/hoe": ["minecraft:oak_leaves"],
        "minecraft:shears_extreme_breaking_speed": ["minecraft:oak_leaves"],
        "minecraft:sword_efficient": ["minecraft:oak_leaves"],
    }
    tools: list[dict] = []
    for material in MATERIAL_ORDER:
        tag = {
            "wood": "minecraft:incorrect_for_wooden_tool",
            "stone": "minecraft:incorrect_for_stone_tool",
            "copper": "minecraft:incorrect_for_copper_tool",
            "iron": "minecraft:incorrect_for_iron_tool",
            "diamond": "minecraft:incorrect_for_diamond_tool",
            "netherite": "minecraft:incorrect_for_netherite_tool",
            "gold": "minecraft:incorrect_for_gold_tool",
        }[material]
        prefix = "wooden" if material == "wood" else material
        tools.append(
            {
                "id": f"minecraft:{prefix}_pickaxe",
                "tool_type": "pickaxe",
                "material": material,
                "harvest_level": LEVELS[material],
                "tier_speed": TIER_SPEEDS[material],
                "default_mining_speed": 1.0,
                "damage_per_block": 1,
                "can_destroy_blocks_in_creative": True,
                "max_damage": 100,
                "max_stack_size": 1,
                "rules": tool_rule(tag, "minecraft:mineable/pickaxe", TIER_SPEEDS[material]),
            }
        )
    for family, tag in [
        ("axe", "minecraft:mineable/axe"),
        ("shovel", "minecraft:mineable/shovel"),
        ("hoe", "minecraft:mineable/hoe"),
    ]:
        tools.append(
            {
                "id": f"minecraft:wooden_{family}",
                "tool_type": family,
                "material": "wood",
                "harvest_level": 0,
                "tier_speed": 2.0,
                "default_mining_speed": 1.0,
                "damage_per_block": 1,
                "can_destroy_blocks_in_creative": True,
                "max_damage": 100,
                "max_stack_size": 1,
                "rules": tool_rule(
                    "minecraft:incorrect_for_wooden_tool", tag, 2.0
                ),
            }
        )
    tools.append(
        {
            "id": "minecraft:shears",
            "tool_type": "shears",
            "default_mining_speed": 1.0,
            "damage_per_block": 1,
            "can_destroy_blocks_in_creative": True,
            "max_damage": 238,
            "max_stack_size": 1,
            "rules": [
                {
                    "blocks": ["minecraft:cobweb"],
                    "speed": 15.0,
                    "correct_for_drops": True,
                },
                {
                    "tag": "minecraft:shears_extreme_breaking_speed",
                    "speed": 15.0,
                },
            ],
        }
    )
    tools.append(
        {
            "id": "minecraft:wooden_sword",
            "tool_type": "sword",
            "default_mining_speed": 1.0,
            "damage_per_block": 2,
            "can_destroy_blocks_in_creative": False,
            "max_damage": 59,
            "max_stack_size": 1,
            "rules": [
                {
                    "blocks": ["minecraft:cobweb"],
                    "speed": 15.0,
                    "correct_for_drops": True,
                },
                {"tag": "minecraft:sword_efficient", "speed": 1.5},
            ],
        }
    )
    tools.sort(key=lambda tool: tool["id"])
    blocks = [
        ["minecraft:cobweb", 4.0, 1, 0, 0, 15.0, 15.0, 48],
        ["minecraft:diamond_ore", 3.0, 1, 1, 2, 1.0, 1.0, 0],
        ["minecraft:iron_ore", 3.0, 1, 1, 1, 1.0, 1.0, 0],
        ["minecraft:oak_leaves", 0.2, 0, 8, 0, 15.0, 1.5, 0],
        ["minecraft:obsidian", 50.0, 1, 1, 3, 1.0, 1.0, 0],
        ["minecraft:stone", 1.5, 1, 1, 0, 1.0, 1.0, 0],
    ]
    return {
        "block_count": len(blocks),
        "state_count": len(blocks),
        "tool_count": len(tools),
        "tool_types": TOOL_TYPES,
        "materials": [
            {"name": material, "harvest_level": LEVELS[material]}
            for material in MATERIAL_ORDER
        ],
        "blocks": blocks,
        "tools": tools,
        "tags": tags,
    }


def valid_blocks() -> dict:
    names = [
        "minecraft:cobweb",
        "minecraft:diamond_ore",
        "minecraft:iron_ore",
        "minecraft:oak_leaves",
        "minecraft:obsidian",
        "minecraft:stone",
    ]
    return {
        name: {"states": [{"id": index, "default": True}]}
        for index, name in enumerate(names)
    }


def tool_component(tool: dict) -> dict:
    rules = []
    for rule in tool["rules"]:
        component: dict = {}
        if "tag" in rule:
            component["blocks"] = "#" + rule["tag"]
        else:
            component["blocks"] = rule["blocks"][0]
        if "speed" in rule:
            component["speed"] = rule["speed"]
        if "correct_for_drops" in rule:
            component["correct_for_drops"] = rule["correct_for_drops"]
        rules.append(component)
    components = {
        "minecraft:max_damage": tool["max_damage"],
        "minecraft:max_stack_size": tool["max_stack_size"],
        "minecraft:tool": {
            "rules": rules,
            "default_mining_speed": tool["default_mining_speed"],
            "damage_per_block": tool["damage_per_block"],
            "can_destroy_blocks_in_creative": tool[
                "can_destroy_blocks_in_creative"
            ],
        },
    }
    return {"components": components}


def run_tool(arguments: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


class PackReportTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._temporary.name)
        self.reports = self.root / "reports"
        self.reports.mkdir()
        self.jar = self.root / "server.jar"
        self.jar.write_bytes(b"synthetic jar bytes")
        self.out = self.root / "block_mining"

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def write_probe(self, probe: dict) -> pathlib.Path:
        path = self.root / "probe.json"
        path.write_text(json.dumps(probe), encoding="utf-8")
        return path

    def write_reports(self, probe: dict) -> None:
        (self.reports / "blocks.json").write_text(
            json.dumps(valid_blocks()), encoding="utf-8"
        )
        component_dir = self.reports / "minecraft" / "components" / "item"
        component_dir.mkdir(parents=True)
        for tool in probe["tools"]:
            path = component_dir / (tool["id"].split(":", 1)[1] + ".json")
            path.write_text(json.dumps(tool_component(tool)), encoding="utf-8")

    def pack(self, probe: dict | None = None, reports_probe: dict | None = None):
        value = probe if probe is not None else valid_probe()
        self.write_reports(reports_probe if reports_probe is not None else value)
        return run_tool(
            [
                "--probe",
                str(self.write_probe(value)),
                "--blocks",
                str(self.reports / "blocks.json"),
                "--reports",
                str(self.reports),
                "--server-jar",
                str(self.jar),
                "--out",
                str(self.out),
            ]
        )

    def test_writes_the_tracked_report_and_manifest(self) -> None:
        result = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads((self.out / "mining.json").read_text())
        self.assertEqual(document["format"], "refrain.block-mining/1")
        self.assertEqual(document["block_count"], 6)
        self.assertEqual(document["tool_count"], len(valid_probe()["tools"]))
        stone = next(
            row for row in document["blocks"] if row[0] == "minecraft:stone"
        )
        self.assertEqual(stone, ["minecraft:stone", 1.5, 1, 1, 0, 1.0, 1.0, 0])
        obsidian = next(
            row for row in document["blocks"] if row[0] == "minecraft:obsidian"
        )
        self.assertEqual(obsidian[1], 50.0)
        self.assertEqual(obsidian[4], 3)
        manifest = json.loads((self.out / "MANIFEST.json").read_text())
        self.assertEqual(manifest["kind"], "block_mining")
        self.assertEqual(manifest["files"]["mining.json"]["block_count"], 6)
        self.assertTrue((self.out / "NOTICE.md").is_file())
        check = run_tool(["--check", str(self.out)])
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_rejects_a_block_count_that_does_not_match_the_blocks_report(
        self,
    ) -> None:
        probe = valid_probe()
        probe["blocks"].pop()
        probe["block_count"] -= 1
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("block", result.stderr)

    def test_rejects_a_missing_block(self) -> None:
        probe = valid_probe()
        probe["blocks"] = [
            row for row in probe["blocks"] if row[0] != "minecraft:obsidian"
        ]
        probe["block_count"] -= 1
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("block", result.stderr)

    def test_rejects_an_invalid_hardness(self) -> None:
        probe = valid_probe()
        probe["blocks"][0][1] = -2.0
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hardness", result.stderr)

    def test_rejects_a_tool_mask_that_disagrees_with_the_tags(self) -> None:
        probe = valid_probe()
        probe["blocks"][0][3] = bit("pickaxe")
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mask", result.stderr)

    def test_rejects_a_needed_tier_that_disagrees_with_the_tags(self) -> None:
        probe = valid_probe()
        probe["blocks"][1][4] = 0
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tier", result.stderr)

    def test_rejects_a_shears_speed_that_disagrees_with_the_rules(self) -> None:
        probe = valid_probe()
        probe["blocks"][2][5] = 5.0
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("shears", result.stderr)

    def test_rejects_a_tool_type_that_disagrees_with_the_rules(self) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:wooden_pickaxe"
        )["tool_type"] = "axe"
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tool type", result.stderr)

    def test_rejects_a_tier_speed_that_disagrees_with_the_rules(self) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:iron_pickaxe"
        )["tier_speed"] = 7.0
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tier speed", result.stderr)

    def test_rejects_a_harvest_level_that_disagrees_with_the_tags(self) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:iron_pickaxe"
        )["harvest_level"] = 3
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("harvest level", result.stderr)

    def test_rejects_tool_rules_that_disagree_with_the_component_reports(
        self,
    ) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:shears"
        )["rules"][1]["speed"] = 45.0
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rules", result.stderr)

    def test_rejects_a_stack_size_that_disagrees_with_the_component_reports(
        self,
    ) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:wooden_sword"
        )["max_stack_size"] = 2
        result = self.pack(probe, reports_probe=valid_probe())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stack", result.stderr)

    def test_rejects_a_block_row_that_is_not_eight_values(self) -> None:
        probe = valid_probe()
        probe["blocks"][0].append(0)
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("eight", result.stderr)

    def test_rejects_an_unknown_material(self) -> None:
        probe = valid_probe()
        next(
            tool
            for tool in probe["tools"]
            if tool["id"] == "minecraft:iron_pickaxe"
        )["material"] = "silver"
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)

    def test_check_rejects_a_tampered_report(self) -> None:
        result = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        mining = self.out / "mining.json"
        mining.write_text(
            mining.read_text().replace('"block_count":6', '"block_count":7')
        )
        check = run_tool(["--check", str(self.out)])
        self.assertNotEqual(check.returncode, 0)
        self.assertIn("digest", check.stderr)


if __name__ == "__main__":
    unittest.main()

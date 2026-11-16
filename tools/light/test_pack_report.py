#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Negative tests for the block light report packer.

The packer accepts a raw probe dump and writes the tracked report, so every
inconsistent dump must be rejected before it reaches the repository.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).with_name("pack_report.py")


def valid_probe() -> dict:
    return {
        "block_count": 2,
        "state_count": 3,
        "shape_count": 1,
        "states": [
            [0, 0, 15, 1, -1],
            [1, 14, 0, 3, 0],
            [2, 0, 1, 1, -1],
        ],
        "faces": ["down", "up", "north", "south", "west", "east"],
        "shapes": [[[], [[0.0, 0.0, 0.0, 1.0, 0.5, 1.0]], [], [], [], []]],
    }


def valid_blocks() -> dict:
    return {
        "minecraft:stone": {"states": [{"id": 0, "default": True}]},
        "minecraft:torch": {
            "properties": {"lit": ["false", "true"]},
            "states": [
                {"id": 1, "properties": {"lit": "false"}},
                {"id": 2, "properties": {"lit": "true"}},
            ],
        },
    }


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
        self.out = self.root / "block_light"

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def write_probe(self, probe: dict) -> pathlib.Path:
        path = self.root / "probe.json"
        path.write_text(json.dumps(probe), encoding="utf-8")
        return path

    def write_blocks(self, blocks: dict) -> pathlib.Path:
        path = self.reports / "blocks.json"
        path.write_text(json.dumps(blocks), encoding="utf-8")
        return path

    def pack(self, probe: dict | None = None, blocks: dict | None = None):
        return run_tool(
            [
                "--probe",
                str(self.write_probe(probe if probe is not None else valid_probe())),
                "--blocks",
                str(self.write_blocks(blocks if blocks is not None else valid_blocks())),
                "--server-jar",
                str(self.jar),
                "--out",
                str(self.out),
            ]
        )

    def test_writes_the_tracked_report_and_manifest(self) -> None:
        result = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads((self.out / "light.json").read_text())
        self.assertEqual(document["format"], "refrain.block-light/1")
        self.assertEqual(document["state_count"], 3)
        self.assertEqual(document["states"][1], [1, 14, 0, 3, 0])
        self.assertEqual(
            document["shapes"],
            [[[], [[0.0, 0.0, 0.0, 1.0, 0.5, 1.0]], [], [], [], []]],
        )
        manifest = json.loads((self.out / "MANIFEST.json").read_text())
        self.assertEqual(manifest["kind"], "block_light")
        self.assertEqual(
            manifest["files"]["light.json"]["state_count"], 3
        )
        self.assertTrue((self.out / "NOTICE.md").is_file())
        check = run_tool(["--check", str(self.out)])
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_rejects_a_state_count_that_does_not_match_the_blocks_report(
        self,
    ) -> None:
        result = self.pack(
            valid_probe(),
            {
                "minecraft:stone": {"states": [{"id": 0}]},
                "minecraft:torch": {"states": [{"id": 1}]},
            },
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("states", result.stderr)

    def test_rejects_an_emission_above_the_light_range(self) -> None:
        probe = valid_probe()
        probe["states"][0][1] = 16
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("emission", result.stderr)

    def test_rejects_a_shape_index_out_of_range(self) -> None:
        probe = valid_probe()
        probe["states"][1][4] = 7
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("shape", result.stderr)

    def test_rejects_a_shape_on_a_state_that_does_not_use_it(self) -> None:
        probe = valid_probe()
        probe["states"][0][3] = 1
        probe["states"][0][4] = 0
        probe["states"][1][4] = -1
        probe["states"][2][4] = -1
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("occlude", result.stderr)

    def test_rejects_an_occlusion_box_outside_the_block(self) -> None:
        probe = valid_probe()
        probe["shapes"][0][1][0][3] = 9.5
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("shape", result.stderr)

    def test_rejects_an_unreferenced_shape(self) -> None:
        probe = valid_probe()
        probe["shape_count"] = 2
        probe["shapes"].append(
            [[], [[0.0, 0.0, 0.0, 1.0, 1.0, 1.0]], [], [], [], []]
        )
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("never referenced", result.stderr)

    def test_rejects_non_dense_state_ids(self) -> None:
        probe = valid_probe()
        probe["states"][2][0] = 7
        result = self.pack(probe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dense", result.stderr)

    def test_check_rejects_a_tampered_report(self) -> None:
        result = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        light = self.out / "light.json"
        light.write_text(light.read_text().replace('"state_count":3', '"state_count":4'))
        check = run_tool(["--check", str(self.out)])
        self.assertNotEqual(check.returncode, 0)
        self.assertIn("digest", check.stderr)


if __name__ == "__main__":
    unittest.main()

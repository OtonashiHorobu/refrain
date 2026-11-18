#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Tests for the biome parameter report packer.

The packer turns the data generator's float reports into the tracked quantized
layout, so the tests pin the quantization, the nether field mapping and the
manifest digests without any game data.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).with_name("biome_parameters.py")


def run_tool(datagen: pathlib.Path, out: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--datagen",
            str(datagen),
            "--out",
            str(out),
        ],
        capture_output=True,
        text=True,
    )


def run_check(directory: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), "--check", str(directory)],
        capture_output=True,
        text=True,
    )


def write_datagen(root: pathlib.Path, overworld: dict, nether: dict) -> pathlib.Path:
    root.mkdir(parents=True, exist_ok=True)
    (root / "overworld.json").write_text(json.dumps(overworld), encoding="utf-8")
    (root / "nether.json").write_text(json.dumps(nether), encoding="utf-8")
    return root


def point_biome(biome: str, **values: object) -> dict:
    return {
        "biome": biome,
        "parameters": {
            "temperature": 0.0,
            "humidity": 0.0,
            "continentalness": 0.0,
            "erosion": 0.0,
            "depth": 0.0,
            "weirdness": 0.0,
            "offset": 0.0,
            **values,
        },
    }


class BiomeParametersTest(unittest.TestCase):
    def test_quantizes_scalars_and_intervals(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {
                    "biomes": [
                        point_biome(
                            "minecraft:plains",
                            temperature=[-1.2, -1.05],
                            continentalness=0.4,
                            weirdness=-0.5,
                            offset=0.175,
                        )
                    ]
                },
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            result = run_tool(datagen, root / "packed")
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(
                (root / "packed" / "overworld.json").read_text(encoding="utf-8")
            )
            self.assertEqual(document["format"], 1)
            self.assertEqual(document["dimension"], "minecraft:overworld")
            self.assertEqual(document["entry_count"], 1)
            entry = document["entries"][0]
            self.assertEqual(entry["temperature"], [-12000, -10500])
            self.assertEqual(entry["continentalness"], [4000, 4000])
            self.assertEqual(entry["weirdness"], [-5000, -5000])
            self.assertEqual(entry["offset"], 1750)

    def test_quantizes_in_single_precision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {
                    "biomes": [
                        point_biome(
                            "minecraft:plains",
                            temperature=[-1.05, -1.05],
                        )
                    ]
                },
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            result = run_tool(datagen, root / "packed")
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(
                (root / "packed" / "overworld.json").read_text(encoding="utf-8")
            )
            # Double-precision arithmetic would truncate to -10499 here.
            self.assertEqual(document["entries"][0]["temperature"], [-10500, -10500])

    def test_moves_the_nether_offset_into_weirdness(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {"biomes": [point_biome("minecraft:plains")]},
                {
                    "biomes": [
                        point_biome(
                            "minecraft:warped_forest",
                            humidity=0.5,
                            offset=0.375,
                        )
                    ]
                },
            )
            result = run_tool(datagen, root / "packed")
            self.assertEqual(result.returncode, 0, result.stderr)
            document = json.loads(
                (root / "packed" / "nether.json").read_text(encoding="utf-8")
            )
            entry = document["entries"][0]
            self.assertEqual(entry["weirdness"], [3750, 3750])
            self.assertEqual(entry["offset"], 0)

    def test_manifest_records_the_written_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {"biomes": [point_biome("minecraft:plains")]},
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            result = run_tool(datagen, root / "packed")
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(
                (root / "packed" / "MANIFEST.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["kind"], "biome_parameters")
            for name in ("overworld.json", "nether.json"):
                data = (root / "packed" / name).read_bytes()
                entry = manifest["files"][name]
                self.assertEqual(entry["bytes"], len(data))
                self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
            self.assertTrue((root / "packed" / "NOTICE.md").is_file())

    def test_missing_dimension_report_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = root / "generated"
            datagen.mkdir()
            (datagen / "overworld.json").write_text(
                json.dumps({"biomes": [point_biome("minecraft:plains")]}),
                encoding="utf-8",
            )
            result = run_tool(datagen, root / "packed")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("nether.json", result.stderr)

    def test_a_malformed_interval_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {
                    "biomes": [
                        point_biome("minecraft:plains", temperature=[0.0, 0.5, 1.0])
                    ]
                },
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            result = run_tool(datagen, root / "packed")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("interval", result.stderr)

    def test_check_accepts_a_packed_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {"biomes": [point_biome("minecraft:plains")]},
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            packed = root / "packed"
            self.assertEqual(run_tool(datagen, packed).returncode, 0)
            result = run_check(packed)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_check_detects_packed_report_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            datagen = write_datagen(
                root / "generated",
                {"biomes": [point_biome("minecraft:plains")]},
                {"biomes": [point_biome("minecraft:nether_wastes")]},
            )
            packed = root / "packed"
            self.assertEqual(run_tool(datagen, packed).returncode, 0)
            overworld = packed / "overworld.json"
            overworld.write_text(
                overworld.read_text(encoding="utf-8").replace(
                    '"offset": 0', '"offset": 1750'
                ),
                encoding="utf-8",
            )
            result = run_check(packed)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("does not match", result.stderr)


if __name__ == "__main__":
    unittest.main()

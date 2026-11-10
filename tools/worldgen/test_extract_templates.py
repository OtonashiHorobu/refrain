#!/usr/bin/env python3
"""Negative tests for the structure-template extractor.

The extractor writes files from an untrusted jar, so every entry name that can
leave the output directory must be rejected before anything is written.
"""

from __future__ import annotations

import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

TOOL = pathlib.Path(__file__).with_name("extract_templates.py")
VERSION = "26.3"
INNER_PATH = f"META-INF/versions/{VERSION}/server-{VERSION}.jar"


def build_jar(path: pathlib.Path, entries: dict[str, bytes]) -> None:
    inner = io.BytesIO()
    with zipfile.ZipFile(inner, "w") as archive:
        for name, payload in entries.items():
            archive.writestr(name, payload)
    with zipfile.ZipFile(path, "w") as outer:
        outer.writestr("version.json", json.dumps({"id": VERSION}))
        outer.writestr(INNER_PATH, inner.getvalue())


def run_tool(jar: pathlib.Path, out: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), "--server-jar", str(jar), "--out", str(out)],
        capture_output=True,
        text=True,
        check=False,
    )


class ExtractTemplatesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._temporary.name)
        self.jar = self.root / "server.jar"
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def test_writes_a_nested_template(self) -> None:
        build_jar(
            self.jar,
            {"data/minecraft/structure/village/plains/house1.nbt": b"nbt"},
        )
        result = run_tool(self.jar, self.out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.out / "minecraft/structure/village/plains/house1.nbt").read_bytes(),
            b"nbt",
        )
        manifest = json.loads((self.out / "TEMPLATES.json").read_text())
        self.assertEqual(manifest["entry_count"], 1)
        self.assertEqual(manifest["game_version"], VERSION)

    def test_rejects_a_parent_traversal(self) -> None:
        build_jar(self.jar, {"data/../structure/escaped.nbt": b"nbt"})
        result = run_tool(self.jar, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe template path", result.stderr)
        self.assertFalse((self.root / "structure" / "escaped.nbt").exists())

    def test_rejects_an_absolute_path(self) -> None:
        build_jar(self.jar, {"data//tmp/structure/escaped.nbt": b"nbt"})
        result = run_tool(self.jar, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe template path", result.stderr)
        self.assertFalse(pathlib.Path("/tmp/structure/escaped.nbt").exists())

    def test_rejects_a_windows_separator(self) -> None:
        build_jar(self.jar, {"data/minecraft/structure/x\\..\\escaped.nbt": b"nbt"})
        result = run_tool(self.jar, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe template path", result.stderr)


if __name__ == "__main__":
    unittest.main()

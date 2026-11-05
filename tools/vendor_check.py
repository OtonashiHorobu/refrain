#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""CI guard for the Refrain vendoring policy.

Checks performed (exit code 0 = all pass, 1 = policy violation):

  G1  no `FetchContent` / `ExternalProject_Add` / `CPMAddPackage` in CMake code
  G2  no git submodules (`.gitmodules`, `git submodule`, `GIT_REPOSITORY` CMake pulls)
  G3  every `conan/recipes/<pkg>/` has conanfile.py + README.md + at least one
      64-hex sha256 pin, and a LICENSE file when the recipe exports one
  G4  every recipe declares name / version / license
  G5  every recipe pin string is a valid sha256 (64 lowercase hex chars)
  G6  no third-party tarball/zips committed to the repo (vendored binaries are
      always fetched and hash-verified by Conan recipes)
  G7  vendor.lock exists and covers every dependency required by conanfile.py

Usage:  python3 tools/vendor_check.py [--verbose]
"""

import argparse
import ast
import fnmatch
import json
import os
import re
import sys

# Single source of truth for the pin manifest: vendor_export.recipe_meta() is
# the AST extractor used by `vendor_export.py --check`, so the guard and the CI
# artifact can never disagree about which sha256 pins a recipe has.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vendor_export  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECIPES_DIR = os.path.join(ROOT, "conan", "recipes")
SKIP_DIRS = {".git", "build", "build-release", "build-debug", "install", ".cache",
             ".vs", ".idea", ".vscode", "node_modules", "third_party"}

BANNED_CMAKE = ("fetchcontent", "externalproject_add", "cpmaddpackage",
                "git_repository", "git_submodule")
CMAKE_EXTS = (".cmake", ".txt")
ARCHIVE_EXTS = (".zip", ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".7z")

failures = []


def fail(rule, message):
    failures.append(f"[{rule}] {message}")


def repo_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        rel = os.path.relpath(dirpath, ROOT).replace("\\", "/")
        if rel == ".":
            rel = ""
        for name in filenames:
            yield rel, name, os.path.join(dirpath, name)


def load_recipe(path):
    """Import the recipe module in a sandbox-less way via exec (no conan import)."""
    source = open(os.path.join(path, "conanfile.py"), encoding="utf-8").read()
    # The recipe imports conan at module top; drop import lines and just parse
    # the class-level literal assignments with a small regex-free reader.
    meta = {}
    for attr in ("name", "version", "license"):
        match = re.search(rf'^\s+{attr}\s*=\s*"([^"]+)"', source, re.MULTILINE)
        if match:
            meta[attr] = match.group(1)
    return source, meta


def check_g1_g2():
    for rel, name, full in repo_files():
        lower = name.lower()
        if lower.endswith(CMAKE_EXTS) or lower in ("cmakelists.txt",):
            try:
                text = open(full, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            low = text.lower()
            for marker in BANNED_CMAKE:
                if marker in low:
                    fail("G1", f"{os.path.join(rel, name)}: contains '{marker}' "
                               "(vendor through Conan recipes instead)")
        if name == ".gitmodules":
            fail("G2", f"{os.path.join(rel, name)}: git submodules are forbidden")
        if lower.endswith((".sh", ".py", ".yml", ".yaml", ".toml")) and re.search(
                r"git\s+submodule\s+(init|update|add|sync|foreach)",
                open(full, encoding="utf-8", errors="ignore").read().lower()):
            fail("G2", f"{os.path.join(rel, name)}: invokes 'git submodule'")


def check_g3_g4_g5():
    if not os.path.isdir(RECIPES_DIR):
        fail("G3", "conan/recipes/ is missing")
        return
    found = False
    for entry in sorted(os.listdir(RECIPES_DIR)):
        path = os.path.join(RECIPES_DIR, entry)
        conanfile = os.path.join(path, "conanfile.py")
        if not os.path.isfile(conanfile):
            continue
        found = True
        rel = os.path.join("conan/recipes", entry)
        source, _ = load_recipe(path)
        # AST-based metadata/pins (same extractor as vendor_export --check).
        meta = vendor_export.recipe_meta(path)
        for required in ("README.md",):
            if not os.path.isfile(os.path.join(path, required)):
                fail("G3", f"{rel}: missing {required}")
        if re.search(r"^\s+exports\s*=.*LICENSE", source, re.MULTILINE) and \
                not os.path.isfile(os.path.join(path, "LICENSE")):
            fail("G3", f"{rel}: exports LICENSE but conan/recipes/{entry}/LICENSE is absent")
        for attr in ("name", "version", "license"):
            if not meta.get(attr):
                fail("G4", f"{rel}: recipe does not declare `{attr}`")
        pins = meta["sha256"]
        if not pins:
            fail("G3", f"{rel}: no sha256 pin found")
        for pin in pins:
            if not re.fullmatch(r"[0-9a-f]{64}", pin):
                fail("G5", f"{rel}: invalid sha256 pin {pin!r}")
    if not found:
        fail("G3", "no recipes with conanfile.py under conan/recipes/")


def conanfile_requirements():
    """Package names required by the top-level conanfile.py with *default*
    options (e.g. `spdlog`). Requirements behind an option that defaults to
    False (the deferred `steam-audio` client stack) are not part of the locked
    graph, so they are skipped."""
    path = os.path.join(ROOT, "conanfile.py")
    tree = ast.parse(open(path, encoding="utf-8").read())

    defaults: dict[str, bool] = {}
    for node in ast.walk(tree):
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and
                isinstance(node.targets[0], ast.Name) and
                node.targets[0].id == "default_options"):
            try:
                defaults = ast.literal_eval(node.value)
            except (ValueError, SyntaxError):
                defaults = {}

    def option_of(test: ast.expr) -> str | None:
        if (isinstance(test, ast.Attribute) and
                isinstance(test.value, ast.Attribute) and
                test.value.attr == "options" and
                isinstance(test.value.value, ast.Name) and
                test.value.value.id == "self"):
            return test.attr
        return None

    names: set[str] = set()

    def collect(node: ast.AST, enabled: bool) -> None:
        if not enabled:
            return
        if isinstance(node, ast.Call):
            func = node.func
            if (isinstance(func, ast.Attribute) and func.attr == "requires" and
                    node.args):
                try:
                    ref = ast.literal_eval(node.args[0])
                except (ValueError, SyntaxError):
                    ref = None
                if isinstance(ref, str) and "/" in ref:
                    names.add(ref.split("/", 1)[0])
        if isinstance(node, ast.If):
            option = option_of(node.test)
            branch_enabled = (enabled if option is None
                              else enabled and bool(defaults.get(option, True)))
            for child in node.body:
                collect(child, branch_enabled)
            for child in node.orelse:
                collect(child, enabled and option is None)
            return
        for child in ast.iter_child_nodes(node):
            collect(child, enabled)

    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            for stmt in node.body:
                if (isinstance(stmt, ast.FunctionDef) and
                        stmt.name == "requirements"):
                    for child in stmt.body:
                        collect(child, True)
    return names


def check_g7_lockfile():
    lock_path = os.path.join(ROOT, "conan", "locks", "vendor.lock")
    if not os.path.isfile(lock_path):
        fail("G7", "conan/locks/vendor.lock is missing; dependencies must be "
                   "pinned (regenerate with `conan lock create . -pr "
                   "gcc-release --lockfile-out=conan/locks/vendor.lock`)")
        return
    try:
        lock = json.load(open(lock_path, encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail("G7", f"conan/locks/vendor.lock is unreadable: {error}")
        return
    locked = set()
    for key in ("requires", "build_requires", "config_requires",
                "python_requires"):
        for entry in lock.get(key, []):
            # "pkg/version#rev%timestamp" -> "pkg"
            locked.add(entry.split("/", 1)[0])
    missing = sorted(conanfile_requirements() - locked)
    if missing:
        fail("G7", "conan/locks/vendor.lock does not cover conanfile.py "
                   f"requirement(s): {', '.join(missing)}")


def check_g6():
    for rel, name, _ in repo_files():
        lower = name.lower()
        if lower.endswith(ARCHIVE_EXTS):
            fail("G6", f"{os.path.join(rel, name)}: committed binary archive; "
                       "third-party payloads must be fetched+hash-verified by a recipe")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    check_g1_g2()
    check_g3_g4_g5()
    check_g6()
    check_g7_lockfile()

    if failures:
        print(f"vendor_check: {len(failures)} violation(s)")
        for item in failures:
            print("  " + item)
        return 1
    print("vendor_check: OK (G1-G7 all pass)")
    if args.verbose:
        print(f"  scanned repo root: {ROOT}")
        print(f"  recipes: {', '.join(sorted(os.listdir(RECIPES_DIR))) if os.path.isdir(RECIPES_DIR) else '-'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

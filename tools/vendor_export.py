#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Export (and optionally build) the in-repo Conan recipes.

Conan 2 cannot resolve `conan/recipes/<pkg>` directly from a consumer
conanfile.py, so the recipes must live in the local Conan cache first.  This
script is the single entry point used by CI and by developers:

    python3 tools/vendor_export.py            # export all recipes to cache
    python3 tools/vendor_export.py --build    # export + `conan create` (package)
    python3 tools/vendor_export.py --check    # only print the pin manifest
    python3 tools/vendor_export.py --offline  # never touch remotes (-nr)

The pin manifest printed by `--check` is the audit list used by
`tools/vendor_check.py`.
"""

import argparse
import ast
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECIPES_DIR = os.path.join(ROOT, "conan", "recipes")


def recipe_dirs():
    if not os.path.isdir(RECIPES_DIR):
        return []
    out = []
    for name in sorted(os.listdir(RECIPES_DIR)):
        path = os.path.join(RECIPES_DIR, name)
        if os.path.isfile(os.path.join(path, "conanfile.py")):
            out.append(path)
    return out


def recipe_meta(path):
    """Best-effort AST extraction of the package identity/ pins."""
    source = open(os.path.join(path, "conanfile.py"), encoding="utf-8").read()
    tree = ast.parse(source)
    meta = {"dir": os.path.basename(path), "name": None, "version": None,
            "license": None, "sha256": [], "repo": None}
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        for stmt in node.body:
            if not isinstance(stmt, ast.Assign) or len(stmt.targets) != 1:
                continue
            target = stmt.targets[0]
            if not isinstance(target, ast.Name):
                continue
            key = target.id
            try:
                value = ast.literal_eval(stmt.value)
            except (ValueError, SyntaxError):
                continue
            if key in ("name", "version", "license", "url", "homepage"):
                meta[key if key != "url" else "repo"] = value
            elif key in ("_sha256", "_crashpad_arc_sha"):
                if isinstance(value, str) and len(value) == 64:
                    meta["sha256"].append(value)
            elif key in ("_artifacts", "_submodules"):
                stack = [value]
                while stack:
                    item = stack.pop()
                    if isinstance(item, str):
                        if len(item) == 64 and all(c in "0123456789abcdef" for c in item):
                            meta["sha256"].append(item)
                    elif isinstance(item, dict):
                        stack.extend(item.values())
                    elif isinstance(item, (list, tuple)):
                        stack.extend(item)
    meta["sha256"] = sorted(set(meta["sha256"]))
    return meta


def run(cmd, cwd=ROOT):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--profile", "-pr", default="gcc-release",
                    help="Conan host profile (default: gcc-release)")
    ap.add_argument("--only", action="append", default=None,
                    help="only handle this recipe directory name (repeatable)")
    ap.add_argument("--build", action="store_true",
                    help="run `conan create` for each recipe (builds from source/import)")
    ap.add_argument("--check", action="store_true",
                    help="only print the pin manifest, do not export")
    ap.add_argument("--offline", action="store_true",
                    help="pass -nr/--no-remote to Conan (cache-only, air-gapped CI)")
    args = ap.parse_args()

    recipes = recipe_dirs()
    if not recipes:
        print(f"ERROR: no recipes found under {RECIPES_DIR}", file=sys.stderr)
        return 2

    metas = [recipe_meta(p) for p in recipes]
    if args.only:
        wanted = set(args.only)
        selected = [(p, m) for p, m in zip(recipes, metas) if m["dir"] in wanted]
        missing = wanted - {m["dir"] for _, m in selected}
        if missing:
            print(f"ERROR: unknown recipe(s): {', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    else:
        selected = list(zip(recipes, metas))

    print(f"{'recipe dir':<16} {'package ref':<28} {'license':<26} sha256 pins")
    print("-" * 100)
    for path, meta in selected:
        ref = f"{meta['name']}/{meta['version']}"
        print(f"{meta['dir']:<16} {ref:<28} {str(meta['license']):<26} {len(meta['sha256'])}")
        for digest in meta["sha256"]:
            print(f"{'':<16} - {digest}")
    if args.check:
        return 0

    extra = ["-nr"] if args.offline else []
    failed = []
    for path, meta in selected:
        if args.build:
            cmd = ["conan", "create", path, "-pr", args.profile,
                   "-pr:b", args.profile, "--build=missing", "--update"] + extra
        else:
            cmd = ["conan", "export", path] + extra
        if run(cmd) != 0:
            failed.append(meta["name"])
            if args.build:
                print(f"ERROR: conan create failed for {meta['name']}", file=sys.stderr)
                break

    print()
    print("Consumer requirements snippet for the top-level conanfile.py:")
    for _, meta in selected:
        print(f'    self.requires("{meta["name"]}/{meta["version"]}")')
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

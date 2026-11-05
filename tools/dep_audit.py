#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Dependency freshness audit.

Compares every pinned dependency in `conanfile.py` against:

  * ConanCenter (CCI) latest version      (`conan list <pkg>/* -r conancenter`)
  * upstream GitHub latest release tag    (GitHub REST API)

and prints a "behind upstream" table. In-repo vendored recipes
(`conan/recipes/<pkg>/conanfile.py`) are recognised as the source of truth for
their package: their version is reported with source "in-repo".

The audit is informational and never fails CI by default; `--fail-on-outdated`
turns "behind upstream" into exit code 1 for local/manual use. Network results
can be cached to a JSON file so the audit also works offline:

    python3 tools/dep_audit.py                    # online audit
    python3 tools/dep_audit.py --update-cache     # online + write cache
    python3 tools/dep_audit.py --offline          # use cache only
    python3 tools/dep_audit.py --json out.json    # machine-readable report

CI runs it from the optional `workflow_dispatch` audit job (continue-on-error).
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CACHE = os.path.join(ROOT, "tools", "dep_audit_cache.json")

# Upstream GitHub repositories (owner/name) for each direct dependency.
UPSTREAM_REPOS = {
    "boost-ext-ut": "boost-ext/ut",
    "benchmark": "google/benchmark",
    "cli11": "CLIUtils/CLI11",
    "crashpad": "getsentry/crashpad",
    "entt": "skypjack/entt",
    "fmt": "fmtlib/fmt",
    "glaze": "stephenberry/glaze",
    "libzip": "nih-at/libzip",
    "lz4": "lz4/lz4",
    # Content/archive and checksum dependencies.
    "libarchive": "libarchive/libarchive",
    "crc32c": "google/crc32c",
    "magic_enum": "Neargye/magic_enum",
    "mimalloc": "microsoft/mimalloc",
    "refl-cpp": "veselink1/refl-cpp",
    "simdjson": "simdjson/simdjson",
    "spdlog": "gabime/spdlog",
    "steam-audio": "ValveSoftware/steam-audio",
    "wasmtime": "bytecodealliance/wasmtime",
    "xxhash": "Cyan4973/xxHash",
    # Content container: block compression + content addressing.
    "zstd": "facebook/zstd",
    "blake3": "BLAKE3-team/BLAKE3",
    # Ed25519 detached signatures for signed `.rfmod` manifests.
    "libsodium": "jedisct1/libsodium",
    # zlib-ng compat drop-in packaged as zlib/1.3.1.
    "zlib": "zlib-ng/zlib-ng",
}

# Packages whose *implementation* is newer than the compatibility version they
# advertise: (upstream repo, pinned implementation version). zlib/1.3.1 is the
# zlib-ng 2.3.3 `ZLIB_COMPAT` drop-in, so freshness is judged on zlib-ng.
IMPLEMENTATION_OVERRIDES = {
    "zlib": ("zlib-ng/zlib-ng", "2.3.3"),
}

# Packages pinned by commit instead of a release tag.
COMMIT_PINNED = {"crashpad"}


def version_key(text: str) -> tuple:
    """Order versions like 3.6.0 / v5.1.0 / 1.17.0 correctly."""
    parts = re.split(r"[.\-_+]", text.strip().lstrip("vV"))
    key: list[tuple[int, object]] = []
    for part in parts:
        if part.isdigit():
            key.append((1, int(part)))
        else:
            match = re.match(r"(\d+)([A-Za-z].*)?", part)
            if match:
                key.append((1, int(match.group(1))))
                key.append((2, match.group(2) or ""))
            elif part:
                key.append((2, part))
    return tuple(key)


def conanfile_default_options(tree: ast.AST) -> dict[str, object]:
    """`default_options` of the top-level recipe (empty when not a literal)."""
    for node in ast.walk(tree):
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and
                isinstance(node.targets[0], ast.Name) and
                node.targets[0].id == "default_options"):
            try:
                return ast.literal_eval(node.value)
            except (ValueError, SyntaxError):
                return {}
    return {}


def option_of(test: ast.expr) -> str | None:
    """`self.options.<name>` behind an `if`, or None for other conditions."""
    if (isinstance(test, ast.Attribute) and
            isinstance(test.value, ast.Attribute) and
            test.value.attr == "options" and
            isinstance(test.value.value, ast.Name) and
            test.value.value.id == "self"):
        return test.attr
    return None


def pinned_dependencies() -> dict[str, str]:
    """Top-level dependencies from conanfile.py with *default* options.

    Mirrors `tools/vendor_check.py::conanfile_requirements`: requirements
    behind an option that defaults to False (e.g. the deferred `steam-audio`
    client stack under `options.client`) are not part of the default graph and
    must not be audited as if they were. Conditions that do not test a recipe
    option (e.g. `options.with_io_uring and settings.os == "Linux"`) keep both
    branches, matching vendor_check.
    """
    tree = ast.parse(open(os.path.join(ROOT, "conanfile.py"),
                          encoding="utf-8").read())
    defaults = conanfile_default_options(tree)
    pins: dict[str, str] = {}

    def collect(node: ast.AST, enabled: bool) -> None:
        if not enabled:
            return
        if isinstance(node, ast.Call):
            func = node.func
            if (isinstance(func, ast.Attribute) and
                    func.attr == "requires" and node.args):
                try:
                    ref = ast.literal_eval(node.args[0])
                except (ValueError, SyntaxError):
                    ref = None
                if isinstance(ref, str) and "/" in ref:
                    name, version = ref.split("/", 1)
                    pins.setdefault(name, version)
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
    return pins


def vendored_versions() -> dict[str, str]:
    """Package versions provided by in-repo recipes (conan/recipes/*)."""
    vendored: dict[str, str] = {}
    recipes_root = os.path.join(ROOT, "conan", "recipes")
    if not os.path.isdir(recipes_root):
        return vendored
    for entry in sorted(os.listdir(recipes_root)):
        path = os.path.join(recipes_root, entry, "conanfile.py")
        if not os.path.isfile(path):
            continue
        source = open(path, encoding="utf-8").read()
        name = re.search(r'^\s+name\s*=\s*"([^"]+)"', source, re.MULTILINE)
        version = re.search(r'^\s+version\s*=\s*"([^"]+)"', source,
                            re.MULTILINE)
        if name and version:
            vendored[name.group(1)] = version.group(1)
    return vendored


def recipe_commit_pin(package: str) -> str | None:
    """SHA pin of a commit-pinned in-repo recipe (`_<name>_sha = "..."`)."""
    path = os.path.join(ROOT, "conan", "recipes", package, "conanfile.py")
    try:
        source = open(path, encoding="utf-8").read()
    except OSError:
        return None
    match = re.search(r'^\s+_\w*_?sha\w*\s*=\s*"([0-9a-f]{40})"', source,
                      re.MULTILINE)
    if match is None:
        match = re.search(r'"([0-9a-f]{40})"', source)
    return match.group(1) if match else None


def cci_latest(package: str) -> str | None:
    try:
        result = subprocess.run(
            ["conan", "list", f"{package}/*", "-r", "conancenter",
             "--format=json"],
            capture_output=True, text=True, timeout=120, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None
    versions: list[str] = []
    for remote in data.values():
        for ref in remote:
            versions.append(ref.rsplit("/", 1)[-1])
    if not versions:
        return None
    # ConanCenter also publishes `cci.YYYYMMDD` snapshots for some projects.
    # They are not release numbers and must not outrank real versions when the
    # package has a semver release list (libsodium: cci.20220430 vs 1.0.22).
    releases = [version for version in versions
                if not version.startswith("cci.")]
    return max(releases or versions, key=version_key)


def github_json(url: str):
    """GET a GitHub API URL, returning None on network/HTTP/JSON failure."""
    request = urllib.request.Request(
        url, headers={"Accept": "application/vnd.github+json",
                      "User-Agent": "refrain-dep-audit"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
        return None


def github_latest(package: str) -> str | None:
    repo = UPSTREAM_REPOS.get(package)
    if repo is None:
        return None
    url = f"https://api.github.com/repos/{repo}/releases/latest"
    if package in COMMIT_PINNED:
        url = f"https://api.github.com/repos/{repo}/commits?per_page=1"
    payload = github_json(url)
    if package in COMMIT_PINNED:
        if isinstance(payload, list) and payload:
            return payload[0].get("sha", "")[:7] or None
        return None
    tag = payload.get("tag_name") if isinstance(payload, dict) else None
    if not tag:
        # Projects such as google/crc32c publish git tags but no GitHub
        # "Releases"; `releases/latest` is then 404. Fall back to the newest
        # tag so freshness is still judged against upstream. Ask for a page of
        # tags and order by version: the API's first entry is not guaranteed
        # to be the latest semver (annotated/date ordering varies).
        tags = github_json(
            f"https://api.github.com/repos/{repo}/tags?per_page=100")
        if isinstance(tags, list):
            names = [entry.get("name") for entry in tags
                     if isinstance(entry, dict) and entry.get("name")]
            if names:
                tag = max(names, key=version_key)
    if not tag:
        return None
    # Upstream release tags occasionally carry a channel suffix that is not
    # part of the version (libsodium publishes `1.0.22-RELEASE`); compare
    # versions, not tag spelling.
    for suffix in ("-RELEASE", "-FINAL", "-STABLE"):
        tag = tag.removesuffix(suffix)
    return tag.lstrip("vV")


def load_cache(path: str) -> dict:
    try:
        return json.load(open(path, encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def audit(offline: bool, update_cache: bool, cache_path: str) -> dict[str, dict]:
    pins = pinned_dependencies()
    vendored = vendored_versions()
    cache = load_cache(cache_path)
    report: dict[str, dict] = {}
    for package, pinned in sorted(pins.items()):
        entry = cache.get(package, {})
        if not offline:
            cci = cci_latest(package) or entry.get("cci")
            upstream = github_latest(package) or entry.get("upstream")
            entry = {"cci": cci, "upstream": upstream}
        entry["pinned"] = pinned
        entry["source"] = "in-repo" if package in vendored else "conancenter"
        if package in vendored:
            entry["pinned"] = vendored[package]
        pinned_version = entry["pinned"]
        if package in IMPLEMENTATION_OVERRIDES:
            _, implementation = IMPLEMENTATION_OVERRIDES[package]
            entry["pinned_implementation"] = implementation
            entry["pinned_label"] = (
                f"{entry['pinned']} (impl {implementation})")
            pinned_version = implementation
        else:
            entry["pinned_label"] = str(entry["pinned"])
        if package in COMMIT_PINNED:
            # Commit-pinned recipes carry the pinned SHA inside the recipe;
            # compare the (short) commit instead of a semver ordering.
            pinned_commit = recipe_commit_pin(package)
            upstream_commit = entry.get("upstream")
            entry["outdated_vs_upstream"] = bool(
                pinned_commit and upstream_commit and
                not upstream_commit.startswith(pinned_commit[:7]))
        else:
            entry["outdated_vs_upstream"] = bool(
                entry.get("upstream") and
                version_key(entry["upstream"]) >
                version_key(pinned_version))
        report[package] = entry
        cache[package] = {k: entry.get(k) for k in ("cci", "upstream")}
    if update_cache:
        json.dump(cache, open(cache_path, "w", encoding="utf-8"),
                  indent=2, sort_keys=True)
        print(f"cache written: {cache_path}")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--offline", action="store_true",
                        help="use the JSON cache, do not query CCI/GitHub")
    parser.add_argument("--update-cache", action="store_true",
                        help="write the fetched versions to the cache file")
    parser.add_argument("--cache", default=DEFAULT_CACHE,
                        help=f"cache file (default: {DEFAULT_CACHE})")
    parser.add_argument("--json", default=None,
                        help="also write the report as JSON to this path")
    parser.add_argument("--fail-on-outdated", action="store_true",
                        help="exit 1 when any dependency is behind upstream")
    args = parser.parse_args()

    report = audit(args.offline, args.update_cache, args.cache)
    print(f"{'package':<14} {'pinned':<24} {'source':<12} "
          f"{'cci':<12} {'upstream':<12} status")
    print("-" * 90)
    outdated = 0
    unknown = 0
    for package, entry in report.items():
        status = "ok"
        if entry.get("outdated_vs_upstream"):
            status = "BEHIND upstream"
            outdated += 1
        elif entry.get("upstream") is None:
            status = "unknown (no network/cache)"
            unknown += 1
        print(f"{package:<14} {str(entry.get('pinned_label')):<24} "
              f"{entry['source']:<12} {str(entry.get('cci')):<12} "
              f"{str(entry.get('upstream')):<12} {status}")
    if outdated:
        print(f"\n{outdated} package(s) are behind upstream; see the freshness "
              "policy (vendor the latest upstream release in conan/recipes/).")
    if unknown:
        print(f"\n{unknown} package(s) could not be checked (offline/cache "
              "miss); run without --offline and with --update-cache to "
              "refresh.")
    if not outdated and not unknown:
        print("\nall audited packages are at the latest upstream release")
    if args.json:
        json.dump(report, open(args.json, "w", encoding="utf-8"), indent=2,
                  sort_keys=True)
        print(f"report written: {args.json}")
    return 1 if (outdated and args.fail_on_outdated) else 0


if __name__ == "__main__":
    sys.exit(main())

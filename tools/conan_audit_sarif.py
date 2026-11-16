#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Convert a `conan audit scan --format json` report into SARIF 2.1.0.

The result is uploaded to GitHub code scanning, which turns each vulnerability
into an alert under Security -> Code scanning. ConanCenter is not a GitHub
Advisory Database ecosystem, so Dependabot cannot produce these alerts.

Usage:
    conan audit scan . --lockfile=conan/locks/vendor.lock \
        --format json --out-file conan-audit.json --severity-level 100
    python3 tools/conan_audit_sarif.py --input conan-audit.json \
        --output conan-audit.sarif --lockfile conan/locks/vendor.lock

`--severity-level 100` keeps the audit command from failing on a finding; the
SARIF alert carries the severity instead.
"""

from __future__ import annotations

import argparse
import json
import sys

SEVERITY_LEVEL = {
    "Critical": "error",
    "High": "error",
    "Medium": "warning",
    "Low": "note",
}


def _node_text(value: object) -> str:
    return value if isinstance(value, str) else ""


def _fixed_versions(node: dict) -> list[str]:
    versions: list[str] = []
    packages = node.get("vulnerablePackages")
    if not isinstance(packages, dict):
        return versions
    for edge in packages.get("edges", []):
        if not isinstance(edge, dict):
            continue
        fixed_node = edge.get("node")
        if not isinstance(fixed_node, dict):
            continue
        for fix in fixed_node.get("fixVersions", []):
            if isinstance(fix, dict):
                version = _node_text(fix.get("version"))
                if version:
                    versions.append(version)
    return sorted(set(versions))


def _help_uri(node: dict) -> str:
    references = node.get("references")
    if isinstance(references, list):
        for reference in references:
            if isinstance(reference, str) and reference.startswith("http"):
                return reference
    return ""


def convert(report: dict, lockfile: str) -> dict:
    rules: list[dict] = []
    rule_index: dict[str, int] = {}
    results: list[dict] = []

    def rule_for(node: dict) -> int:
        name = _node_text(node.get("name")) or "unknown-vulnerability"
        if name in rule_index:
            return rule_index[name]
        severity = _node_text(node.get("severity")) or "Unknown"
        description = _node_text(node.get("description")) or name
        rule: dict = {
            "id": name,
            "name": name.replace("/", "-"),
            "shortDescription": {"text": f"{name} ({severity})"},
            "fullDescription": {"text": description},
            "properties": {
                "tags": ["security", "dependency"],
                "severity": severity,
            },
        }
        uri = _help_uri(node)
        if uri:
            rule["helpUri"] = uri
        rule_index[name] = len(rules)
        rules.append(rule)
        return rule_index[name]

    data = report.get("data")
    if not isinstance(data, dict):
        raise SystemExit("conan audit report has no 'data' object")

    for ref, package in data.items():
        if not isinstance(package, dict):
            continue
        vulnerabilities = package.get("vulnerabilities")
        if not isinstance(vulnerabilities, dict):
            continue
        for edge in vulnerabilities.get("edges", []):
            if not isinstance(edge, dict):
                continue
            node = edge.get("node")
            if not isinstance(node, dict):
                continue
            name = _node_text(node.get("name")) or "unknown-vulnerability"
            severity = _node_text(node.get("severity")) or "Unknown"
            cvss = node.get("cvss")
            score = ""
            if isinstance(cvss, dict):
                preferred = cvss.get("preferredBaseScore")
                if preferred is not None:
                    score = f" (CVSS {preferred})"
            fixed = _fixed_versions(node)
            withdrawn = bool(node.get("withdrawn"))
            message = f"{name}{score} in {ref}"
            if fixed:
                message += f"; fixed in {', '.join(fixed)}"
            if withdrawn:
                message += "; advisory withdrawn"
            results.append(
                {
                    "ruleId": name,
                    "ruleIndex": rule_for(node),
                    "level": "note" if withdrawn else SEVERITY_LEVEL.get(
                        severity, "warning"),
                    "message": {"text": message},
                    "locations": [
                        {
                            "physicalLocation": {
                                "artifactLocation": {"uri": lockfile},
                                "region": {"startLine": 1, "startColumn": 1},
                            }
                        }
                    ],
                    "properties": {
                        "package": ref,
                        "severity": severity,
                        "cvss": cvss if isinstance(cvss, dict) else {},
                        "fixedVersions": fixed,
                    },
                }
            )

    return {
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "version": "2.1.0",
        "runs": [
            {
                "tool": {
                    "driver": {
                        "name": "conan-audit",
                        "informationUri": "https://conan.io/audit",
                        "rules": rules,
                    }
                },
                "columnKind": "utf16CodeUnits",
                "results": results,
                "properties": {
                    "conanError": _node_text(report.get("conan_error")),
                    "provider": _node_text(report.get("provider")),
                },
            }
        ],
    }


def self_test() -> int:
    report = {
        "provider": "conancenter",
        "data": {
            "fmt/12.0.0": {
                "vulnerabilities": {
                    "edges": [
                        {
                            "node": {
                                "name": "CVE-2025-0001",
                                "severity": "Critical",
                                "description": "example critical issue",
                                "references": ["https://example.invalid/CVE-2025-0001"],
                                "cvss": {"preferredBaseScore": 9.8},
                                "vulnerablePackages": {
                                    "edges": [
                                        {"node": {"fixVersions": [{"version": "12.2.0"}]}}
                                    ]
                                },
                            }
                        },
                        {
                            "node": {
                                "name": "CVE-2025-0002",
                                "severity": "Low",
                                "cvss": {"preferredBaseScore": 2.0},
                            }
                        },
                    ]
                }
            }
        },
    }
    sarif = convert(report, "conan/locks/vendor.lock")
    run = sarif["runs"][0]
    assert len(run["tool"]["driver"]["rules"]) == 2
    assert len(run["results"]) == 2
    assert run["results"][0]["level"] == "error"
    assert run["results"][1]["level"] == "note"
    assert run["results"][0]["properties"]["fixedVersions"] == ["12.2.0"]
    empty = convert({"data": {}}, "conan/locks/vendor.lock")
    assert empty["runs"][0]["results"] == []
    print("conan-audit SARIF converter self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="conan audit json report")
    parser.add_argument("--output", help="SARIF output path")
    parser.add_argument("--lockfile", default="conan/locks/vendor.lock",
                        help="path recorded in SARIF locations")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.input or not args.output:
        parser.error("--input and --output are required")
    with open(args.input, encoding="utf-8") as report_file:
        report = json.load(report_file)
    sarif = convert(report, args.lockfile)
    with open(args.output, "w", encoding="utf-8") as output_file:
        json.dump(sarif, output_file, indent=2)
        output_file.write("\n")
    results = sarif["runs"][0]["results"]
    print(f"conan-audit SARIF: {len(results)} result(s) -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

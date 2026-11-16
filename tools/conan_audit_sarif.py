#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Convert a `conan audit scan --format json` report into SARIF 2.1.0.

The result is uploaded to GitHub code scanning, which turns each vulnerability
into an alert under Security -> Code scanning. ConanCenter is not a GitHub
Advisory Database ecosystem, so Dependabot cannot produce these alerts.

A triage file can mark findings that were reviewed against the vendored
implementation: `not-applicable` and `accepted-risk` findings are left out of
the alert stream and listed in the summary, while `pending-fix` findings stay
visible with their note.

Usage:
    conan audit scan . --lockfile=conan/locks/vendor.lock \
        --format json --out-file conan-audit.json --severity-level 100
    python3 tools/conan_audit_sarif.py --input conan-audit.json \
        --output conan-audit.sarif --summary conan-audit-summary.md \
        --triage .github/conan-audit-triage.json \
        --lockfile conan/locks/vendor.lock
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

# Numeric scores GitHub uses for the alert badge when the audit has no CVSS.
SEVERITY_SCORE = {
    "Critical": 9.5,
    "High": 8.0,
    "Medium": 5.0,
    "Low": 2.0,
}

SEVERITY_RANK = {"Critical": 4, "High": 3, "Medium": 2, "Low": 1}

TRIAGE_SCHEMA = "refrain.conan-audit-triage/1"
TRIAGE_STATUSES = {"not-applicable", "accepted-risk", "pending-fix"}


def _text(value: object) -> str:
    return value if isinstance(value, str) else ""


def _score(node: dict) -> float:
    cvss = node.get("cvss")
    if isinstance(cvss, dict):
        preferred = cvss.get("preferredBaseScore")
        if isinstance(preferred, (int, float)):
            return float(preferred)
    return SEVERITY_SCORE.get(_text(node.get("severity")), 5.0)


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
                version = _text(fix.get("version"))
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


def load_triage(path: str | None) -> dict[str, dict]:
    if not path:
        return {}
    with open(path, encoding="utf-8") as triage_file:
        document = json.load(triage_file)
    if document.get("schema") != TRIAGE_SCHEMA:
        raise SystemExit(
            f"triage file {path}: expected schema {TRIAGE_SCHEMA!r}"
        )
    entries: dict[str, dict] = {}
    for entry in document.get("entries", []):
        cve = _text(entry.get("cve"))
        status = _text(entry.get("status"))
        if not cve:
            raise SystemExit(f"triage file {path}: entry without a cve")
        if status not in TRIAGE_STATUSES:
            raise SystemExit(
                f"triage file {path}: {cve} has unknown status {status!r}"
            )
        entries[cve] = entry
    return entries


def collect(report: dict) -> list[dict]:
    findings: list[dict] = []
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
            findings.append(
                {
                    "package": ref,
                    "node": node,
                    "cve": _text(node.get("name")) or "unknown-vulnerability",
                    "severity": _text(node.get("severity")) or "Unknown",
                    "score": _score(node),
                    "withdrawn": bool(node.get("withdrawn")),
                    "fixed": _fixed_versions(node),
                    "help_uri": _help_uri(node),
                    "description": _text(node.get("description")),
                }
            )
    findings.sort(
        key=lambda finding: (
            -SEVERITY_RANK.get(finding["severity"], 0),
            finding["package"],
            finding["cve"],
        )
    )
    return findings


def convert(
    report: dict,
    lockfile: str,
    triage: dict[str, dict] | None = None,
) -> tuple[dict, list[dict], list[dict]]:
    triage = triage or {}
    findings = collect(report)
    rules: list[dict] = []
    rule_index: dict[str, int] = {}
    results: list[dict] = []
    triaged: list[dict] = []

    def rule_for(finding: dict) -> int:
        name = finding["cve"]
        if name in rule_index:
            return rule_index[name]
        rule: dict = {
            "id": name,
            "name": name.replace("/", "-"),
            "shortDescription": {
                "text": f"{name} ({finding['severity']})"
            },
            "fullDescription": {
                "text": finding["description"] or name
            },
            "properties": {
                "tags": ["security", "dependency"],
                "severity": finding["severity"],
                "security-severity": f"{finding['score']:.1f}",
            },
        }
        if finding["help_uri"]:
            rule["helpUri"] = finding["help_uri"]
        if finding["fixed"]:
            rule["help"] = {
                "text": (
                    f"Fixed in: {', '.join(finding['fixed'])}"
                )
            }
        rule_index[name] = len(rules)
        rules.append(rule)
        return rule_index[name]

    for finding in findings:
        note = triage.get(finding["cve"])
        status = _text(note.get("status")) if note else ""
        if status in {"not-applicable", "accepted-risk"}:
            triaged.append({**finding, "status": status,
                            "reason": _text(note.get("reason"))})
            continue
        message = f"{finding['cve']} ({finding['severity']}"
        if finding["score"]:
            message += f", CVSS {finding['score']:.1f}"
        message += f") in {finding['package']}"
        if finding["fixed"]:
            message += f"; fixed in {', '.join(finding['fixed'])}"
        if finding["withdrawn"]:
            message += "; advisory withdrawn"
        if status == "pending-fix":
            message += (
                f"; pending fix: {_text(note.get('reason')) or 'tracked'}"
            )
        results.append(
            {
                "ruleId": finding["cve"],
                "ruleIndex": rule_for(finding),
                "level": "note" if finding["withdrawn"] else SEVERITY_LEVEL.get(
                    finding["severity"], "warning"),
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
                    "package": finding["package"],
                    "severity": finding["severity"],
                    "cvss": finding["node"].get("cvss", {}),
                    "fixedVersions": finding["fixed"],
                },
            }
        )

    sarif = {
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
                    "conanError": _text(report.get("conan_error")),
                    "provider": _text(report.get("provider")),
                    "triagedFindings": len(triaged),
                },
            }
        ],
    }
    return sarif, findings, triaged


def summarise(findings: list[dict], triaged: list[dict]) -> str:
    counts: dict[str, int] = {}
    packages: set[str] = set()
    for finding in findings:
        if any(t["cve"] == finding["cve"] for t in triaged):
            continue
        counts[finding["severity"]] = counts.get(finding["severity"], 0) + 1
        packages.add(finding["package"])
    open_findings = [
        finding for finding in findings
        if not any(
            t["cve"] == finding["cve"] for t in triaged
        )
    ]
    lines = ["### ConanCenter vulnerability audit", ""]
    lines.append(
        f"{len(open_findings)} open finding(s) in {len(packages)} package(s): "
        + (", ".join(
            f"{name} {counts[name]}" for name in
            ("Critical", "High", "Medium", "Low") if counts.get(name)
        ) or "none")
    )
    if open_findings:
        lines += [
            "",
            "| Severity | Package | Advisory | CVSS | Fixed in |",
            "|---|---|---|---|---|",
        ]
        for finding in open_findings:
            fixed = ", ".join(finding["fixed"]) or "-"
            link = f"[{finding['cve']}]({finding['help_uri']})" if \
                finding["help_uri"] else finding["cve"]
            lines.append(
                f"| {finding['severity']} | {finding['package']} | {link} | "
                f"{finding['score']:.1f} | {fixed} |"
            )
    if triaged:
        lines += ["", "### Triaged findings", ""]
        for finding in triaged:
            lines.append(
                f"- {finding['cve']} ({finding['status']}): "
                f"{finding['reason'] or 'no reason recorded'}"
            )
    return "\n".join(lines) + "\n"


def self_test() -> int:
    report = {
        "provider": "conancenter",
        "data": {
            "zlib/1.3.1": {
                "vulnerabilities": {
                    "edges": [
                        {
                            "node": {
                                "name": "CVE-TEST-HIGH",
                                "severity": "High",
                                "description": "example",
                                "references": ["https://example.invalid/advisory"],
                                "cvss": {"preferredBaseScore": 7.4},
                                "vulnerablePackages": {
                                    "edges": [
                                        {"node": {"fixVersions": [
                                            {"version": "1.3.2"}]}}
                                    ]
                                },
                            }
                        },
                        {
                            "node": {
                                "name": "CVE-TEST-LOW",
                                "severity": "Low",
                                "cvss": {"preferredBaseScore": 2.0},
                            }
                        },
                    ]
                }
            }
        },
    }
    triage = {
        "CVE-TEST-LOW": {
            "status": "not-applicable",
            "reason": "test entry",
        }
    }
    sarif, findings, triaged = convert(
        report, "conan/locks/vendor.lock", triage
    )
    run = sarif["runs"][0]
    assert len(findings) == 2
    assert len(triaged) == 1
    assert len(run["results"]) == 1
    assert run["results"][0]["level"] == "error"
    assert run["results"][0]["ruleId"] == "CVE-TEST-HIGH"
    rule = run["tool"]["driver"]["rules"][0]
    assert rule["properties"]["security-severity"] == "7.4"
    summary = summarise(findings, triaged)
    assert "CVE-TEST-HIGH" in summary
    assert "CVE-TEST-LOW (not-applicable)" in summary
    print("conan-audit SARIF converter self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="conan audit json report")
    parser.add_argument("--output", help="SARIF output path")
    parser.add_argument("--summary", help="markdown summary output path")
    parser.add_argument("--triage", help="triage file")
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
    triage = load_triage(args.triage)
    sarif, findings, triaged = convert(report, args.lockfile, triage)
    with open(args.output, "w", encoding="utf-8") as output_file:
        json.dump(sarif, output_file, indent=2)
        output_file.write("\n")
    if args.summary:
        with open(args.summary, "w", encoding="utf-8") as summary_file:
            summary_file.write(summarise(findings, triaged))
    print(
        f"conan-audit SARIF: {len(sarif['runs'][0]['results'])} alert(s), "
        f"{len(triaged)} triaged -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verify recorded generator sources/notices and fail closed on desktop input drift."""
import argparse
import json
from pathlib import Path

from verify_inventory import check_file, digest, verify

HERE = Path(__file__).resolve().parent


def signature(capture):
    # Install receipts include install timestamps. They are retained provenance,
    # whereas the installed recipe, notices, versions and actual input bytes are
    # locked. Generated build outputs are regenerated/tested, not locked bytes.
    packages = {}
    for name, record in capture["packages"].items():
        packages[name] = {k: v for k, v in record.items() if k not in ("receipt",)}
    return {"source_commit": capture["source_commit"], "profile_sha256": capture["profile_sha256"],
            "source_files": capture["source_files"],
            "integration_inputs": capture["integration_inputs"],
            "external_files": {r["path"]: {"sha256": r["sha256"], "owners": r["owners"]} for r in capture["external_files"]},
            "packages": packages, "system_runtime": capture["system_runtime"]}


def compare(expected, actual):
    observed = signature(actual)
    if actual["status"] != "captured" or actual["unowned"]:
        raise ValueError("Incomplete dependency/generator capture")
    for key in expected:
        if observed[key] != expected[key]:
            before, after = expected[key], observed[key]
            if isinstance(before, dict):
                added, removed = sorted(after.keys()-before.keys()), sorted(before.keys()-after.keys())
                changed = sorted(k for k in before.keys() & after.keys() if before[k] != after[k])
                raise ValueError(f"Desktop lock drift in {key}: added={added[:8]}, removed={removed[:8]}, changed={changed[:8]}")
            raise ValueError(f"Desktop lock drift in {key}")


def verify_sources(source, audit=None):
    audit = HERE/"audit" if audit is None else audit
    verify(source, audit)
    lock = json.loads((audit/"dependency-lock.json").read_text())
    if lock["policy_sha256"] != digest(HERE/"license-policy.json", True):
        raise ValueError("Licence policy changed without inventory refresh")
    for platform, data in lock["platforms"].items():
        for name, expected in data["signature"]["integration_inputs"].items():
            check_file(HERE.parents[1], name, expected, True)
        for name, record in data["signature"]["source_files"].items():
            check_file(source, name, record["sha256"], True)
        for name, expected in data["retained_files"].items():
            check_file(audit, name, expected)
        catalog = json.loads((audit/data["license_catalog"]).read_text())
        if set(catalog["files"]) != set(data["signature"]["source_files"]):
            raise ValueError(f"Incomplete source licence coverage: {platform}")
        for name, expected in catalog["evidence_sha256_lf"].items():
            check_file(source, name, expected, True)
        for name, origin in catalog["supplemental_notices"].items():
            check_file(HERE/"supplemental-notices", name, origin["sha256"])
    return lock


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--platform", choices=("linux", "macos"))
    args = parser.parse_args()
    lock = verify_sources(args.source.resolve())
    if args.capture:
        if not args.platform:
            parser.error("--capture requires --platform")
        compare(lock["platforms"][args.platform]["signature"], json.loads(args.capture.read_text()))
    print("Verified generator source closure, licence evidence and retained dependency notices"+
          ("; desktop dependency lock matches" if args.capture else " (source-only)"))

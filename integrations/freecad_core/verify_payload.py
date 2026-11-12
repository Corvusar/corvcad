#!/usr/bin/env python3
"""Reject measured restricted/unreviewed example assets in a production payload.

This exclusion check is necessary, not sufficient, for a future native release.
Renaming a file cannot bypass a content exclusion. No files are changed.
"""
import argparse
import hashlib
import json
from pathlib import Path


def verify(directory, catalogs):
    if not directory.is_dir():
        raise FileNotFoundError(f"Missing production payload directory: {directory}")
    excluded = {}
    for catalog in catalogs:
        for name, record in catalog["production_payload_exclusions"].items():
            excluded.setdefault(record["sha256"], []).append(name)
            if "sha256_lf" in record:
                excluded.setdefault(record["sha256_lf"], []).append(name)
    violations = []
    for path in directory.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            for digest in {hashlib.sha256(data).hexdigest(), hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()}:
                if digest in excluded:
                    violations.append({"payload": str(path), "upstream": excluded[digest]})
    if violations:
        raise ValueError(f"Excluded upstream assets in production payload: {violations[:8]}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, action="append", required=True)
    args = parser.parse_args()
    verify(args.directory, [json.loads(p.read_text()) for p in args.catalog])
    print("No measured excluded assets in payload; full release/licence checks remain separate")

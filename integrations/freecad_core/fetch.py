#!/usr/bin/env python3
"""Fetch a verified stock F1 binary, or an unmodified pinned F2 source checkout.

No install, extraction, build, patch application or app linkage is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

LOCK = json.loads(Path(__file__).with_name("upstream.json").read_text())


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def binary(platform, output):
    spec = LOCK["desktop_proof"][platform]
    target = output / spec["url"].rsplit("/", 1)[1]
    if not target.exists():
        temporary = target.with_suffix(target.suffix + ".partial")
        urllib.request.urlretrieve(spec["url"], temporary)
        if sha256(temporary) != spec["sha256"]:
            raise RuntimeError(f"Checksum mismatch: {temporary}")
        temporary.replace(target)
    if sha256(target) != spec["sha256"]:
        raise RuntimeError(f"Checksum mismatch: {target}; preserve it and use a new output directory")
    print(f"Verified {target}")


def source(output):
    spec = LOCK["source"]
    target = output / "FreeCAD"
    if not target.exists():
        subprocess.run(["git", "-c", "core.longpaths=true", "clone", "--depth=1",
                        "--branch", spec["tag"], spec["repository"], str(target)], check=True)
    actual = subprocess.check_output(["git", "-C", str(target), "rev-parse", "HEAD"], text=True).strip()
    if actual != spec["commit"]:
        raise RuntimeError(f"Unexpected source commit: {actual}")
    dirty = subprocess.check_output(["git", "-C", str(target), "status", "--porcelain"], text=True)
    if dirty:
        raise RuntimeError("Source checkout has local changes; preserve it and use a new output directory")
    print(f"Verified unmodified source {target} at {actual}; F2 build/inventory not performed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=["source", *LOCK["desktop_proof"]])
    parser.add_argument("--output", type=Path, default=Path("build/freecad-core"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.kind == "source":
        source(args.output.resolve())
    else:
        binary(args.kind, args.output.resolve())

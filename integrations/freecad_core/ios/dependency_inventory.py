#!/usr/bin/env python3
"""Record actual iOS package ABIs and archives before considering KiCad reuse.

MIT. A matching version alone does not establish interchangeable libraries.
This inventory is experimental build evidence, not a release licence review.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import build_occt
import target_config


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def paragraphs(text):
    result = []
    for block in text.split("\n\n"):
        record = {}
        for line in block.splitlines():
            if ": " in line and not line.startswith(" "):
                key, value = line.split(": ", 1)
                record[key] = value
        if record:
            result.append(record)
    return result


def capture(installed, triplet):
    prefix = installed / triplet
    if not prefix.is_dir():
        raise ValueError("Target dependency prefix is missing")
    records = paragraphs((installed / "vcpkg/status").read_text())
    packages = [r for r in records if r.get("Architecture") == triplet]
    cores = {p["Package"]: p for p in packages if "Feature" not in p}
    if not cores or any("Version" not in p for p in cores.values()):
        raise ValueError("Missing target package versions")
    if any(p["Package"] not in cores for p in packages):
        raise ValueError("Feature has no installed core package")
    # Per-feature records may not have an ABI; core records must have one.
    if any(not p.get("Abi") for p in cores.values()):
        raise ValueError("Missing target package ABI hashes")
    native = [p for p in prefix.rglob("*") if p.is_file() and
              (p.suffix in (".a", ".dylib", ".so") or any(part.endswith(".framework") for part in p.parts))]
    if any(p.suffix in (".dylib", ".so") for p in native):
        raise ValueError("Dynamic library in static iOS dependency prefix")
    if any(any(part.endswith(".framework") for part in p.parts) for p in native):
        raise ValueError("Unexpected framework in archive-only iOS dependency prefix")
    if any(any(name in p.name for name in ("Qt6Gui", "Qt6Widgets", "Qt6OpenGL", "TKOpenGl", "Coin", "Quarter")) for p in native):
        raise ValueError("GUI dependency in headless iOS prefix")
    archives = {p.relative_to(prefix).as_posix(): {"sha256": digest(p), "bytes": p.stat().st_size}
                for p in native if p.suffix == ".a"}
    if not archives:
        raise ValueError("No target static archives")
    notices = {p.relative_to(prefix).as_posix(): digest(p) for p in sorted((prefix / "share").rglob("copyright"))}
    if not notices:
        raise ValueError("Missing dependency notices")
    return {"schema": 1, "triplet": triplet, "packages": packages, "archives": archives,
            "notices": notices, "sharing_rule": "Reuse the same target package only when registry, triplet, features and ABI match. Link each common archive once; do not combine independently bundled dependency archives."}


def inspect_target(installed, triplet, report):
    """Verify archive members actually target the requested SDK and iOS minimum."""
    selected = target_config.from_triplet(triplet)
    sdk = selected.sdk
    minimum = json.loads((Path(__file__).parent / "inputs.json").read_text())["minimum_ios"]
    checked = {}
    for relative, record in report["archives"].items():
        path = installed / triplet / relative
        if digest(path) != record["sha256"]:
            raise ValueError("Dependency archive changed after capture: " + relative)
        architecture = subprocess.check_output(["xcrun", "lipo", "-archs", str(path)], text=True).strip()
        if architecture != selected.architecture:
            raise ValueError("Wrong dependency archive architecture: " + relative + ": " + repr(architecture))
        commands = subprocess.check_output(["xcrun", "otool", "-l", str(path)], text=True)
        count = build_occt.validate_platform(commands, sdk, minimum)
        checked[relative] = {"sha256": record["sha256"], "verified_build_versions": count}
    return {"schema": 1, "sdk": sdk, "architecture": selected.architecture, "minimum_ios": minimum, "archives": checked}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installed", type=Path)
    parser.add_argument("triplet", choices=target_config.TRIPLETS)
    parser.add_argument("output", type=Path)
    parser.add_argument("--verify-target", action="store_true", help="Inspect actual Mach-O archive members; requires Xcode")
    args = parser.parse_args()
    result = capture(args.installed, args.triplet)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.verify_target:
        target = inspect_target(args.installed, args.triplet, result)
        args.output.with_name(args.output.stem + "-macho.json").write_text(json.dumps(target, indent=2) + "\n")
    print(f"Recorded {len(result['packages'])} package records, {len(result['archives'])} static archives")

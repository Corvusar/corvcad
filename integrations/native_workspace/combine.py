#!/usr/bin/env python3
"""Inspect raw native link inputs before composing the optional combined edition.

MIT. This preflight deliberately does not accept an app or issue a package. Old
independently bundled dependency archives cannot establish a common ABI.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "freecad_core/ios"))
import build_occt
import target_config

BUNDLED = {"libcorvcad_kicad_deps.a", "libcorvcad_freecad_deps.a"}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def coalesce(paths):
    """Select identical archive inputs once; never resolve differences by order."""
    selected, aliases = {}, defaultdict(list)
    for candidate in paths:
        path = candidate.resolve(strict=True)
        if not path.is_file() or path.suffix != ".a":
            raise ValueError("Expected a raw static archive: " + str(path))
        if path.name in BUNDLED:
            raise ValueError("Use original dependency archives, not bundled input: " + path.name)
        checksum = digest(path)
        if path.name in selected:
            previous, previous_hash = selected[path.name]
            if checksum != previous_hash:
                raise ValueError("Different builds of the same archive: " + str(previous) + " and " + str(path))
            aliases[str(previous)].append(str(path))
        else:
            selected[path.name] = (path, checksum)
    if not selected:
        raise ValueError("No archive inputs")
    return list(selected.values()), dict(aliases)


def strong_definitions(text):
    """Parse Apple nm -gU -m, retaining member identity and strong definitions.

    POSIX nm's T/D letters do not distinguish Mach-O weak C++ definitions.
    Weak symbols still need matching source/header ABIs; this check only catches
    concrete strong-symbol collisions that archive ordering could conceal.
    """
    lines = [line.strip() for line in text.splitlines()]
    reserved = {line[:-1] for line in lines if line.endswith(":")}
    occurrences, assigned = defaultdict(int), set()
    member, result = None, defaultdict(set)
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if line.endswith(":"):
            member = line[:-1]
            occurrences[member] += 1
            if occurrences[member] > 1:
                # Archives may contain multiple objects with the same name.
                # Keep the first diagnostic label unchanged, but distinguish
                # later occurrences without colliding with a literal filename.
                label = f"{member} [occurrence {occurrences[member]}]"
                while label in reserved or label in assigned:
                    label += " [duplicate]"
                member = label
            assigned.add(member)
            continue
        if " external " not in " " + line + " " or "(undefined)" in line:
            continue
        match = re.fullmatch(r"[0-9a-fA-F]+\s+\([^)]*\)\s+(.+?)\s+(\S+)", line)
        if not match:
            raise ValueError("Unrecognized external symbol record: " + line)
        attributes, symbol = match.groups()
        if "weak" in attributes.split():
            continue
        if member is None:
            raise ValueError("Archive symbol lacks member identity: " + symbol)
        result[symbol].add(member)
    return dict(result)


def collisions(records):
    definitions = defaultdict(set)
    for archive, symbols in records.items():
        for symbol, members in symbols.items():
            for member in members:
                definitions[symbol].add((archive, member))
    return {symbol: [{"archive": archive, "member": member} for archive, member in sorted(locations)]
            for symbol, locations in sorted(definitions.items()) if len(locations) > 1}


def inspect(paths, sdk, architecture):
    selected = target_config.target(sdk, architecture)
    inputs, aliases = coalesce(paths)
    records, symbols = {}, {}
    for path, checksum in inputs:
        arch = subprocess.check_output(["xcrun", "lipo", "-archs", str(path)], text=True).strip()
        if arch != selected.architecture:
            raise ValueError("Wrong archive architecture: " + str(path) + ": " + arch)
        load = subprocess.check_output(["xcrun", "otool", "-l", str(path)], text=True)
        count = build_occt.validate_platform(load, sdk, "17.0")
        output = subprocess.check_output(["xcrun", "nm", "-gU", "-m", str(path)], text=True)
        symbols[str(path)] = strong_definitions(output)
        if digest(path) != checksum:
            raise ValueError("Archive changed during inspection: " + str(path))
        records[str(path)] = {"sha256": checksum, "bytes": path.stat().st_size,
                              "verified_build_versions": count,
                              "strong_external_symbols": len(symbols[str(path)])}
    conflicts = collisions(symbols)
    return {"schema": 1, "status": "conflicts" if conflicts else "preflight passed",
            "target": {"sdk": sdk, "architecture": architecture, "minimum_ios": "17.0"},
            "archives": records, "identical_inputs_coalesced": aliases,
            "strong_symbol_conflicts": conflicts,
            "scope": "Raw archive preflight only. Matching package ABIs, actual source/header closure, notices, combined link and runtime tests remain required; no device or app acceptance."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    parser.add_argument("--architecture", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    report = inspect(args.archives, args.sdk, args.architecture)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Inspected {len(report['archives'])} archives; {len(report['strong_symbol_conflicts'])} strong-symbol conflicts")
    return 1 if report["strong_symbol_conflicts"] else 0


if __name__ == "__main__":
    sys.exit(main())

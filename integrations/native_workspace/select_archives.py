#!/usr/bin/env python3
"""Measure whether fixed unused archive variants can leave the native closure. MIT.

This is a conservative archive-level check, not a linker reachability proof.
Every undefined external in every retained archive is considered, including
objects a later ordinary archive link might not extract. Strong collisions,
source/header ABI compatibility and the actual link remain separate checks.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess

from combine import coalesce, digest

OMITTED = {
    "libprotobuf-lite.a": "Alternative protobuf lite runtime; retain the full protobuf runtime and verify every referenced export remains provided.",
    "libutf8_range.a": "Alternative UTF-8 validation archive; retain utf8_validity and verify every referenced export remains provided.",
    "libboost_container.a": "Unused compiled Boost.Container allocator implementation; reject omission if any retained consumer requires a unique export.",
    "libharfbuzz-subset.a": "Unused HarfBuzz subset companion; retain the primary shaping archive and reject any unique consumer requirement.",
    "libharfbuzz-gpu.a": "Unused HarfBuzz GPU companion; reject omission if any retained consumer requires a unique export.",
    "libharfbuzz-raster.a": "Unused HarfBuzz raster companion; reject omission if any retained consumer requires a unique export.",
    "libharfbuzz-vector.a": "Unused HarfBuzz vector companion; reject omission if any retained consumer requires a unique export.",
}


class OmissionError(ValueError):
    """Keep the complete failed measurement available to an evidence writer."""
    def __init__(self, report):
        self.report = report
        failures = report["missing_retained_providers"]
        names = ", ".join(sorted(failures)[:12])
        super().__init__("Retained consumers require unique omitted archive exports: " + names)


def symbols(text):
    """Parse Apple nm -g -m external definitions and imports, including weak ones.

    Keep Mach-O weak definitions as providers; treating them as strong would be
    incorrect for collision detection, but excluding them here would lose real
    providers. The compositor separately checks concrete strong collisions.
    """
    result = {"strong": set(), "weak": set(), "undefined": set()}
    member = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.endswith(":"):
            member = line[:-1]
            continue
        if not re.search(r"(?<![\w-])external(?![\w-])", line):
            continue
        match = re.fullmatch(
            r"(?:([0-9a-fA-F]+)\s+)?\(([^)]*)\)\s+(.+?)\s+(\S+)(?:\s+\(from [^)]+\))?", line)
        if not match:
            raise ValueError("Unrecognized external symbol record: " + line)
        address, section, attributes, symbol = match.groups()
        # Apple nm annotates outlined cold functions inside the attribute field.
        # This does not change whether the external definition is weak/strong.
        attributes = attributes.replace("[cold func]", "").replace("[no dead strip]", "")
        if section == "common":
            attributes = re.sub(r"\(alignment 2\^[0-9]+\)", "", attributes)
        words = attributes.split()
        if "external" not in words or any(word not in {"weak", "private", "external", "automatically", "hidden", "referenced", "dynamically"} for word in words):
            raise ValueError("Unrecognized external symbol attributes: " + line)
        if member is None:
            raise ValueError("Archive external symbol lacks member identity: " + symbol)
        if section == "undefined":
            if address is not None:
                raise ValueError("Unexpected addressed undefined external symbol: " + line)
            result["undefined"].add(symbol)
        elif address is not None and (section.startswith("__") or section in {"absolute", "common"}):
            result["weak" if "weak" in words else "strong"].add(symbol)
        else:
            raise ValueError("Unrecognized external symbol section: " + line)
    return result


def symbol_digest(values):
    encoded = json.dumps(sorted(values), separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def select(paths):
    """Return (retained raw Paths, report); fail if an omission loses a provider.

    Variant names are fixed, rather than caller-controlled. Re-run this check
    against the actual archives after every dependency rebuild. Alias byte
    identity comes from combine.coalesce; no archive is rewritten or stripped.
    """
    inputs, aliases = coalesce(paths)
    selected = [path for path, _ in inputs if path.name not in OMITTED]
    if not selected:
        raise ValueError("No retained archive inputs")
    exports, undefined, archive_records = {}, {}, {}
    retained_providers, retained_consumers = defaultdict(set), defaultdict(set)
    for path, checksum in inputs:
        output = subprocess.check_output(["xcrun", "nm", "-g", "-m", str(path)], text=True)
        parsed = symbols(output)
        if digest(path) != checksum:
            raise ValueError("Archive changed during selection: " + str(path))
        name = str(path)
        exports[name] = parsed["strong"] | parsed["weak"]
        undefined[name] = parsed["undefined"]
        archive_records[name] = {"sha256": checksum, "bytes": path.stat().st_size,
                                 "strong_exports": len(parsed["strong"]), "weak_exports": len(parsed["weak"]),
                                 "undefined_externals": len(parsed["undefined"]),
                                 "exports_sha256": symbol_digest(exports[name]),
                                 "undefined_sha256": symbol_digest(parsed["undefined"])}
        if path.name not in OMITTED:
            for symbol in exports[name]:
                retained_providers[symbol].add(name)
            for symbol in undefined[name]:
                retained_consumers[symbol].add(name)
    excluded, failures = {}, {}
    for path, _ in inputs:
        if path.name not in OMITTED:
            continue
        name = str(path)
        referenced = sorted(exports[name] & retained_consumers.keys())
        checks = {}
        for symbol in referenced:
            providers = sorted(retained_providers.get(symbol, set()))
            check = {"retained_consumers": sorted(retained_consumers[symbol]), "retained_providers": providers}
            checks[symbol] = check
            if not providers:
                failure = failures.setdefault(symbol, {"omitted_providers": [], "retained_consumers": check["retained_consumers"]})
                failure["omitted_providers"].append(name)
        excluded[name] = {**archive_records[name], "reason": OMITTED[path.name],
                          "retained_consumer_checks": checks,
                          "exports_without_retained_consumer": len(exports[name]) - len(referenced),
                          "unique_exports_without_retained_consumer": sum(
                              symbol not in retained_providers and symbol not in retained_consumers for symbol in exports[name])}
    report = {"schema": 1, "status": "rejected" if failures else "archive variants selected",
              "selected_archives": {str(path): archive_records[str(path)] for path in selected},
              "excluded_archives": excluded, "identical_inputs_coalesced": aliases,
              "fixed_variants_not_present": sorted(set(OMITTED) - {path.name for path, _ in inputs}),
              "missing_retained_providers": failures,
              "scope": "Measured fixed archive omissions only. No object stripping or collision allowlist; source/header ABI, strong-symbol collisions, actual link and runtime acceptance remain required."}
    if failures:
        raise OmissionError(report)
    return selected, report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        _, report = select(args.archives)
        exit_code = 0
    except OmissionError as error:
        report, exit_code = error.report, 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(exit_code)

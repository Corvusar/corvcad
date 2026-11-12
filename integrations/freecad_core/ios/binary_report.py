#!/usr/bin/env python3
"""Verify the probe's Mach-O platform/architecture and measure its file size.

`size` includes the reserved 4 GiB __PAGEZERO region; it is not file size.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import target_config


def check(platform_text, dynamic_text, sdk):
    expected = {"iphoneos": "IOS", "iphonesimulator": "IOSSIMULATOR"}[sdk]
    if not re.search(r"^\s*platform\s+" + expected + r"\s*$", platform_text, re.M):
        raise ValueError("Mach-O platform does not match target SDK")
    if not re.search(r"^\s*minos\s+17\.0\s*$", platform_text, re.M):
        raise ValueError("Unexpected minimum iOS version")
    links = []
    for line in dynamic_text.splitlines()[1:]:
        if not line.strip():
            continue
        path = line.strip().split(" (", 1)[0]
        if not path.startswith(("/usr/lib/", "/System/Library/Frameworks/")):
            raise ValueError("Non-system dynamic dependency in embedding probe: " + path)
        links.append(path)
    if not links:
        raise ValueError("Missing dynamic link metadata")
    return links


if __name__ == "__main__":
    binary = Path(sys.argv[1])
    sdk = sys.argv[2]
    selected = target_config.target(sdk, sys.argv[3] if len(sys.argv) > 3 else "arm64")
    arch = subprocess.check_output(["xcrun", "lipo", "-archs", str(binary)], text=True).strip()
    if arch != selected.architecture:
        raise ValueError("Probe architecture does not match requested target")
    platform_text = (binary.parent / "macho-build.txt").read_text()
    links = check(platform_text, (binary.parent / "dynamic-links.txt").read_text(), sdk)
    report = {"schema": 1, "sdk": sdk, "architecture": arch,
              "executable_file_bytes": binary.stat().st_size,
              "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "dynamic_system_links": links}
    (binary.parent / "binary-metadata.json").write_text(json.dumps(report, indent=2) + "\n")

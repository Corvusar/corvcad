#!/usr/bin/env python3
"""Create an isolated iPad simulator from installed compatible types/runtimes."""
import json
from pathlib import Path
import subprocess
import sys


def version(value):
    parts = [int(p) for p in value.split(".")]
    parts += [0] * (3 - len(parts))
    return (parts[0] << 16) + (parts[1] << 8) + parts[2]


def select(runtimes, types):
    available = [r for r in runtimes if r.get("isAvailable") and
                 ".iOS-" in r["identifier"] and version(r["version"]) >= version("17.0")]
    for runtime in sorted(available, key=lambda r: version(r["version"]), reverse=True):
        compatible = [t for t in types if t["name"].startswith("iPad") and
                      t.get("minRuntimeVersion", 0) <= version(runtime["version"]) <= t.get("maxRuntimeVersion", 0xFFFFFFFF)]
        if compatible:
            return runtime, compatible[-1]
    raise ValueError("No compatible installed iPad/iOS 17+ simulator runtime")


if __name__ == "__main__":
    runtimes = json.loads(subprocess.check_output(["xcrun", "simctl", "list", "runtimes", "-j"]))["runtimes"]
    types = json.loads(subprocess.check_output(["xcrun", "simctl", "list", "devicetypes", "-j"]))["devicetypes"]
    runtime, device_type = select(runtimes, types)
    identifier = subprocess.check_output(["xcrun", "simctl", "create", "CorvCAD F3 Python",
                                         device_type["identifier"], runtime["identifier"]], text=True).strip()
    Path(sys.argv[1]).write_text(json.dumps({"runtime": runtime, "type": device_type, "device": identifier}, indent=2) + "\n")
    print(identifier)

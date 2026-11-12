#!/usr/bin/env python3
"""Verify actual static FreeCAD targets/imports, then repeat desktop F1 suites.

This is a development check on macOS; it does not accept F3's iOS gate.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

import prepare_static

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import inventory
import test_slice


def check_artifacts(records):
    # Upstream builds bundled E57Format even with Points disabled. Record and
    # verify that auxiliary archive as well; it is not a Python extension.
    expected = {"FreeCADBase", "FreeCADApp", "E57Format", *prepare_static.PROFILE["module_targets"]}
    libraries = {r["target"]: r for r in records if r["type"].endswith("LIBRARY")}
    if set(libraries) != expected:
        raise ValueError("Unexpected static library closure: " + repr(sorted(libraries)))
    if any(r["type"] != "STATIC_LIBRARY" or not r["produced"] for r in libraries.values()):
        raise ValueError("Static FreeCAD libraries were not produced")
    programs = [r for r in records if r["target"] == "FreeCADMainCmd" and r["type"] == "EXECUTABLE"]
    if len(programs) != 1 or not programs[0]["produced"]:
        raise ValueError("Static FreeCADCmd was not produced")
    return programs[0]


def run(source, build, output):
    report = prepare_static.verify(source)
    records = inventory.target_artifacts(build)
    program = check_artifacts(records)
    executable = build / program["path"]
    output.mkdir(parents=True, exist_ok=False)
    links = subprocess.check_output(["otool", "-L", str(executable)], text=True)
    (output / "dynamic-links.txt").write_text(links)
    if inventory.gui_dependency(links) or any(name in links for name in ("FreeCADApp", "FreeCADBase", "/Part.so", "/Import.so", "/_PartDesign.so")):
        raise ValueError("Dynamic FreeCAD/GUI library leaked into the static profile")
    static_report = output / "static-imports.json"
    env = {**os.environ, "CORVCAD_STATIC_REPORT": str(static_report)}
    result = subprocess.run([str(executable), "-u", str(output / "static-user.cfg"),
                             "-s", str(output / "static-system.cfg"), str(Path(__file__).with_name("static_probe.py"))],
                            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=300)
    (output / "static-imports.log").write_bytes(result.stdout)
    if result.returncode or not static_report.exists() or json.loads(static_report.read_text()).get("passed") is not True:
        raise ValueError("Static FreeCAD import probe failed; inspect static-imports.log")
    regression = output / "regression"
    regression.mkdir()
    test_slice.main(build, regression)
    report.update({"status": "static desktop regression passed; iOS acceptance remains open",
                   "platform": platform.platform(), "artifacts": records,
                   "executable_file_bytes": executable.stat().st_size,
                   "imports": json.loads(static_report.read_text())})
    (output / "static-desktop.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("build/freecad-ios/FreeCAD"))
    parser.add_argument("--build", type=Path, default=Path("build/freecad-ios/static-desktop"))
    parser.add_argument("--output", type=Path, default=Path("build/freecad-ios/static-evidence"))
    args = parser.parse_args()
    run(args.source.resolve(), args.build.resolve(), args.output.resolve())

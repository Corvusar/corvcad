#!/usr/bin/env python3
"""Run the F1 proof in stock FreeCADCmd; require a fresh, complete success report."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(executable, data, output, launcher_args=()):
    # A fresh directory prevents stale successful reports from masking a crash.
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["CORVCAD_FREECAD_INPUT"] = str(data.resolve())
    environment["CORVCAD_FREECAD_OUTPUT"] = str(output.resolve())
    command = [str(executable), *launcher_args, "-u", str(output.resolve() / "user.cfg"),
               "-s", str(output.resolve() / "system.cfg"), str(Path(__file__).with_name("proof.py").resolve())]
    completed = subprocess.run(command, env=environment, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    (output / "freecad.log").write_bytes(completed.stdout)
    report_path = output / "report.json"
    if completed.returncode or not report_path.exists():
        raise RuntimeError(f"FreeCAD proof failed (exit {completed.returncode}); see {output / 'freecad.log'}")
    report = json.loads(report_path.read_text())
    if report.get("status") != "passed" or report["input_sha256"] != hashlib.sha256(data.read_bytes()).hexdigest():
        raise RuntimeError("Invalid proof report")
    for name, record in report["files"].items():
        artifact = output / name
        if hashlib.sha256(artifact.read_bytes()).hexdigest() != record["sha256"]:
            raise RuntimeError(f"Artifact hash mismatch: {name}")
    print(f"PASS {data.name}: {report['bore_count']} bores; 2 STEP solids; closed tray/lid STL; 8 parameter recomputes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freecad", type=Path, required=True, help="Stock 1.1.4 FreeCADCmd executable")
    parser.add_argument("--freecad-arg", action="append", default=[], help="Argument before FreeCAD options (AppRun: freecadcmd)")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Must not already exist")
    args = parser.parse_args()
    run(args.freecad, args.input, args.output, args.freecad_arg)

#!/usr/bin/env python3
"""Run upstream tests and the accepted F1 inputs against a source-built slice."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from run import run

HERE = Path(__file__).resolve().parent


def main(build, output):
    candidates = [p for p in (build / "bin").iterdir() if p.name.lower() == "freecadcmd"]
    if len(candidates) != 1:
        raise RuntimeError("Expected exactly one source-built FreeCADCmd")
    executable = candidates[0].resolve()
    report_path = output / "upstream-tests.json"
    if report_path.exists():
        raise FileExistsError("Refusing to reuse an upstream test report")
    env = {**os.environ, "CORVCAD_FREECAD_TEST_REPORT": str(report_path.resolve())}
    result = subprocess.run([str(executable), "-u", str(output.resolve()/"tests-user.cfg"),
                             "-s", str(output.resolve()/"tests-system.cfg"), str(HERE/"upstream_tests.py")],
                            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1200)
    (output / "upstream-tests.log").write_bytes(result.stdout)
    # Still run both geometry proofs if upstream tests fail, retaining independent diagnostics.
    for name, board in (("demo", "demo.kicad_pcb"), ("roundtrip", "roundtrip/RoundTrip.kicad_pcb")):
        fixture = HERE / "fixtures" / f"{name}.json"
        data = json.loads(fixture.read_text())
        source = HERE.parents[1] / "engine/tests/fixtures" / board
        if hashlib.sha256(source.read_bytes().replace(b"\r\n", b"\n")).hexdigest() != data["source"]["sha256"]:
            raise RuntimeError("F1 input snapshot no longer matches its KiCad source fixture")
        run(executable, fixture, output / f"{name}-proof")
    if result.returncode or not report_path.exists() or json.loads(report_path.read_text())["status"] != "passed":
        raise RuntimeError("Upstream App suite failed; inspect the retained test report/log")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    main(args.build.resolve(), args.output.resolve())

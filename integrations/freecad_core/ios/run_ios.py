#!/usr/bin/env python3
"""Validate a linked iOS runtime and run unchanged F1 proofs in an owned simulator.

MIT. A device-target check only validates linkage; it never executes on a device.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import subprocess
import sys

import binary_report
import stage_runtime
import target_config

sys.path.insert(0, str(stage_runtime.CORE))
from checks import close, expected
from inputs import validate

OUTPUTS = {"enclosure.FCStd", "enclosure.step", "tray.step", "lid.step", "tray.stl", "lid.stl"}
PARAMETERS = {"wall", "floor", "standoff_height", "standoff_diameter", "margin",
              "board_clearance", "lid_thickness", "lid_gap"}
BUILTINS = {"FreeCAD", "__FreeCADBase__", "Materials", "Part", "Mesh", "Sketcher", "_PartDesign", "Spreadsheet", "Import"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_runtime(runtime):
    manifest = json.loads((runtime / "runtime-manifest.json").read_text())
    if manifest.get("schema") != 1 or manifest.get("executable") != "bin/FreeCADCmd" or manifest.get("stdlib") != "lib/python3.13":
        raise ValueError("Unrecognized staged runtime")
    actual = {path.relative_to(runtime).as_posix() for path in runtime.rglob("*") if path.is_file()}
    if actual != {*manifest["files"], "runtime-manifest.json"}:
        raise ValueError("Runtime file set differs from its staged manifest")
    for name, record in manifest["files"].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name:
            raise ValueError("Runtime manifest path escapes its root")
        path = stage_runtime.checked_file(runtime / name, runtime)
        if path.stat().st_size != record["bytes"] or sha(path) != record["sha256"]:
            raise ValueError("Staged runtime was modified: " + name)
    if sum(record["bytes"] for record in manifest["files"].values()) != manifest["file_bytes"]:
        raise ValueError("Runtime byte inventory is inconsistent")
    return manifest


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError("Expected a finite numeric measurement")
    return value


def check_parts(parts, targets):
    if set(parts) != {"Tray", "Lid"}:
        raise ValueError("Expected exactly tray and lid geometry")
    for name, target in targets.items():
        actual = parts[name]
        if actual.get("valid_solids") != 1 or len(actual["bounds_mm"]) != 6:
            raise ValueError("Invalid solid or bounds report")
        for value, expected_value in zip(actual["bounds_mm"], target["bounds"]):
            close(number(value), expected_value)
        close(number(actual["volume_mm3"]), target["volume"], max(1e-5, target["volume"] * 1e-8))


def compare_desktop(report, reference):
    if reference.get("status") != "passed" or reference["input_sha256"] != report["input_sha256"]:
        raise ValueError("Desktop reference does not represent the same accepted input")
    if report["settings_mm"] != reference["settings_mm"] or report["bore_count"] != reference["bore_count"]:
        raise ValueError("Desktop settings or bore count differ")
    for key in ("parts", "step_reimport"):
        targets = {name: {"bounds": values["bounds_mm"], "volume": values["volume_mm3"]}
                   for name, values in reference[key].items()}
        check_parts(report[key], targets)
    by_name = {record["parameter"]: record for record in reference["parameter_recomputes"]}
    for candidate in report["parameter_recomputes"]:
        previous = by_name[candidate["parameter"]]
        if candidate["value_mm"] != previous["value_mm"]:
            raise ValueError("Desktop parameter recompute input differs")
        targets = {name: {"bounds": values["bounds_mm"], "volume": values["volume_mm3"]}
                   for name, values in previous["parts"].items()}
        check_parts(candidate["parts"], targets)


def verify_proof(output, fixture, harness, reference):
    report = json.loads((output / "report.json").read_text())
    metrics = json.loads((output / "ios-metrics.json").read_text())
    data = json.loads(fixture.read_text())
    settings = validate(data)
    lock = json.loads((harness / "upstream.json").read_text())
    version = report.get("freecad_version", [])
    if report.get("status") != "passed" or report.get("input_sha256") != sha(fixture):
        raise ValueError("Invalid or stale F1 proof report")
    if version[:3] != lock["source"]["tag"].split(".") or version[-1:] != [lock["source"]["commit"]]:
        raise ValueError("F1 report did not use the pinned FreeCAD source")
    if report["board_source"] != data["source"] or report["settings_mm"] != settings:
        raise ValueError("F1 report settings or source differ")
    if report["tools_sha256"] != {name: sha(harness / name) for name in stage_runtime.PROOF_FILES}:
        raise ValueError("F1 did not execute the unchanged staged proof sources")
    if set(report["files"]) != OUTPUTS:
        raise ValueError("Incomplete or unexpected F1 output set")
    for name, record in report["files"].items():
        path = output / name
        if not path.is_file() or path.is_symlink() or path.stat().st_size <= 0:
            raise ValueError("Missing or empty F1 output: " + name)
        if path.stat().st_size != record["bytes"] or sha(path) != record["sha256"]:
            raise ValueError("F1 output hash or size differs: " + name)
    if report["bore_count"] != len(data["mounting_holes"]) or report["save_reopen_recompute"] != "passed":
        raise ValueError("F1 bore/save-reopen verification failed")
    targets = expected(data, settings)
    check_parts(report["parts"], targets)
    check_parts(report["step_reimport"], targets)
    changes = report["parameter_recomputes"]
    if len(changes) != 8 or {record["parameter"] for record in changes} != PARAMETERS:
        raise ValueError("Missing independent parameter recomputes")
    for change in changes:
        candidate = {**settings, change["parameter"]: number(change["value_mm"])}
        validate(data, candidate)
        check_parts(change["parts"], expected(data, candidate))
    for name in ("Tray", "Lid"):
        mesh = report["stl"][name]
        if mesh["triangles"] <= 0 or mesh["edges"] <= 0:
            raise ValueError("Empty STL verification report")
        close(number(mesh["signed_volume_mm3"]), targets[name]["volume"], max(0.1, targets[name]["volume"] * 0.005))
    if set(report["synthetic_model_regressions"]) != {"no_mounts", "mixed_bores_and_negative_origin"}:
        raise ValueError("Missing synthetic F1 model checks")
    if number(report["elapsed_seconds"]) <= 0:
        raise ValueError("Invalid F1 elapsed time")
    if metrics.get("passed") is not True or metrics.get("platform") != "ios":
        raise ValueError("No actual iOS runtime measurement")
    if metrics["extension_origins"] != {name: "built-in" for name in BUILTINS}:
        raise ValueError("FreeCAD modules did not all resolve from the inittab")
    durations = metrics["recompute_call_seconds"]
    if not durations or metrics["recompute_calls"] != len(durations) or any(number(t) < 0 for t in durations):
        raise ValueError("Incomplete recompute timing measurements")
    close(number(metrics["recompute_total_seconds"]), sum(durations), 1e-9)
    close(number(metrics["recompute_max_seconds"]), max(durations), 1e-9)
    if number(metrics["peak_process_rss_bytes"]) <= 0:
        raise ValueError("Missing peak process RSS measurement")
    desktop = json.loads(reference.read_text())
    compare_desktop(report, desktop)
    return {"input_sha256": sha(fixture), "desktop_reference_sha256": sha(reference),
            "desktop_geometry_comparison": "passed", "metrics": metrics,
            "files": report["files"], "parts": report["parts"], "step_reimport": report["step_reimport"]}


def child_environment(runtime, output, fixture):
    env = {key: value for key, value in os.environ.items() if not key.startswith("SIMCTL_CHILD_")}
    values = {"CORVCAD_PYTHON_STDLIB": runtime / "lib/python3.13",
              "FREECAD_USER_HOME": output / "home", "FREECAD_USER_DATA": output / "data",
              "FREECAD_USER_TEMP": output / "temp", "CORVCAD_FREECAD_INPUT": fixture,
              "CORVCAD_FREECAD_OUTPUT": output, "CORVCAD_IOS_METRICS": output / "ios-metrics.json"}
    for key in ("FREECAD_USER_HOME", "FREECAD_USER_DATA", "FREECAD_USER_TEMP"):
        values[key].mkdir()
    env.update({"SIMCTL_CHILD_" + key: str(value) for key, value in values.items()})
    env["SIMCTL_CHILD_PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def cleanup_simulator(identifier, output):
    actions = {}
    for operation in ("shutdown", "delete"):
        try:
            result = subprocess.run(["xcrun", "simctl", operation, identifier], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, timeout=60)
            actions[operation] = {"returncode": result.returncode}
        except (OSError, subprocess.TimeoutExpired) as error:
            actions[operation] = {"error": str(error)}
    (output / "simulator-cleanup.json").write_text(json.dumps({"device": identifier, "actions": actions}, indent=2) + "\n")
    return actions["delete"].get("returncode") == 0


def run(runtime, sdk, output, references, architecture="arm64"):
    target_config.target(sdk, architecture)
    if platform.system() != "Darwin":
        raise ValueError("iOS binary validation requires macOS and Xcode")
    manifest = verify_runtime(runtime)
    if manifest.get("architecture", "arm64") != architecture:
        raise ValueError("Staged runtime architecture does not match requested target")
    output.mkdir(parents=True, exist_ok=False)
    executable = runtime / manifest["executable"]
    actual_architecture = subprocess.check_output(["xcrun", "lipo", "-archs", str(executable)], text=True).strip()
    if actual_architecture != architecture:
        raise ValueError("FreeCAD runtime architecture does not match requested target")
    build_metadata = subprocess.check_output(["xcrun", "vtool", "-show-build", str(executable)], text=True)
    dynamic = subprocess.check_output(["otool", "-L", str(executable)], text=True)
    links = binary_report.check(build_metadata, dynamic, sdk)
    (output / "macho-build.txt").write_text(build_metadata)
    (output / "dynamic-links.txt").write_text(dynamic)
    summary = {"schema": 1, "sdk": sdk, "architecture": architecture,
               "executable_file_bytes": executable.stat().st_size, "executable_sha256": sha(executable),
               "runtime_file_bytes": manifest["file_bytes"], "dynamic_system_links": links,
               "runtime_manifest_sha256": sha(runtime / "runtime-manifest.json")}
    if sdk == "iphoneos":
        summary["status"] = "device target link/platform validated; no device execution"
        (output / "ios-link.json").write_text(json.dumps(summary, indent=2) + "\n")
        return summary
    metadata = output / "simulator.json"
    identifier = subprocess.check_output([sys.executable, str(stage_runtime.HERE / "simulator.py"), str(metadata)], text=True).strip()
    owned = json.loads(metadata.read_text())
    if not identifier or identifier != owned["device"]:
        raise ValueError("Simulator creation did not return its owned identifier")
    proofs = {}
    try:
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True, timeout=120)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=300)
        for name in ("demo", "roundtrip"):
            case = output / (name + "-proof")
            case.mkdir()
            fixture = runtime / "harness/fixtures" / (name + ".json")
            environment = child_environment(runtime, case, fixture)
            command = ["xcrun", "simctl", "spawn", identifier, str(executable),
                       "--set-config", "AppHomePath=" + str(runtime) + "/",
                       "-u", str(case / "user.cfg"), "-s", str(case / "system.cfg"), str(runtime / "harness/ios_proof.py")]
            completed = subprocess.run(command, env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, timeout=1200)
            (case / "freecad.log").write_bytes(completed.stdout)
            if completed.returncode or not (case / "report.json").is_file() or not (case / "ios-metrics.json").is_file():
                raise ValueError("Simulator F1 proof failed; inspect " + str(case / "freecad.log"))
            proofs[name] = verify_proof(case, fixture, runtime / "harness", references / ("f2-accepted-macos-" + name + ".json"))
        verify_runtime(runtime)  # Resources must remain unchanged after both runs.
    finally:
        failing = sys.exc_info()[0] is not None
        if not cleanup_simulator(identifier, output) and not failing:
            raise RuntimeError("Could not delete the owned simulator; inspect simulator-cleanup.json")
    summary.update({"status": "simulator F1 proofs passed with static FreeCAD modules", "simulator": owned,
                    "proofs": proofs, "peak_process_rss_bytes": max(p["metrics"]["peak_process_rss_bytes"] for p in proofs.values()),
                    "scope": "Simulator execution; no physical iPad validation or app integration"})
    (output / "ios-simulator.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--references", type=Path, default=stage_runtime.CORE / "evidence")
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, default="arm64")
    args = parser.parse_args()
    run(args.runtime.resolve(), args.sdk, args.output.resolve(), args.references.resolve(), args.architecture)

#!/usr/bin/env python3
"""Run both mixed native C ABI initialization orders in an owned simulator. MIT."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "integrations/freecad_core/ios"))
import binary_report
import simulator

ORDERS = ("kicad-first", "freecad-first")
EXPORTS = {"enclosure.FCStd", "tray.step", "lid.step", "tray.stl", "lid.stl"}
OPERATIONS = {"both_native_engines", "interleaved_transactions", "undo_redo", "rejected_mechanical_edit",
              "netlist_erc_drc_refill", "schematic_save_copy_reopen", "pcb_board_setup_edit_reopen",
              "mechanical_save_reopen", "source_bytes_preserved", "step_stl"}
NATIVE_BACKEND = "kicad-10.0.6-native"


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def record(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected an actual regular file: " + str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def snapshot(root):
    """Raw bytes of every runtime/fixture file, rejecting linked or special inputs."""
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Expected an actual directory: " + str(root))
    files = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError("Linked or special input: " + str(path))
        if path.is_file():
            files[path.relative_to(root).as_posix()] = record(path)
    if not files:
        raise ValueError("Expected a nonempty input directory: " + str(root))
    return files


def unchanged(root, before):
    after = snapshot(root)
    if after != before:
        changed = sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))
        raise ValueError("Input files changed under " + str(root) + ": " + ", ".join(changed[:20]))
    return after


def check_board_edit(case, edit, original):
    if not isinstance(edit, dict) or edit.get("copy") != "board-copy" or edit.get("rule") != "min_clearance":
        raise ValueError("Missing actual native Board Setup edit/copy report")
    # Derive filenames from the measured original tree rather than trusting a
    # report path. This also rejects absolute paths, traversal and wrong pairs.
    projects = [name for name in original if Path(name).suffix == ".kicad_pro"]
    if len(projects) != 1:
        raise ValueError("Expected exactly one original KiCad project")
    project = projects[0]
    board = Path(project).with_suffix(".kicad_pcb").as_posix()
    if board not in original or edit.get("board_file") != board or edit.get("project_file") != project:
        raise ValueError("Board edit copy filenames do not match the original project pair")
    copied = snapshot(case / "board-copy")
    if set(copied) != set(original):
        raise ValueError("Board edit copy lost or added original project files")
    for name, expected in original.items():
        if name not in {board, project} and copied[name] != expected:
            raise ValueError("Board edit copy changed another original file: " + name)
    for name, key in ((board, "board"), (project, "project")):
        if copied[name] != edit.get(key) or not copied[name]["bytes"]:
            raise ValueError("Board edit copy changed after process exit: " + name)
    before, after = edit.get("before_mm"), edit.get("after_mm")
    if any(type(value) not in (int, float) or not math.isfinite(value) for value in (before, after)):
        raise ValueError("Board Setup clearance edit must contain finite sizes")
    delta = 0.025 if before < 999 else -0.025
    if before == after or not math.isclose(after - before, delta, rel_tol=0, abs_tol=1e-9):
        raise ValueError("Board Setup clearance edit has an unexpected delta")
    lines = {}
    for name, expected in (("before", before), ("after", after)):
        path = case / ("board-setup-" + name + ".txt")
        if record(path)["sha256"] != edit.get("setup_" + name + "_sha256"):
            raise ValueError("Board Setup snapshot changed after process exit: " + name)
        lines[name] = path.read_bytes().split(b"\n")
        rules = [(index, line.split(b"\t")) for index, line in enumerate(lines[name]) if line.startswith(b"rule\tmin_clearance\t")]
        if len(rules) != 1 or len(rules[0][1]) != 3:
            raise ValueError("Board Setup snapshot lacks one actual clearance rule")
        index, fields = rules[0]
        actual = float(fields[2])
        # mixed.cpp deliberately matches the native six-significant-digit serializer.
        serialized = float(format(expected, ".6g"))
        if not math.isfinite(actual) or not math.isclose(actual, serialized, rel_tol=0, abs_tol=1e-12):
            raise ValueError("Board Setup snapshot clearance differs from its edit report")
        lines[name][index] = b"<edited clearance>"
    if lines["before"] != lines["after"] or edit.get("setup_before_sha256") == edit.get("setup_after_sha256"):
        raise ValueError("Board Setup edit must change only the requested clearance rule")


def check_case(case, order, source, original):
    if case.is_symlink() or not case.is_dir():
        raise ValueError("Expected an actual owned case directory")
    report = json.loads((case / "mixed-report.json").read_text())
    if report.get("schema") != 1 or report.get("passed") is not True or report.get("order") != order:
        raise ValueError("Missing successful mixed native C ABI report for " + order)
    if any(report.get(key) != NATIVE_BACKEND for key in ("schematic_backend", "board_backend")):
        raise ValueError("Mixed report does not identify both actual native KiCad backends")
    capabilities = report.get("capabilities")
    if type(capabilities) is not int or capabilities & 57 != 57:
        raise ValueError("Mixed report lacks required native KiCad capabilities")
    operations = report.get("operations")
    if not isinstance(operations, dict) or any(operations.get(key) is not True for key in OPERATIONS):
        raise ValueError("Mixed report lacks required successful native operations")
    if report.get("source_inputs") != original:
        raise ValueError("Probe source hashes differ from the pre-process fixture snapshot")
    unchanged(source, original)
    copied = snapshot(case / "project")
    for relative, expected in original.items():
        if copied.get(relative) != expected:
            raise ValueError("Copied original project changed after process exit: " + relative)
    check_board_edit(case, report.get("board_edit"), original)
    exports = report.get("exports", {})
    if set(exports) != EXPORTS:
        raise ValueError("Missing mixed native exports")
    for name, expected in exports.items():
        path = case / "project" / name if name == "enclosure.FCStd" else case / name
        if record(path) != expected or not expected["bytes"]:
            raise ValueError("Native export changed or is empty after process exit: " + name)
    if record(case / "input.json")["sha256"] != report.get("input_json_sha256"):
        raise ValueError("Actual mechanical input differs from the native report")
    return report


def environment(config):
    env = {key: value for key, value in os.environ.items() if not key.startswith("SIMCTL_CHILD_")}
    # Neither backend may inherit a user configuration or host Python overrides.
    values = {"HOME": config / "home", "CFFIXED_USER_HOME": config / "home",
              "KICAD_CONFIG_HOME": config / "kicad", "XDG_CONFIG_HOME": config / "xdg",
              "TMPDIR": config / "tmp", "PYTHONDONTWRITEBYTECODE": "1",
              "PYTHONPATH": "", "PYTHONHOME": ""}
    for name in ("home", "kicad", "xdg", "tmp"):
        (config / name).mkdir(parents=True, exist_ok=False)
    env.update({"SIMCTL_CHILD_" + key: str(value) for key, value in values.items()})
    return env


def cleanup(identifier, output):
    """Only the UUID returned by this run's create call is ever operated on."""
    actions = {}
    for operation in ("shutdown", "delete"):
        try:
            result = subprocess.run(["xcrun", "simctl", operation, identifier], capture_output=True, text=True, timeout=60)
            actions[operation] = {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
        except (OSError, subprocess.TimeoutExpired) as error:
            actions[operation] = {"error": str(error)}
    write_json(output / "simulator-cleanup.json", {"device": identifier, "actions": actions})
    return actions["delete"].get("returncode") == 0


def validate(binary, runtime, source, fixture, output, architecture="x86_64", timeout=1200):
    # Resolve parents before claiming output; refuse any overlap with protected inputs.
    binary, runtime, source, fixture, output = (path.absolute() for path in (binary, runtime, source, fixture, output))
    for protected in (runtime, source):
        if output.resolve().is_relative_to(protected.resolve()) or protected.resolve().is_relative_to(output.resolve()):
            raise ValueError("Probe output must not overlap runtime or source project")
    for protected in (binary, fixture):
        if protected.resolve().is_relative_to(output.resolve()):
            raise ValueError("Probe output must not contain an input file")
    original, resources = snapshot(source), snapshot(runtime)
    input_record, binary_record = record(fixture), record(binary)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"schema": 1, "passed": False, "sdk": "iphonesimulator", "architecture": architecture,
               "scope": "Combined native C ABI smoke test; no app or physical-device acceptance",
               "binary": binary_record, "fixture": input_record, "source_inputs": original,
               "runtime_before": resources, "runtime_file_count": len(resources), "cases": {}}
    identifier = None
    try:
        arch = subprocess.check_output(["xcrun", "lipo", "-archs", str(binary)], text=True).strip()
        if arch != architecture:
            raise ValueError("Mixed probe architecture differs from requested simulator target")
        build = subprocess.check_output(["xcrun", "vtool", "-show-build", str(binary)], text=True)
        links = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        summary["system_links"] = binary_report.check(build, links, "iphonesimulator")
        (output / "macho-build.txt").write_text(build)
        (output / "dynamic-links.txt").write_text(links)
        runtimes = json.loads(subprocess.check_output(["xcrun", "simctl", "list", "runtimes", "-j"]))["runtimes"]
        types = json.loads(subprocess.check_output(["xcrun", "simctl", "list", "devicetypes", "-j"]))["devicetypes"]
        selected, device_type = simulator.select(runtimes, types)
        created = subprocess.check_output(["xcrun", "simctl", "create", "CorvCAD mixed native C ABI",
                                           device_type["identifier"], selected["identifier"]], text=True).strip()
        identifier = str(uuid.UUID(created))
        metadata = {"device": identifier, "runtime": selected, "type": device_type}
        write_json(output / "simulator.json", metadata)
        summary["simulator"] = metadata
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True, timeout=120)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=300)
        for order in ORDERS:
            case = output / order  # C++ probe itself must create this fresh directory.
            entry = {"passed": False}
            summary["cases"][order] = entry
            env = environment(output / (order + "-config"))
            command = ["xcrun", "simctl", "spawn", "--standalone", identifier, str(binary), order,
                       str(runtime), str(source), str(fixture), str(case)]
            entry["command"] = command
            with (output / (order + ".log")).open("wb") as stream:
                process = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=timeout)
            entry["returncode"] = process.returncode
            if process.returncode:
                raise RuntimeError("Mixed probe process failed for " + order + "; retained log/output at " + str(output))
            entry["report"] = check_case(case, order, source, original)
            unchanged(runtime, resources)
            if record(fixture) != input_record or record(binary) != binary_record:
                raise ValueError("Mixed probe changed a protected input file")
            entry["passed"] = True
        summary["passed"] = True
    except BaseException as error:
        summary["error"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        failing = sys.exc_info()[0] is not None
        cleanup_ok = cleanup(identifier, output) if identifier else False
        if not identifier:
            write_json(output / "simulator-cleanup.json", {"device": None, "actions": {}, "reason": "No owned simulator created"})
        summary["simulator_deleted"] = cleanup_ok
        preservation_errors = []
        for name, root, before in (("runtime", runtime, resources), ("source", source, original)):
            try:
                unchanged(root, before)
                summary[name + "_unchanged"] = True
            except (OSError, ValueError) as error:
                summary[name + "_unchanged"] = False
                preservation_errors.append(str(error))
        for name, path, before in (("fixture", fixture, input_record), ("binary", binary, binary_record)):
            try:
                if record(path) != before:
                    raise ValueError("Protected " + name + " changed after process exit")
                summary[name + "_unchanged"] = True
            except (OSError, ValueError) as error:
                summary[name + "_unchanged"] = False
                preservation_errors.append(str(error))
        summary["passed"] = summary["passed"] and cleanup_ok and not preservation_errors
        if preservation_errors:
            summary["preservation_errors"] = preservation_errors
        write_json(output / "mixed-simulator.json", summary)
        if not failing and not summary["passed"]:
            raise RuntimeError("Mixed validation cleanup/preservation failed; inspect " + str(output))
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--source-project", type=Path, default=REPO / "engine/tests/fixtures/roundtrip")
    parser.add_argument("--input", type=Path, default=REPO / "integrations/freecad_core/fixtures/roundtrip.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--architecture", choices=("x86_64", "arm64"), default="x86_64")
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()
    validate(args.binary, args.runtime, args.source_project, args.input, args.output, args.architecture, args.timeout)

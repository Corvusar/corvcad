"""Run real hidden diagnostics in a new owned iPad simulator. MIT. Never physical F5 evidence."""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import time

from collect import OUTPUTS, validate_mechanical_reply
from packet import inside, record, save, verify

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ios"))
from simulator import select


def inventory(root):
    if not root.is_dir() or root.is_symlink():
        raise ValueError("Missing regular resource directory")
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("Linked resource is not accepted: " + str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = record(path)
    if not result:
        raise ValueError("Empty runtime resources")
    return result


def verify_package_manifest(path, resources):
    receipt = json.loads(path.read_text())
    if receipt.get("schema") != 1 or receipt.get("sdk") != "iphonesimulator" or set(receipt.get("resources", {})) != set(resources):
        raise ValueError("Package receipt does not represent this simulator runtime")
    for name, actual in resources.items():
        if any(receipt["resources"][name].get(key) != value for key, value in actual.items()):
            raise ValueError("App resource differs from package receipt: " + name)
    return receipt


def validate_rehearsal(reply, expected_input, resources):
    validate_mechanical_reply(reply, expected_input, "simulator")
    if (reply.get("rehearsal_only") is not True or reply.get("physical_footprint_measured") is not False
            or reply.get("runtime_initialized_before") is not False or reply.get("native_available") is not True):
        raise ValueError("Expected a fresh-process, simulator-only native rehearsal")
    required = {"harness/" + name for name in ("upstream.json", "session_runtime.py", "model.py", "inputs.py", "checks.py")}
    sources = reply.get("runtime_sources", {})
    if set(sources) != required or any(sources[name] != resources.get(name) for name in required):
        raise ValueError("Running diagnostic helper identity differs from app resources")
    if set(reply.get("settings_mm", {})) != {"margin", "wall", "floor", "board_clearance", "standoff_height", "standoff_diameter", "lid_thickness", "lid_gap"}:
        raise ValueError("Missing retained parameter values")
    return reply


def copy_outputs(source, destination, reply):
    for name, expected in reply["files"].items():
        path = inside(source, name)
        if record(path) != expected:
            raise ValueError("Actual simulator output differs from reported hash: " + name)
        shutil.copyfile(path, destination / name)
        if record(destination / name) != expected:
            raise ValueError("Copied simulator output differs: " + name)
    if json.loads((destination / "input.json").read_text()) != reply["input"]:
        raise ValueError("Actual simulator input differs from notification report")
    durable = inside(source, "device-report.json")
    report = json.loads(durable.read_text())
    if any(reply.get(key) != value for key, value in report.items()):
        raise ValueError("Durable report differs from the simulator reply")
    shutil.copyfile(durable, destination / "device-report.json")


def rehearse(app, packet, output, package_manifest=None, receipts=()):
    if platform.system() != "Darwin":
        raise ValueError("Simulator rehearsal requires macOS and Xcode")
    manifest = verify(packet)
    resources = inventory(app / "FreeCADRuntime")
    info = plistlib.loads((app / "Info.plist").read_bytes())
    bundle_id = info.get("CFBundleIdentifier")
    executable = info.get("CFBundleExecutable", "")
    if bundle_id != "org.corvcad.CorvCAD" or not re.fullmatch(r"[A-Za-z0-9_.-]+", executable):
        raise ValueError("Unexpected app bundle identity")
    if package_manifest:
        verify_package_manifest(package_manifest, resources)
    output.mkdir(parents=True, exist_ok=False)
    state = {"schema": 1, "scope": "F5 diagnostic simulator rehearsal", "hardware_class": "simulator",
             "f5_accepted": False, "physical_footprint_measured": False, "status": "running", "cases": {},
             "packet_sha256": record(packet / "packet.json")["sha256"]}
    identifier, sequence, case_source, case_output = None, 0, None, None

    def command(words, *, timeout=60, required=True, env=None):
        nonlocal sequence
        sequence += 1
        result = subprocess.run(words, capture_output=True, text=True, timeout=timeout, env=env)
        save(output / f"command-{sequence:03}.json", {"command": words, "returncode": result.returncode,
                                                    "stdout": result.stdout, "stderr": result.stderr})
        if required and result.returncode:
            raise RuntimeError("Simulator command failed: " + " ".join(words[:4]))
        return result

    try:
        save(output / "runtime-resources.json", resources)
        save(output / "packet.json", manifest)
        if package_manifest:
            shutil.copyfile(package_manifest, output / "package-manifest.json")
        for index, path in enumerate(receipts):
            shutil.copyfile(path, output / f"build-receipt-{index:02}-{path.name}")
        save(output / "app-identity.json", {"bundle_id": bundle_id, "info_plist": record(app / "Info.plist"),
            "code_images": {p.name: record(p) for p in [app / executable, *sorted(app.glob("*.debug.dylib"))]},
            "runtime_files": len(resources), "runtime_bytes": sum(r["bytes"] for r in resources.values())})
        command(["xcodebuild", "-version"])
        command(["xcrun", "lipo", "-archs", str(app / executable)])
        command(["xcrun", "vtool", "-show-build", str(app / executable)])
        runtimes = json.loads(command(["xcrun", "simctl", "list", "runtimes", "-j"]).stdout)["runtimes"]
        types = json.loads(command(["xcrun", "simctl", "list", "devicetypes", "-j"]).stdout)["devicetypes"]
        runtime, device_type = select(runtimes, types)
        identifier = command(["xcrun", "simctl", "create", "CorvCAD F5 rehearsal", device_type["identifier"], runtime["identifier"]]).stdout.strip()
        if not re.fullmatch(r"[0-9A-Fa-f-]{36}", identifier):
            raise ValueError("Invalid owned simulator identity")
        state["simulator"] = {"device": identifier, "runtime": runtime, "type": device_type}
        save(output / "owned-simulator.json", state["simulator"])
        command(["xcrun", "simctl", "boot", identifier])
        command(["xcrun", "simctl", "bootstatus", identifier, "-b"], timeout=300)
        command(["xcrun", "simctl", "install", identifier, str(app)], timeout=180)
        container = Path(command(["xcrun", "simctl", "get_app_container", identifier, bundle_id, "data"]).stdout.strip())
        installed = Path(command(["xcrun", "simctl", "get_app_container", identifier, bundle_id, "app"]).stdout.strip())
        if inventory(installed / "FreeCADRuntime") != resources:
            raise ValueError("Installed simulator runtime differs from source app")
        for name, case in manifest["cases"].items():
            print("Rehearsing simulator case " + name, flush=True)
            case_output = output / name
            case_output.mkdir()
            relative = f"Gate5/{manifest['run_id']}/{name}"
            case_source = container / "Documents" / relative
            case_source.mkdir(parents=True, exist_ok=False)
            inputs = case_output / "inputs"
            inputs.mkdir()
            for filename, expected in case["files"].items():
                source = inside(packet, name + "/" + filename)
                destination = case_source / filename
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
                if record(destination) != expected:
                    raise ValueError("Seeded simulator input differs from immutable packet")
                shutil.copyfile(source, inputs / filename)
            command(["xcrun", "simctl", "terminate", identifier, bundle_id], required=False)
            environment = {key:value for key,value in os.environ.items() if not key.startswith("SIMCTL_CHILD_CORVCAD_")}
            environment["SIMCTL_CHILD_CORVCAD_MECHANICAL_REQUEST"] = relative + "/request.json"
            command(["xcrun", "simctl", "launch", identifier, bundle_id], env=environment)
            deadline, reply = time.monotonic()+300, None
            reply_path = case_source / "simulator-reply.json"
            while time.monotonic() < deadline:
                if reply_path.is_file():
                    try:
                        reply = json.loads(reply_path.read_text())
                        break
                    except json.JSONDecodeError:
                        pass  # A Data.write may still be finishing; no guessed success.
                time.sleep(0.5)
            if reply is None:
                raise TimeoutError("No actual diagnostic reply after 300 seconds")
            shutil.copyfile(reply_path, case_output / "simulator-reply.json")
            expected_input = json.loads(inside(packet, name + "/expected-input.json").read_text())
            validate_rehearsal(reply, expected_input, resources)
            copy_outputs(case_source / "outputs", case_output, reply)
            command(["xcrun", "simctl", "io", identifier, "screenshot", str(case_output / "app-window.png")], required=False)
            state["cases"][name] = {"passed": True, "process_id": reply["process_id"],
                "files": {name:record(case_output/name) for name in OUTPUTS}, "runtime_initialized_before": False}
            command(["xcrun", "simctl", "terminate", identifier, bundle_id])
        if inventory(installed / "FreeCADRuntime") != resources or inventory(app / "FreeCADRuntime") != resources:
            raise ValueError("Runtime resources changed during rehearsal")
        state["status"] = "passed; simulator diagnostics only"
    except Exception as error:
        state["status"], state["error"] = "failed", str(error)
        if identifier and re.fullmatch(r"[0-9A-Fa-f-]{36}", identifier):
            try:
                command(["xcrun", "simctl", "spawn", identifier, "log", "show", "--last", "5m",
                         "--style", "json", "--predicate", 'process == "CorvCAD"'], timeout=30, required=False)
            except Exception:
                state["simulator_log_unavailable"] = True
        if case_source and case_output:
            # Retain bounded known files from a failed attempt before deleting our simulator.
            for relative in ["simulator-reply.json", *["outputs/"+n for n in OUTPUTS | {"device-report.json", "run.started"}]]:
                source = case_source / relative
                if source.is_file() and not source.is_symlink():
                    destination = case_output / "failed" / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, destination)
        raise
    finally:
        cleanup = {}
        if identifier and re.fullmatch(r"[0-9A-Fa-f-]{36}", identifier):
            for operation in ("shutdown", "delete"):
                try:
                    cleanup[operation] = command(["xcrun", "simctl", operation, identifier], required=False).returncode
                except Exception as error:
                    cleanup[operation] = str(error)
        state["cleanup"] = cleanup
        if identifier and cleanup.get("delete") != 0:
            state["status"] = "failed"
            state["cleanup_error"] = "Owned simulator could not be deleted"
        state["artifacts"] = {p.relative_to(output).as_posix():record(p) for p in sorted(output.rglob("*")) if p.is_file()}
        save(output / "rehearsal.json", state)
    if state["status"] != "passed; simulator diagnostics only":
        raise RuntimeError("Simulator rehearsal cleanup failed; retain rehearsal.json")
    return state


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--packet", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--package-manifest", type=Path)
    parser.add_argument("--receipt", type=Path, action="append", default=[])
    args = parser.parse_args()
    result = rehearse(args.app.resolve(), args.packet.resolve(), args.output.resolve(),
                      args.package_manifest.resolve() if args.package_manifest else None, args.receipt)
    print(result["status"] + "; F5 remains Planned")

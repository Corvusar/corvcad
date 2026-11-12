#!/usr/bin/env python3
"""Exercise the real F4 C ABI and optional app in an owned simulator. MIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "ios"))
import binary_report
import run_ios
import target_config
from package import verify_package


def command(words, log, **kwargs):
    with log.open("wb") as stream:
        result = subprocess.run(words, stdout=stream, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        print(log.read_text(errors="replace")[-18000:], file=sys.stderr)
        raise RuntimeError("Native validation failed; inspect " + str(log))


def seed_project(container, name):
    project = container / "Documents/Projects" / name
    project.mkdir(parents=True, exist_ok=False)
    for extension in ("kicad_pro", "kicad_sch", "kicad_pcb"):
        source = REPO / "engine/tests/fixtures/roundtrip" / ("RoundTrip." + extension)
        shutil.copyfile(source, project / (name + "." + extension))
    return project


def validate(root, sdk, package, output, app=True, architecture="arm64"):
    target_config.target(sdk, architecture)
    verify_package(package / ("sim" if sdk == "iphonesimulator" else "device"), sdk, architecture)
    output.mkdir(parents=True, exist_ok=False)
    binary = root / ("freecad-" + sdk) / "corvcad-session/bin/corvcad_mechanical_probe"
    arch = subprocess.check_output(["xcrun", "lipo", "-archs", str(binary)], text=True).strip()
    if arch != architecture:
        raise ValueError("Native session probe architecture differs from requested target")
    build = subprocess.check_output(["xcrun", "vtool", "-show-build", str(binary)], text=True)
    links = subprocess.check_output(["otool", "-L", str(binary)], text=True)
    system = binary_report.check(build, links, sdk)
    (output / "macho-build.txt").write_text(build)
    (output / "dynamic-links.txt").write_text(links)
    report = {"schema": 1, "sdk": sdk, "architecture": arch, "system_links": system,
              "probe_bytes": binary.stat().st_size, "probe_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "scope": "F4 development validation; no physical device execution"}
    if app:
        subprocess.run(["xcodegen", "generate"], cwd=REPO / "app", check=True)
    xcode = ["xcodebuild", "-project", str(REPO / "app/CorvCAD.xcodeproj"), "-scheme", "CorvCAD",
             "-configuration", "Debug", "-derivedDataPath", str(output / "DerivedData"),
             "NATIVE_KICAD=NO", "NATIVE_FREECAD=YES", "FREECAD_PACKAGE=" + str(package),
             "CODE_SIGNING_ALLOWED=NO"]
    if sdk == "iphoneos":
        if app:
            command([*xcode, "build", "-destination", "generic/platform=iOS"], output / "app-build.log", timeout=1800)
        report["status"] = "device C ABI and app link validated; no device execution"
        verify_package(package / "device", sdk, architecture)
        (output / "f4-device.json").write_text(json.dumps(report, indent=2) + "\n")
        return report
    metadata = output / "simulator.json"
    identifier = subprocess.check_output([sys.executable, str(HERE.parent / "ios/simulator.py"), str(metadata)], text=True).strip()
    report["simulator"] = json.loads(metadata.read_text())
    if identifier != report["simulator"]["device"]:
        raise ValueError("Owned simulator identity mismatch")
    try:
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True, timeout=120)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=300)
        cases = {}
        for name in ("demo", "roundtrip"):
            case = output / name
            case.mkdir()
            fixture = HERE.parent / "fixtures" / (name + ".json")
            env = {k: v for k, v in os.environ.items() if not k.startswith("SIMCTL_CHILD_")}
            command(["xcrun", "simctl", "spawn", identifier, str(binary),
                     str(package / "sim/FreeCADRuntime"), str(case), str(fixture)],
                    case / "native-probe.log", env=env, timeout=1200)
            result = json.loads((case / "f4-session-report.json").read_text())
            if (result.get("schema") != 1 or result.get("passed") is not True or
                    result.get("native_runtime", {}).get("platform") != "ios"):
                raise ValueError("Missing actual native C ABI acceptance")
            exports = {}
            for filename in ("enclosure.FCStd", "tray.step", "lid.step", "tray.stl", "lid.stl"):
                path = case / filename
                if not path.is_file() or not path.stat().st_size:
                    raise ValueError("Missing actual native session output: " + filename)
                exports[filename] = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            cases[name] = {"report": result, "input_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(), "exports": exports}
        report["cases"] = cases
        if app:
            destination = ["-destination", "id=" + identifier]
            command([*xcode, "build-for-testing", *destination], output / "app-build.log", timeout=1800)
            bundle = output / "DerivedData/Build/Products/Debug-iphonesimulator/CorvCAD.app"
            subprocess.run(["xcrun", "simctl", "install", identifier, str(bundle)], check=True)
            container = Path(subprocess.check_output(["xcrun", "simctl", "get_app_container", identifier,
                                                      "org.corvcad.CorvCAD", "data"], text=True).strip())
            project = seed_project(container, "FreeCAD-F4")
            (project / "enclosure.corvcad.json").write_text(json.dumps({
                "margin": 3, "wall": 7, "floor": 2, "boardClearance": 2,
                "standoffHeight": 4, "standoffDiameter": 6, "lidThickness": 2,
                "lidGap": 0.25, "showLid": True, "exploded": True}))
            landscape = seed_project(container, "FreeCAD-F4-Landscape")
            shutil.copyfile(project / "enclosure.corvcad.json", landscape / "enclosure.corvcad.json")
            command([*xcode, "test-without-building", *destination,
                     "-only-testing:CorvCADUITests/NativeFreeCADTests", "-resultBundlePath", str(output / "UITests.xcresult")],
                    output / "app-tests.log", timeout=1800)
            report["app_ui_tests"] = "NativeFreeCADTests passed"
            default_xcode = ["NATIVE_FREECAD=NO" if value == "NATIVE_FREECAD=YES" else value for value in xcode]
            command([*default_xcode, "build-for-testing", *destination], output / "default-app-build.log", timeout=1800)
            subprocess.run(["xcrun", "simctl", "install", identifier, str(bundle)], check=True)
            default_container = Path(subprocess.check_output(["xcrun", "simctl", "get_app_container", identifier,
                                                              "org.corvcad.CorvCAD", "data"], text=True).strip())
            seed_project(default_container, "Demo")
            command([*default_xcode, "test-without-building", *destination,
                     "-only-testing:CorvCADUITests/EnclosureUndoTests",
                     "-resultBundlePath", str(output / "DefaultUITests.xcresult")],
                    output / "default-app-tests.log", timeout=1800)
            report["default_app_ui_tests"] = "EnclosureUndoTests passed without native FreeCAD"
        report["status"] = "native mechanical C ABI fixtures and app simulator checks passed"
        verify_package(package / "sim", sdk, architecture)
        (output / "f4-simulator.json").write_text(json.dumps(report, indent=2) + "\n")
    finally:
        failing = sys.exc_info()[0] is not None
        if not run_ios.cleanup_simulator(identifier, output) and not failing:
            raise RuntimeError("Could not delete owned simulator")
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path("build/freecad-ios"))
    p.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    p.add_argument("--package", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--no-app", action="store_true")
    p.add_argument("--architecture", choices=("arm64", "x86_64"), default="arm64")
    a = p.parse_args()
    validate(a.root.resolve(), a.sdk, a.package.resolve(), a.output.resolve(), not a.no_app, a.architecture)

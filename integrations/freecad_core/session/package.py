#!/usr/bin/env python3
"""Package a reviewed, SDK-specific F4 development adapter. MIT. No IPA/release."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ios"))
import cross
import run_ios
import target_config

CORE_ARCHIVES = {"libFreeCADBase.a", "libFreeCADApp.a", "libMaterials.a", "libPart.a", "libMesh.a",
                 "libSketcher.a", "lib_PartDesign.a", "libSpreadsheet.a", "libImport.a"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_package(folder, sdk, architecture="arm64"):
    target_config.target(sdk, architecture)
    report = json.loads((folder / "package-manifest.json").read_text())
    if report.get("schema") != 1 or report.get("sdk") != sdk:
        raise ValueError("Native package belongs to another SDK")
    if report.get("architecture", "arm64") != architecture:
        raise ValueError("Native package belongs to another architecture")
    for filename, key in (("libcorvcad_freecad.a", "core_sha256"),
                          ("libcorvcad_freecad_deps.a", "dependencies_sha256")):
        if digest(folder / filename) != report[key]:
            raise ValueError("Native package archive changed: " + filename)
    resources = folder / "FreeCADRuntime"
    actual = {p.relative_to(resources).as_posix() for p in resources.rglob("*") if p.is_file()}
    if actual != set(report["resources"]):
        raise ValueError("Native package resource set changed")
    for name, record in report["resources"].items():
        path = run_ios.stage_runtime.checked_file(resources / name, resources)
        if path.stat().st_size != record["bytes"] or digest(path) != record["sha256"]:
            raise ValueError("Native package resource changed: " + name)
    return report


def link_inputs(build):
    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json"), key=lambda p: p.stat().st_mtime).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    target = next(t for t in model["configurations"][0]["targets"] if t["name"] == "corvcad_mechanical_probe")
    target = json.loads((reply / target["jsonFile"]).read_text())
    words = shlex.split(" ".join(f["fragment"] for f in target["link"]["commandFragments"]))
    archives, forced, system = [], [], []
    i = 0
    while i < len(words):
        word = words[i]
        if words[i:i+3] == ["-Xlinker", "-force_load", "-Xlinker"]:
            if i + 3 >= len(words):
                raise ValueError("Incomplete force-load option")
            forced.append(Path(words[i+3]))
            i += 4
            continue
        if word in ("-arch", "-isysroot"):
            i += 2
            continue
        if word == "-framework":
            if i + 1 >= len(words):
                raise ValueError("Incomplete framework link option")
            system += [word, words[i + 1]]
            i += 2
            continue
        if word.startswith("-Wl,-force_load,"):
            forced.append(Path(word.split(",", 2)[2]))
        elif word.endswith(".a"):
            archives.append(Path(word))
        elif word.startswith("-l"):
            system.append(word)
        elif word.endswith((".dylib", ".so")):
            raise ValueError("Non-static dependency in FreeCAD package: " + word)
        i += 1
    resolve = lambda p: (p if p.is_absolute() else build / p).resolve()
    forced = list(dict.fromkeys(resolve(p) for p in forced))
    archives = list(dict.fromkeys(resolve(p) for p in archives))
    # The reviewed command force-loads nine FreeCAD archives, not every OCCT or
    # dependency object. Preserve this boundary when composing the app package.
    if len(forced) != 9 or {p.name for p in forced} != CORE_ARCHIVES:
        raise ValueError("Expected the nine reviewed FreeCAD force-load archives")
    for p in [*forced, *archives]:
        if not p.is_file() or p.suffix != ".a":
            raise ValueError("Missing static package input: " + str(p))
    session = [p for p in archives if p.name == "libcorvcad_freecad_session.a"]
    if len(session) != 1:
        raise ValueError("Missing or ambiguous native session archive")
    core = [session[0], *forced]
    dependencies = [p for p in archives if p not in core]
    if not dependencies:
        raise ValueError("Missing static FreeCAD dependency closure")
    system = [w for w in system if not (w.startswith("-l") and "lib" + w[2:] + ".a" in {p.name for p in dependencies})]
    return core, dependencies, system


def package(root, sdk, output, architecture="arm64"):
    cross.inputs(root, sdk, architecture)  # Recheck target inventories and shared archives.
    build = root / ("freecad-" + sdk)
    core, dependencies, system = link_inputs(build)
    runtime = root / ("runtime-" + sdk)
    manifest = run_ios.verify_runtime(runtime)
    if manifest.get("architecture", "arm64") != architecture:
        raise ValueError("Runtime belongs to another architecture")
    output.mkdir(parents=True, exist_ok=False)
    core_out, deps_out = output / "libcorvcad_freecad.a", output / "libcorvcad_freecad_deps.a"
    for destination, sources in ((core_out, core), (deps_out, dependencies)):
        subprocess.run(["xcrun", "libtool", "-static", "-o", str(destination), *map(str, sources)], check=True)
    resources = output / "FreeCADRuntime"
    records = {}
    for name, record in manifest["files"].items():
        if name.startswith("bin/"):
            continue  # The app embeds the static adapter, never a CLI executable.
        target = resources / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(runtime / name, target)
        records[name] = record
    backend = resources / "harness/session_runtime.py"
    shutil.copy2(HERE / "session_runtime.py", backend)
    host = root / "python/host/python.exe"
    subprocess.run([str(host), "-I", "-m", "compileall", "-q", "-b", "--invalidation-mode", "checked-hash",
                    "-d", "runtime/harness", str(backend)], check=True)
    for path in (backend, backend.with_suffix(".pyc")):
        records[path.relative_to(resources).as_posix()] = {
            "bytes": path.stat().st_size, "sha256": digest(path), "origin": "CorvCAD F4 session helper", "notice": "MIT"}
        path.chmod(path.stat().st_mode & ~0o222)
    rsp = output / "linker-flags.rsp"
    rsp.write_text(shlex.join([str(deps_out), *system]) + "\n")
    report = {"schema": 1, "sdk": sdk, "architecture": architecture,
              "scope": "F4 development static adapter; no app/device/release acceptance",
              "core_inputs": {str(p): digest(p) for p in core},
              "dependency_inputs": {str(p): digest(p) for p in dependencies},
              "core_sha256": digest(core_out), "dependencies_sha256": digest(deps_out),
              "system_link_options": system, "resources": records,
              "resource_bytes": sum(p["bytes"] for p in records.values()),
              "source_files": {p.name: digest(p) for p in HERE.glob("*.cpp")}}
    (output / "package-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    verify_package(output, sdk, architecture)
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path("build/freecad-ios"))
    p.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--architecture", choices=("arm64", "x86_64"), default="arm64")
    a = p.parse_args()
    package(a.root.resolve(), a.sdk, a.output.resolve(), a.architecture)

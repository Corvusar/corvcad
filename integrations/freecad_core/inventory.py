#!/usr/bin/env python3
"""Capture measured F2 build inputs, object production, resources and link closure.

This emits review material, not a release-compliance verdict or dependency lock.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shlex
import subprocess

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def declarations(path):
    with path.open("rb") as stream:
        prefix = stream.read(16000).decode("utf-8", errors="replace")
    return re.findall(r"SPDX-License-Identifier:\s*([^\r\n*]+)", prefix) or ["review-required"]


def capture(command, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    return {"command": command, "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def gui_dependency(text):
    return bool(re.search(r"Qt[56]?(?:Widgets|Gui)|FreeCADGui|(?:lib)?(?:Coin|Quarter)(?:\.|/|$)|pyside",
                          text, re.IGNORECASE))


def target_artifacts(build):
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    if not indices:
        return []
    index = json.loads(indices[-1].read_text())
    model = next((item for item in index["objects"] if item["kind"] == "codemodel"), None)
    if not model:
        return []
    records = []
    for configuration in json.loads((reply / model["jsonFile"]).read_text())["configurations"]:
        for reference in configuration["targets"]:
            target = json.loads((reply / reference["jsonFile"]).read_text())
            for artifact in target.get("artifacts", []):
                path = (build / artifact["path"]).resolve()
                if not path.is_relative_to(build):
                    raise RuntimeError(f"Target artifact escapes build directory: {path}")
                records.append({"target": target["name"], "type": target["type"],
                                "path": path.relative_to(build).as_posix(),
                                "produced": path.is_file(), "sha256": sha(path) if path.is_file() else None})
    return records


def collect(source, build, output):
    source, build, output = source.resolve(), build.resolve(), output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "status": "unconfigured", "platform": platform.platform(),
              "acceptance": "Review required; this is not a complete corresponding-source archive",
              "source_git": capture(["git", "-C", str(source), "rev-parse", "HEAD"]),
              "source_changes": capture(["git", "-C", str(source), "diff", "HEAD", "--stat"]),
              "profile_sha256": sha(HERE / "slice.json"),
              "patches": {p.name: sha(p) for p in sorted((HERE / "patches").glob("*.patch"))}}
    commands = build / "compile_commands.json"
    if not commands.exists():
        (output / "inventory.json").write_text(json.dumps(report, indent=2)+"\n")
        return report
    records = []
    for entry in json.loads(commands.read_text()):
        file = Path(entry["file"]).resolve()
        args = entry.get("arguments") or shlex.split(entry["command"])
        obj = Path(entry["directory"]) / args[args.index("-o")+1] if "-o" in args else None
        relative = file.relative_to(source).as_posix() if file.is_relative_to(source) else str(file)
        if re.search(r"/(?:Gui|GUI)/", "/"+relative) or "FreeCADGui" in relative:
            raise RuntimeError(f"GUI translation unit leaked into F2: {relative}")
        if not file.is_file() and not file.is_relative_to(build):
            raise FileNotFoundError(f"Missing source translation unit: {file}")
        records.append({"path": relative, "sha256": sha(file) if file.is_file() else None,
                        "source_produced": file.is_file(),
                        "generated": file.is_relative_to(build),
                        "object_produced": bool(obj and obj.is_file()),
                        "license_declarations": declarations(file) if file.is_file() else ["not-generated-yet"],
                        "command": args})
    report["translation_units"] = records
    report["planned_translation_units"] = len(records)
    report["produced_objects"] = sum(r["object_produced"] for r in records)
    artifacts = target_artifacts(build)
    report["target_artifacts"] = artifacts
    dependencies = capture(["ninja", "-t", "deps"], cwd=build)
    (output / "compiler-dependencies.txt").write_text(dependencies["stdout"])
    headers = set()
    for line in dependencies["stdout"].splitlines():
        if line.startswith("    "):
            path = Path(line.strip())
            if not path.is_absolute():
                path = build / path
            path = path.resolve()
            if path.is_file():
                headers.add(path)
    report["compiler_discovered_files"] = [
        {"path": p.relative_to(source).as_posix() if p.is_relative_to(source) else str(p),
         "sha256": sha(p), "license_declarations": declarations(p),
         "origin": "source" if p.is_relative_to(source) else "build" if p.is_relative_to(build) else "host"}
        for p in sorted(headers)]
    resources = []
    for directory in (build / "Mod", build / "data", build / "share"):
        if directory.exists():
            resources.extend({"path": p.relative_to(build).as_posix(), "sha256": sha(p)}
                             for p in sorted(directory.rglob("*")) if p.is_file() and p.suffix not in (".so", ".dylib"))
    report["resources"] = resources
    notices = {}
    for p in source.rglob("*"):
        if p.is_file() and not p.is_relative_to(source / ".git") and re.match(r"(?i)^(licen[cs]e|copying|copyright|notice)(\.|$)", p.name):
            relative = p.relative_to(source)
            target = output / "source-notices" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(p.read_bytes())
            notices[relative.as_posix()] = sha(p)
    report["retained_source_notices"] = notices
    binaries = [p for p in (build / "bin").glob("*") if p.name.lower() == "freecadcmd"]
    for directory in (build / "lib", build / "Mod"):
        binaries += sorted(directory.rglob("*.so")) + sorted(directory.rglob("*.dylib"))
    links = []
    for binary in binaries:
        record = capture(["otool", "-L", str(binary)] if platform.system() == "Darwin" else ["ldd", str(binary)])
        links.append(record)
        if gui_dependency(record["stdout"]):
            raise RuntimeError(f"GUI runtime dependency leaked into {binary}")
    report["runtime_link_inspections"] = links
    report["status"] = ("objects-and-runtime-recorded" if binaries and artifacts and
                        all(r["object_produced"] for r in records) and all(a["produced"] for a in artifacts)
                        else "partial-build")
    report["remaining_review"] = ["Per-file licence classification including files without SPDX",
                                    "Dependency source/binary locks and retained dependency notices",
                                    "Review OCCT visualization linkage required by the full Import App target",
                                    "Linux and macOS test acceptance; no iOS evidence"]
    (output / "inventory.json").write_text(json.dumps(report, indent=2)+"\n")
    print(f"F2 inventory: {report['produced_objects']}/{len(records)} objects; {len(headers)} compiler inputs; {len(links)} link inspections")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    report = collect(args.source.resolve(), args.build.resolve(), args.output.resolve())
    if args.require_complete and report["status"] != "objects-and-runtime-recorded":
        raise RuntimeError("Source build did not produce every inventoried object and target artifact")

#!/usr/bin/env python3
"""Retain matching Linux/macOS F2 source inventories from downloaded CI artifacts.

The result is review material, not an accepted licence/dependency audit. A fresh
output directory is required; existing evidence is never silently overwritten.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zipfile

HERE = Path(__file__).resolve().parent


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_relative(value):
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "\\" in value or ":" in value:
        raise ValueError(f"Unsafe inventory path: {value}")
    return path.as_posix()


def add_source(files, path, record, role, platform):
    path = safe_relative(path)
    current = files.setdefault(path, {"sha256": record["sha256"], "roles": [], "platforms": [],
                                      "license_declarations": record.get("license_declarations", ["review-required"])})
    if current["sha256"] != record["sha256"]:
        raise ValueError(f"Source bytes differ between platforms: {path}")
    current["roles"] = sorted(set(current["roles"] + [role]))
    current["platforms"] = sorted(set(current["platforms"] + [platform]))


def member(archive, suffix):
    names = [name for name in archive.namelist() if name.endswith(suffix)]
    if len(names) != 1:
        raise ValueError(f"Expected exactly one {suffix}, found {len(names)}")
    return archive.read(names[0])


def retain(archives, output, run_url, repository_commit):
    output.mkdir(parents=True, exist_ok=False)
    pin = json.loads((HERE/"upstream.json").read_text())["source"]["commit"]
    profile_data = (HERE/"slice.json").read_bytes()
    baseline = json.loads((HERE/"evidence/windows-upstream-baseline.json").read_text())["suites"]
    files, platforms, notices = {}, {}, {}
    for platform, path in archives.items():
        with zipfile.ZipFile(path) as archive:
            inventory_data = member(archive, "/inventory.json")
            inventory = json.loads(inventory_data)
            tests = json.loads(member(archive, "/upstream-tests.json"))
            if inventory["source_git"]["stdout"].strip() != pin or inventory["profile_sha256"] != sha(profile_data):
                raise ValueError("Artifact does not match the current pinned source/profile")
            if inventory["status"] != "objects-and-runtime-recorded" or tests["status"] != "passed":
                raise ValueError("Refusing to retain a failed/partial run as the successful desktop baseline")
            if {name: r["run"] for name, r in tests["suites"].items()} != {name: r["run"] for name, r in baseline.items()}:
                raise ValueError("Upstream suite counts differ from the measured stock baseline")
            if not inventory["target_artifacts"] or not all(a["produced"] for a in inventory["target_artifacts"]):
                raise ValueError("Missing target output in baseline")
            package_name = "host-packages.tsv" if platform == "linux" else "host-packages.json"
            package_data = member(archive, "/" + package_name)
            package_path = "host-packages/" + platform + Path(package_name).suffix
            target = output / package_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(package_data)
            source_prefix = inventory["source_git"]["command"][2]
            build_prefix = source_prefix.rsplit("/", 1)[0] + "/headless"
            generated = {}
            for record in inventory["translation_units"]:
                if record["generated"]:
                    name = safe_relative(str(PurePosixPath(record["path"]).relative_to(build_prefix)))
                    generated[name] = record["sha256"]
                else:
                    add_source(files, record["path"], record, "translation-unit", platform)
            for record in inventory["compiler_discovered_files"]:
                if record["origin"] == "source":
                    add_source(files, record["path"], record, "compiler-input", platform)
                elif record["origin"] == "build":
                    name = safe_relative(str(PurePosixPath(record["path"]).relative_to(build_prefix)))
                    generated[name] = record["sha256"]
            for name, digest in inventory["retained_source_notices"].items():
                name = safe_relative(name)
                data = member(archive, "/source-notices/" + name)
                if sha(data) != digest or (name in notices and notices[name] != digest):
                    raise ValueError(f"Notice checksum mismatch: {name}")
                notices[name] = digest
                target = output/"notices"/name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            platforms[platform] = {
                "raw_artifact_sha256": sha(path.read_bytes()), "raw_inventory_sha256": sha(inventory_data),
                "host_packages": {"path": package_path, "sha256": sha(package_data), "status": "snapshot; not a lock"},
                "platform": inventory["platform"], "planned_translation_units": inventory["planned_translation_units"],
                "produced_objects": inventory["produced_objects"], "target_artifacts": inventory["target_artifacts"],
                "generated_file_hashes": dict(sorted(generated.items())), "resources": inventory["resources"],
                "runtime_link_inspections": [{"binary": item["command"][-1].replace(build_prefix, "<build>"),
                                               "exit": item["exit"],
                                               "output": item["stdout"].replace(build_prefix, "<build>")}
                                              for item in inventory["runtime_link_inspections"]]}
    result = {"schema": 1, "status": "measured; review incomplete", "run_url": run_url,
              "repository_commit": repository_commit, "source_commit": pin, "profile_sha256": sha(profile_data),
              "retention_tool_sha256": sha(Path(__file__).read_bytes()),
              "source_files": dict(sorted(files.items())), "platforms": platforms,
              "source_tree_notices": dict(sorted(notices.items())),
              "remaining": ["Dependency locks/ownership and dependency notices",
                            "Per-file licence classification; review-required is not an approval",
                            "Generator-input graph verification beyond the read-only source verifier",
                            "Source-tree notices include components outside the selected link closure",
                            "Complete corresponding-source archive remains a separate release requirement"]}
    (output/"desktop-inputs.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8", newline="\n")
    print(f"Retained {len(files)} source inputs, {len(notices)} source-tree notices and {len(platforms)} platform records")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--macos", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory")
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--repository-commit", required=True)
    args = parser.parse_args()
    retain({"linux": args.linux, "macos": args.macos}, args.output, args.run_url, args.repository_commit)

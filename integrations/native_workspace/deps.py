#!/usr/bin/env python3
"""Build one reviewed dependency union in an isolated per-SDK prefix. MIT.

This does not build/package KiCad or FreeCAD and cannot establish combined
native acceptance. Existing accepted dependency prefixes are never modified.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "freecad_core/ios"))
import dependency_inventory
import target_config

LOCK = json.loads((HERE / "inputs.json").read_text())


def sha(path, normalize=False):
    data = path.read_bytes()
    if normalize:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def expected_manifest(freecad, kicad, additional=()):
    result = copy.deepcopy(freecad)
    result["name"] = "corvcad-native-workspace-dependencies"
    result["version-string"] = "0.1.0"
    seen = {value if isinstance(value, str) else value["name"]: value
            for value in result["dependencies"] if isinstance(value, str) or not value.get("host")}
    for port in [*kicad["vcpkg"]["ports"], *additional]:
        name = port.split("[", 1)[0]
        features = [] if "[" not in port else port.split("[", 1)[1].rstrip("]").split(",")
        if name in seen:
            original = seen[name]
            declared = set(original.get("features", [])) if isinstance(original, dict) else set()
            if not set(features) <= declared:
                raise ValueError("Dependency root requests unreviewed features: " + port)
            continue
        dependency = name if not features else {"name": name, "features": features}
        result["dependencies"].append(dependency)
        seen[name] = dependency
    return result


def verify_inputs(repository=REPOSITORY):
    for reference in LOCK["references"].values():
        path = repository / reference["path"]
        if not path.resolve().is_relative_to(repository.resolve()) or path.is_symlink():
            raise ValueError("Dependency reference escapes source checkout")
        if sha(path, normalize=True) != reference["sha256_lf"]:
            raise ValueError("Reviewed dependency input changed: " + reference["path"])
    freecad = json.loads((repository / LOCK["references"]["freecad_manifest"]["path"]).read_text())
    kicad = json.loads((repository / LOCK["references"]["kicad"]["path"]).read_text())
    mechanical_inputs = json.loads((repository / LOCK["references"]["freecad_ios_inputs"]["path"]).read_text())
    manifest = json.loads((HERE / "vcpkg.json").read_text())
    baseline = LOCK["vcpkg"]["commit"]
    if (manifest != expected_manifest(freecad, kicad, LOCK["additional_dependencies"]) or manifest["builtin-baseline"] != baseline or
            mechanical_inputs["vcpkg"]["commit"] != baseline or kicad["vcpkg"]["commit"] != baseline or
            mechanical_inputs["minimum_ios"] != LOCK["minimum_ios"]):
        raise ValueError("Union manifest, registry or minimum SDK differs from reviewed inputs")
    for triplet in target_config.TRIPLETS:
        reference = repository / LOCK["references"]["triplet_" + triplet]["path"]
        if sha(HERE / "triplets" / (triplet + ".cmake"), True) != sha(reference, True):
            raise ValueError("Union triplet differs from reviewed explicit cross-compilation settings")
    for triplet, recipe in LOCK["host_triplets"].items():
        path = HERE / "triplets" / (triplet + ".cmake")
        if path.is_symlink() or sha(path, True) != recipe["sha256_lf"]:
            raise ValueError("Reviewed host triplet changed: " + triplet)
    return {name: dict(reference) for name, reference in LOCK["references"].items()}


def plan(root, sdk, architecture="arm64", jobs=2, reference=None, clean_buildtrees_after_build=False):
    if jobs < 1:
        raise ValueError("jobs must be positive")
    references = verify_inputs()
    selected = target_config.target(sdk, architecture)
    if selected.minimum_ios != LOCK["minimum_ios"]:
        raise ValueError("Target minimum differs from union inputs")
    root = root.resolve()
    directory = root / sdk
    checkout = directory / "vcpkg"
    installed = directory / "installed"
    host = target_config.host_triplet()
    if host not in LOCK["host_triplets"]:
        raise ValueError("Host triplet has no reviewed dependency recipe: " + host)
    triplet_file = HERE / "triplets" / (selected.triplet + ".cmake")
    command = [str(checkout / "vcpkg"), "install", "--x-manifest-root=" + str(HERE),
               "--overlay-triplets=" + str(HERE / "triplets"), "--triplet", selected.triplet,
               "--host-triplet", host, "--x-install-root=" + str(installed)]
    if clean_buildtrees_after_build:
        command.append("--clean-buildtrees-after-build")
    target = {"sdk": sdk, "architecture": architecture, "triplet": selected.triplet,
              "host_triplet": host, "minimum_ios": selected.minimum_ios}
    return {"schema": 1, "target": target, "directory": str(directory), "vcpkg": str(checkout),
            "installed": str(installed), "inventory_path": str(directory / ("inventory-" + selected.triplet + ".json")),
            "macho_path": str(directory / ("inventory-" + selected.triplet + "-macho.json")),
            "receipt_path": str(directory / "native-dependencies.json"), "command": command,
            "manifest_sha256_lf": sha(HERE / "vcpkg.json", True),
            "triplet_sha256_lf": sha(triplet_file, True), "inputs_sha256_lf": sha(HERE / "inputs.json", True),
            "host_triplet_sha256_lf": LOCK["host_triplets"][host]["sha256_lf"],
            "registry_commit": LOCK["vcpkg"]["commit"], "jobs": jobs,
            "retention": {"clean_buildtrees_after_build": clean_buildtrees_after_build},
            "reference": str(reference.resolve()) if reference else None,
            "input_references": references, "source_policy": LOCK["source_policy"]}


def claim(selected):
    directory = Path(selected["directory"])
    marker = directory / "native-dependency-target.json"
    expected = {"schema": 1, "target": selected["target"], "registry_commit": selected["registry_commit"],
                "manifest_sha256_lf": selected["manifest_sha256_lf"],
                "triplet_sha256_lf": selected["triplet_sha256_lf"],
                "host_triplet_sha256_lf": selected["host_triplet_sha256_lf"],
                "inputs_sha256_lf": selected["inputs_sha256_lf"]}
    if marker.exists():
        if json.loads(marker.read_text()) != expected:
            raise ValueError("SDK dependency root belongs to different reviewed inputs or architecture; use a new root")
        return
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("Unowned dependency directory; preserve it and use a new root")
    directory.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps(expected, indent=2) + "\n")


def run(command, cwd=None, env=None):
    print("Running:", *map(str, command), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


def checkout(selected):
    directory = Path(selected["vcpkg"])
    if not directory.exists():
        # Full version history is needed by the Eigen3 override's port tree.
        command = ["git", "clone", "--quiet"]
        if selected.get("reference"):
            # Borrow Git objects read-only, then dissociate so pruning the
            # old cache cannot invalidate this SDK's registry checkout.
            command += ["--reference-if-able", selected["reference"], "--dissociate"]
        run([*command, LOCK["vcpkg"]["repository"], directory])
    status = subprocess.check_output(["git", "-C", str(directory), "status", "--porcelain", "--untracked-files=all"], text=True)
    if status.strip():
        raise ValueError("vcpkg has local edits; preserve it and use a new root")
    head = subprocess.check_output(["git", "-C", str(directory), "rev-parse", "HEAD"], text=True).strip()
    if head != LOCK["vcpkg"]["commit"]:
        run(["git", "-C", directory, "checkout", "--quiet", "--detach", LOCK["vcpkg"]["commit"]])
    actual = subprocess.check_output(["git", "-C", str(directory), "rev-parse", "HEAD"], text=True).strip()
    if actual != LOCK["vcpkg"]["commit"]:
        raise ValueError("vcpkg source commit differs from the reviewed registry")
    executable = directory / "vcpkg"
    if not executable.is_file():
        run([directory / "bootstrap-vcpkg.sh", "-disableMetrics"], directory)
    return executable


def build(selected):
    if platform.system() != "Darwin":
        raise ValueError("Native iOS dependency builds require macOS and Xcode")
    claim(selected)
    executable = checkout(selected)
    directory = Path(selected["directory"])
    # vcpkg itself verifies binary-cache ABI identities. Never manually graft
    # archives/headers from accepted FreeCAD or KiCad prefixes into this root.
    env = {**os.environ, "VCPKG_MAX_CONCURRENCY": str(selected["jobs"]),
           "CMAKE_BUILD_PARALLEL_LEVEL": str(selected["jobs"])}
    run(selected["command"], directory, env)
    triplet = selected["target"]["triplet"]
    inventory = dependency_inventory.capture(Path(selected["installed"]), triplet)
    macho = dependency_inventory.inspect_target(Path(selected["installed"]), triplet, inventory)
    Path(selected["inventory_path"]).write_text(json.dumps(inventory, indent=2) + "\n")
    Path(selected["macho_path"]).write_text(json.dumps(macho, indent=2) + "\n")
    sdk = selected["target"]["sdk"]
    toolchain = {"vcpkg_commit": selected["registry_commit"], "vcpkg_executable_sha256": sha(executable),
                 "manifest_sha256_lf": selected["manifest_sha256_lf"], "triplet_sha256_lf": selected["triplet_sha256_lf"],
                 "host_triplet_sha256_lf": selected["host_triplet_sha256_lf"],
                 "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
                 "sdk_version": subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-version"], text=True).strip(),
                 "compiler": subprocess.check_output(["xcrun", "--sdk", sdk, "clang++", "--version"], text=True).strip()}
    receipt = {"schema": 1, "status": "dependency inputs built and target archives verified",
               "scope": "Candidate shared dependency prefix only; no combined native acceptance",
               "target": selected["target"], "toolchain": toolchain, "input_references": selected["input_references"],
               "retention": selected["retention"],
               "source_policy": selected["source_policy"], "inventory": inventory, "macho": macho,
               "installed": selected["installed"],
               "paths": {"vcpkg": selected["vcpkg"], "installed": selected["installed"],
                         "prefix": str(Path(selected["installed"]) / triplet),
                         "inventory": selected["inventory_path"], "macho": selected["macho_path"]},
               "inventory_sha256": sha(Path(selected["inventory_path"])),
               "macho_sha256": sha(Path(selected["macho_path"]))}
    Path(selected["receipt_path"]).write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("build/native-workspace/deps"))
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, default="arm64")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--plan", action="store_true", help="Verify source pins and print paths without changing caches")
    parser.add_argument("--reference", type=Path, help="Read-only Git object reference; new checkout dissociates before use")
    parser.add_argument("--clean-buildtrees-after-build", action="store_true",
                        help="Remove successful port buildtree subdirectories; retain top-level logs/ABI records, packages and downloads")
    args = parser.parse_args()
    selected = plan(args.root, args.sdk, args.architecture, args.jobs, args.reference, args.clean_buildtrees_after_build)
    if args.plan:
        print(json.dumps(selected, indent=2))
    else:
        build(selected)

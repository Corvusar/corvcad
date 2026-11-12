#!/usr/bin/env python3
"""Retain candidate desktop closure locks, licence evidence and dependency notices."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from retain_inventory import member, safe_relative
from verify_closure import signature

HERE = Path(__file__).resolve().parent


def retain(archives, output, run_url, commit):
    output.mkdir(parents=True, exist_ok=False)
    result = {"schema": 1, "status": "candidate; review required", "run_url": run_url,
              "repository_commit": commit,
              "policy_sha256": hashlib.sha256((HERE/"license-policy.json").read_bytes()).hexdigest(),
              "scope": "Version/content locks for actual desktop build inputs. No automatic package downgrade; unavailable versions fail reproduction. Complete release source archives and iOS dependencies are separate.",
              "platforms": {}}
    for platform, path in archives.items():
        with zipfile.ZipFile(path) as archive:
            closure = json.loads(member(archive, "/closure.json"))
            pin = json.loads((HERE/"upstream.json").read_text())["source"]["commit"]
            profile = hashlib.sha256((HERE/"slice.json").read_bytes()).hexdigest()
            if closure["source_commit"] != pin or closure["profile_sha256"] != profile:
                raise ValueError("Capture source/profile does not match this integration")
            if closure["status"] != "captured" or closure["unowned"]:
                raise ValueError("Cannot lock unowned inputs")
            inventory = json.loads(member(archive, "/inventory.json"))
            if inventory["status"] != "objects-and-runtime-recorded":
                raise ValueError("Incomplete source build")
            compiled = {r["path"]: r for r in inventory["compiler_discovered_files"] if r["origin"] == "source"}
            compiled.update({r["path"]: r for r in inventory["translation_units"] if not r["generated"]})
            if not compiled.keys() <= closure["source_files"].keys():
                raise ValueError("Generator closure omits measured compiled source inputs")
            for name, record in compiled.items():
                if record["sha256"] != closure["source_files"][name]["sha256"]:
                    raise ValueError(f"Compiled input changed: {name}")
            tests = json.loads(member(archive, "/upstream-tests.json"))
            if tests["status"] != "passed":
                raise ValueError("Upstream suites did not pass")
            for fixture in ("demo", "roundtrip"):
                if json.loads(member(archive, "/"+fixture+"-proof/report.json"))["status"] != "passed":
                    raise ValueError("F1 model proof did not pass")
            catalog_data = member(archive, "/license-catalog.json")
            catalog = json.loads(catalog_data)
            if catalog["policy_sha256"] != result["policy_sha256"]:
                raise ValueError("Catalogue uses a different licence policy")
            if set(catalog["files"]) != set(closure["source_files"]):
                raise ValueError("Incomplete source licence catalogue")
            retained = {}

            def save(name, data):
                name = safe_relative(name)
                target = output/name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                retained[name] = hashlib.sha256(data).hexdigest()

            catalog_name = "closure/"+platform+"-licenses.json"
            save(catalog_name, catalog_data)
            for filename, digest_key in (("generator-graph.dot", "generator_graph_sha256"), ("build-commands.txt", "build_commands_sha256")):
                data = member(archive, "/"+filename)
                if hashlib.sha256(data).hexdigest() != closure[digest_key]:
                    raise ValueError("Generator graph/command checksum mismatch")
                save("closure/"+platform+"-"+filename, data)
            for package in closure["packages"].values():
                notices = package["notices"] + [package[k] for k in ("receipt", "installed_recipe") if k in package]
                for notice in notices:
                    data = member(archive, "/dependency-notices/"+safe_relative(notice["path"]))
                    if hashlib.sha256(data).hexdigest() != notice["sha256"]:
                        raise ValueError("Dependency notice/recipe checksum mismatch")
                    save("dependencies/"+platform+"/"+notice["path"], data)
            result["platforms"][platform] = {"signature": signature(closure), "license_catalog": catalog_name,
                                              "retained_files": retained, "artifact_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    # Baseline/current CMake dependency records enumerate different header
    # inputs on Linux. Preserve the original two-platform union without
    # pretending that every platform's current graph has identical inputs.
    original = json.loads((HERE/"audit/desktop-inputs.json").read_text())["source_files"]
    combined = {name: record for platform in result["platforms"].values()
                for name, record in platform["signature"]["source_files"].items()}
    if not original.keys() <= combined.keys() or any(combined[n]["sha256"] != r["sha256"] for n, r in original.items()):
        raise ValueError("Combined closure changed or omitted an original compiled input")
    (output/"dependency-lock.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8", newline="\n")
    print("Retained candidate desktop dependency locks and generator/licence evidence")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--macos", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--repository-commit", required=True)
    args = parser.parse_args()
    retain({"linux": args.linux, "macos": args.macos}, args.output, args.run_url, args.repository_commit)

#!/usr/bin/env python3
"""Prepare/configure the isolated F2 headless source-build investigation.

CorvCAD code, MIT. Source adaptations are separately licensed in patches/README.md.
Never resets a source checkout. All patches and resulting edited files are hashed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import fetch

HERE = Path(__file__).resolve().parent
PROFILE = json.loads((HERE / "slice.json").read_text())


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def digest(path, lf=False):
    data = path.read_text(encoding="utf-8").encode() if lf else path.read_bytes()
    return hashlib.sha256(data).hexdigest()


def prepare(source):
    retained = PROFILE.get("retained_license")
    if retained and digest(HERE / retained["path"], True) != retained["sha256_lf"]:
        raise RuntimeError("Retained FreeCAD licence hash mismatch")
    if not source.exists():
        if source.name != "FreeCAD":
            raise ValueError("New source checkout must be named FreeCAD")
        source.parent.mkdir(parents=True, exist_ok=True)
        fetch.source(source.parent)
    if git(source, "rev-parse", "HEAD") != fetch.LOCK["source"]["commit"]:
        raise RuntimeError("F2 source is not at the reviewed pin")
    expected = {}
    for patch in PROFILE["patches"]:
        if digest(HERE / patch["path"]) != patch["sha256"]:
            raise RuntimeError("Patch hash mismatch")
        expected.update(patch["files"])
    changed = set(git(source, "diff", "HEAD", "--name-only").splitlines())
    untracked = git(source, "ls-files", "--others", "--exclude-standard")
    if untracked or changed - expected.keys():
        raise RuntimeError("Undeclared source edits; preserve this checkout and use another directory")
    if changed:
        if changed != expected.keys() or any(digest(source / p, True) != v["after_sha256_lf"] for p, v in expected.items()):
            raise RuntimeError("Partial/modified source patch state; preserve it and use another directory")
    else:
        if any(digest(source / p, True) != v["before_sha256_lf"] for p, v in expected.items()):
            raise RuntimeError("Unexpected source file before patching")
        for patch in PROFILE["patches"]:
            subprocess.run(["git", "-C", str(source), "apply", "--check", str(HERE / patch["path"])], check=True)
            subprocess.run(["git", "-C", str(source), "apply", str(HERE / patch["path"])], check=True)
    if any(digest(source / p, True) != v["after_sha256_lf"] for p, v in expected.items()):
        raise RuntimeError("Patched source content does not match declared hashes")
    print("Verified F2 pin and complete patch state")


def options(source):
    text = (source / "cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake").read_text()
    # Disable every upstream module, including future defaults, then enable the
    # reviewed list. Runtime/helper BUILD_* options are supplied explicitly below.
    names = set(re.findall(r'option\((BUILD_[A-Z_]+)\s+"Build the FreeCAD', text))
    enabled = set(PROFILE["enabled_modules"])
    if not enabled <= names:
        raise RuntimeError(f"Unknown module flags: {enabled - names}")
    flags = [f"-D{name}={'ON' if name in enabled else 'OFF'}" for name in sorted(names)]
    flags += ["-DBUILD_GUI=OFF", "-DENABLE_DEVELOPER_TESTS=OFF", "-DBUILD_FEM_NETGEN=OFF",
              "-DBUILD_DESIGNER_PLUGIN=OFF", "-DBUILD_VR=OFF", "-DBUILD_CLOUD=OFF",
              "-DFREECAD_QT_VERSION=6", "-DFREECAD_CREATE_MAC_APP=OFF", "-DFREECAD_USE_PCH=OFF",
              "-DFREECAD_USE_CCACHE=ON", "-DFREECAD_CHECK_PIVY=OFF", "-DFREECAD_USE_3DCONNEXION_LEGACY=OFF",
              "-DFREECAD_3DCONNEXION_SUPPORT=None", "-DFREECAD_USE_EXTERNAL_FMT=ON",
              "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG",
              "-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    return flags


def configure(source, build, extra):
    prepare(source)
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    for name in ("codemodel-v2", "cache-v2", "toolchains-v1", "cmakeFiles-v1"):
        (query / name).touch()
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", *options(source), *extra]
    (build / "configure-command.json").write_text(json.dumps(command, indent=2)+"\n")
    subprocess.run(command, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "configure"))
    parser.add_argument("--source", type=Path, default=Path("build/freecad-core/FreeCAD"))
    parser.add_argument("--build", type=Path, default=Path("build/freecad-core/headless"))
    args, extra = parser.parse_known_args()
    if args.action == "prepare":
        if extra:
            parser.error("Unexpected arguments")
        prepare(args.source.resolve())
    else:
        configure(args.source.resolve(), args.build.resolve(), extra)

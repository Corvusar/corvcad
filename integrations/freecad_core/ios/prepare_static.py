#!/usr/bin/env python3
"""Prepare a separately hashed static FreeCAD investigation; never reset sources.

MIT. The source patch is governed by the notices in patches/README.md.
This profile is not the accepted F2 desktop profile or an accepted iOS build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import slice as desktop

PROFILE = json.loads((HERE / "static-profile.json").read_text())


def digest(path, lf=True):
    data = path.read_text(encoding="utf-8").encode() if lf else path.read_bytes()
    return hashlib.sha256(data).hexdigest()


def declarations():
    if digest(HERE.parent / "slice.json") != PROFILE["desktop_profile_sha256_lf"]:
        raise ValueError("Desktop base profile changed without static-profile review")
    if digest(HERE / "static_registry.cpp") != PROFILE["registry_sha256_lf"]:
        raise ValueError("Static module registry changed without profile review")
    result = {}
    for patch in desktop.PROFILE["patches"]:
        if digest(HERE.parent / patch["path"], False) != patch["sha256"]:
            raise ValueError("Desktop patch checksum mismatch")
        result.update({name: record["after_sha256_lf"] for name, record in patch["files"].items()})
    patch = PROFILE["patch"]
    if digest(HERE / patch["path"], False) != patch["sha256"]:
        raise ValueError("Static patch checksum mismatch")
    if result.keys() & patch["files"].keys():
        raise ValueError("Overlapping patch profiles require an explicit reviewed transition")
    result.update({name: record["after_sha256_lf"] for name, record in patch["files"].items()})
    return result


def verify(source):
    expected = declarations()
    if desktop.git(source, "rev-parse", "HEAD") != desktop.fetch.LOCK["source"]["commit"]:
        raise ValueError("Static source checkout is not at the reviewed FreeCAD pin")
    if desktop.git(source, "ls-files", "--others", "--exclude-standard"):
        raise ValueError("Untracked source inputs; preserve this checkout")
    actual = set(desktop.git(source, "diff", "HEAD", "--name-only").splitlines())
    if actual != expected.keys():
        raise ValueError("Incomplete or undeclared static source edits; preserve this checkout")
    for name, checksum in expected.items():
        if digest(source / name) != checksum:
            raise ValueError("Modified static source input: " + name)
    retained = desktop.PROFILE["retained_license"]
    if digest(HERE.parent / retained["path"]) != retained["sha256_lf"]:
        raise ValueError("Retained FreeCAD licence changed")
    return {"schema": 1, "source_commit": desktop.fetch.LOCK["source"]["commit"],
            "profile_sha256_lf": digest(HERE / "static-profile.json"),
            "source_files": expected, "status": "source patch state verified; native build not established"}


def prepare(source):
    declarations()
    edited = PROFILE["patch"]["files"]
    if source.exists() and any(digest(source / name) == record["after_sha256_lf"] for name, record in edited.items()):
        # A partial or edited application fails verification. Never patch over it.
        return verify(source)
    desktop.prepare(source)
    for name, record in edited.items():
        if digest(source / name) != record["before_sha256_lf"]:
            raise ValueError("Unexpected input before static patch: " + name)
    patch = HERE / PROFILE["patch"]["path"]
    # Several upstream files store CRLF in Git itself. Full before/after hashes
    # above/below guard content; ignore context whitespace only during apply.
    subprocess.run(["git", "-C", str(source), "apply", "--check", "--ignore-space-change", str(patch)], check=True)
    subprocess.run(["git", "-C", str(source), "apply", "--ignore-space-change", str(patch)], check=True)
    return verify(source)


def configure(source, build, extra):
    if platform.system() != "Darwin":
        raise ValueError("Static native configuration requires macOS and Xcode")
    report = prepare(source)
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    for name in ("codemodel-v2", "cache-v2", "toolchains-v1", "cmakeFiles-v1"):
        (query / name).touch()
    flags = desktop.options(source) + ["-DFREECAD_STATIC_MODULES=ON",
             "-DCORVCAD_STATIC_REGISTRY=" + str(HERE / "static_registry.cpp")]
    flags += ["-D" + name + "=OFF" for name in PROFILE["disabled_additional_modules"]]
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", *flags, *extra]
    (build / "static-source-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    (build / "configure-command.json").write_text(json.dumps(command, indent=2) + "\n")
    subprocess.run(command, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify", "configure"))
    parser.add_argument("--source", type=Path, default=Path("build/freecad-ios/FreeCAD"))
    parser.add_argument("--build", type=Path, default=Path("build/freecad-ios/static-desktop"))
    args, extra = parser.parse_known_args()
    source = args.source.resolve()
    if args.action == "configure":
        configure(source, args.build.resolve(), extra)
    else:
        if extra:
            parser.error("Unexpected arguments")
        report = prepare(source) if args.action == "prepare" else verify(source)
        print(json.dumps(report, indent=2))

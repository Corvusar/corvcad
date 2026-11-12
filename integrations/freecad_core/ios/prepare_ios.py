#!/usr/bin/env python3
"""Apply a pinned iOS adaptation after the separately verified static profile.

MIT. Upstream changes in patches/0002-ios-runtime.patch retain their LGPL
notices. Never reset or overwrite an unreviewed, partial or modified checkout.
"""
import argparse
import json
from pathlib import Path
import platform
import subprocess

import prepare_static as static

HERE = Path(__file__).resolve().parent
PROFILE = json.loads((HERE / "ios-profile.json").read_text())


def declarations():
    baseline = static.declarations()
    if static.digest(HERE / "static-profile.json") != PROFILE["static_profile_sha256_lf"]:
        raise ValueError("Static base profile changed without iOS profile review")
    patch = PROFILE["patch"]
    if static.digest(HERE / patch["path"], False) != patch["sha256"]:
        raise ValueError("iOS patch checksum mismatch")
    for name in baseline.keys() & patch["files"].keys():
        if baseline[name] != patch["files"][name]["before_sha256_lf"]:
            raise ValueError("iOS patch does not continue the reviewed static file: " + name)
    return {**baseline, **{name: item["after_sha256_lf"] for name, item in patch["files"].items()}}


def verify(source):
    expected = declarations()
    git = static.desktop.git
    if git(source, "rev-parse", "HEAD") != static.desktop.fetch.LOCK["source"]["commit"]:
        raise ValueError("iOS source checkout differs from the reviewed FreeCAD pin")
    if git(source, "ls-files", "--others", "--exclude-standard"):
        raise ValueError("Untracked iOS source inputs; preserve this checkout")
    if set(git(source, "diff", "HEAD", "--name-only").splitlines()) != expected.keys():
        raise ValueError("Incomplete or undeclared iOS source edits; preserve this checkout")
    for name, checksum in expected.items():
        if static.digest(source / name) != checksum:
            raise ValueError("Modified iOS source input: " + name)
    retained = static.desktop.PROFILE["retained_license"]
    if static.digest(HERE.parent / retained["path"]) != retained["sha256_lf"]:
        raise ValueError("Retained FreeCAD licence changed")
    return {"schema": 1, "source_commit": static.desktop.fetch.LOCK["source"]["commit"],
            "profile_sha256_lf": static.digest(HERE / "ios-profile.json"),
            "static_profile_sha256_lf": PROFILE["static_profile_sha256_lf"],
            "source_files": expected,
            "status": "iOS source patch state verified; native validation not established"}


def prepare(source):
    declarations()
    files = PROFILE["patch"]["files"]
    if source.exists() and any(static.digest(source / name) == item["after_sha256_lf"]
                               for name, item in files.items()):
        return verify(source)
    static.prepare(source)
    for name, item in files.items():
        if static.digest(source / name) != item["before_sha256_lf"]:
            raise ValueError("Unexpected source input before iOS patch: " + name)
    patch = HERE / PROFILE["patch"]["path"]
    subprocess.run(["git", "-C", str(source), "apply", "--check", "--ignore-space-change", str(patch)], check=True)
    subprocess.run(["git", "-C", str(source), "apply", "--ignore-space-change", str(patch)], check=True)
    return verify(source)


def configure(source, build, extra):
    if platform.system() != "Darwin":
        raise ValueError("iOS native configuration requires macOS and Xcode")
    if "-DCMAKE_SYSTEM_NAME=iOS" not in extra:
        raise ValueError("iOS preparation requires an explicit iOS target system")
    report = prepare(source)
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    for name in ("codemodel-v2", "cache-v2", "toolchains-v1", "cmakeFiles-v1"):
        (query / name).touch()
    flags = static.desktop.options(source) + ["-DFREECAD_STATIC_MODULES=ON",
        "-DCORVCAD_STATIC_REGISTRY=" + str(HERE / "static_registry.cpp")]
    flags += ["-D" + name + "=OFF" for name in static.PROFILE["disabled_additional_modules"]]
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", *flags, *extra]
    (build / "ios-source-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    (build / "configure-command.json").write_text(json.dumps(command, indent=2) + "\n")
    subprocess.run(command, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify", "configure"))
    parser.add_argument("--source", type=Path, default=Path("build/freecad-ios/source-ios/FreeCAD"))
    parser.add_argument("--build", type=Path, default=Path("build/freecad-ios/freecad-iphonesimulator"))
    args, extra = parser.parse_known_args()
    if args.action == "configure":
        configure(args.source.resolve(), args.build.resolve(), extra)
    else:
        if extra:
            parser.error("Unexpected arguments")
        print(json.dumps((prepare if args.action == "prepare" else verify)(args.source.resolve()), indent=2))

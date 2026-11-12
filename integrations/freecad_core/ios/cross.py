#!/usr/bin/env python3
"""Configure/build the experimental static FreeCAD slice for one iOS SDK.

MIT. This does not stage an app or establish simulator geometry acceptance.
Host code generators and target headers/libraries have separate explicit roots.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess

import prepare_ios
import build_occt
import dependency_inventory
import target_config

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / "inputs.json").read_text())


def require(path):
    if not path.is_file():
        raise ValueError("Missing cross-build input: " + str(path))
    return path


def verify_python_dependencies(metadata, prefix):
    expected = {"libffi.a", "libz.a", "libbz2.a"}
    system_expat = metadata.get("system_expat", False)
    if not isinstance(system_expat, bool):
        raise ValueError("Unknown CPython Expat provider")
    if system_expat:
        expected.add("libexpat.a")
    recorded = metadata["dependency_archives"]
    if set(recorded) != expected:
        raise ValueError("CPython dependency inventory differs from its declared providers")
    for name in sorted(expected):
        if hashlib.sha256(require(prefix / "lib" / name).read_bytes()).hexdigest() != recorded[name]:
            raise ValueError("CPython dependency differs from target inventory: " + name)
    return prefix / "lib/libexpat.a" if system_expat else None


def inputs(root, sdk, architecture="arm64"):
    selected = target_config.target(sdk, architecture)
    triplet = selected.triplet
    python = root / "python" / sdk
    metadata = json.loads(require(python / "python-build.json").read_text())
    if (metadata["sdk"] != sdk or metadata["source"] != LOCK["python"] or
            metadata.get("target") != selected.compiler_target or
            metadata.get("architecture", "arm64") != architecture):
        raise ValueError("CPython SDK or source pin differs from cross-build inputs")
    deps = root / "deps/installed"
    prefix, host = deps / triplet, deps / target_config.host_triplet()
    paths = {
        "python_library": require(python / "stage/lib/libpython3.13.a"),
        "python_header": require(python / "stage/include/python3.13/Python.h"),
        "host_python": require(root / "python/host/python.exe"),
        "toolchain": require(root / "deps/vcpkg/scripts/buildsystems/vcpkg.cmake"),
        "qt": require(prefix / "share/Qt6/Qt6Config.cmake"),
        "linguist": require(host / "share/Qt6LinguistTools/Qt6LinguistToolsConfig.cmake"),
    }
    # OCCT is built separately: vcpkg's OCCT 8 is outside this FreeCAD profile.
    occt = root / "occt" / sdk / "stage"
    paths["occt"] = require(occt / "lib/cmake/opencascade/OpenCASCADEConfig.cmake")
    occt_report = json.loads(require(root / "occt" / sdk / "occt-build.json").read_text())
    if (occt_report["sdk"] != sdk or occt_report["source"] != build_occt.PIN or
            occt_report.get("architecture") != architecture or
            occt_report["minimum_ios"] != LOCK["minimum_ios"]):
        raise ValueError("OCCT SDK or source pin differs from cross-build inputs")
    for relative, record in occt_report["archives"].items():
        if build_occt.digest(require(occt / relative)) != record["sha256"]:
            raise ValueError("OCCT archive changed after inventory: " + relative)
    inventory = json.loads(require(root / "deps" / ("inventory-" + triplet + ".json")).read_text())
    if inventory["triplet"] != triplet or inventory != dependency_inventory.capture(deps, triplet):
        raise ValueError("Dependency inventory belongs to another target")
    target = json.loads(require(root / "deps" / ("inventory-" + triplet + "-macho.json")).read_text())
    if (target["sdk"] != sdk or target.get("architecture") != architecture or target["minimum_ios"] != LOCK["minimum_ios"] or
            {name: record["sha256"] for name, record in target["archives"].items()} !=
            {name: record["sha256"] for name, record in inventory["archives"].items()}):
        raise ValueError("Target archive SDK verification differs from dependency inventory")
    if hashlib.sha256(paths["python_library"].read_bytes()).hexdigest() != metadata["archive_sha256"]:
        raise ValueError("CPython archive changed after inventory")
    paths["ffi"] = require(prefix / "lib/libffi.a")
    if hashlib.sha256(paths["ffi"].read_bytes()).hexdigest() != metadata["libffi_sha256"]:
        raise ValueError("CPython was not built against this target's libffi")
    expat = verify_python_dependencies(metadata, prefix)
    if expat:
        paths["expat"] = expat
    return triplet, prefix, host, occt, paths


def flags(root, sdk, architecture="arm64"):
    target_config.target(sdk, architecture)
    triplet, prefix, host, occt, paths = inputs(root, sdk, architecture)
    toolkit_report = json.loads((root / "occt" / sdk / "occt-build.json").read_text())
    result = [
        "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_ARCHITECTURES=" + architecture,
        "-DCMAKE_OSX_SYSROOT=" + sdk,
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + LOCK["minimum_ios"],
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(paths["toolchain"]),
        "-DVCPKG_TARGET_TRIPLET=" + triplet,
        "-DVCPKG_HOST_TRIPLET=" + target_config.host_triplet(),
        "-DVCPKG_OVERLAY_TRIPLETS=" + str(HERE / "triplets"),
        "-DVCPKG_INSTALLED_DIR=" + str(root / "deps/installed"),
        "-DVCPKG_MANIFEST_INSTALL=OFF",
        "-DCMAKE_PREFIX_PATH=" + str(occt) + ";" + str(prefix),
        "-DCMAKE_FIND_ROOT_PATH=" + str(occt) + ";" + str(prefix),
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
        "-DCMAKE_FIND_FRAMEWORK=NEVER",
        "-DQt6_DIR=" + str(paths["qt"].parent),
        "-DQT_HOST_PATH=" + str(host),
        "-DQt6LinguistTools_DIR=" + str(paths["linguist"].parent),
        "-DOpenCASCADE_DIR=" + str(paths["occt"].parent),
        "-DCORVCAD_IOS_OCCT_LIBRARIES=" + ";".join(sorted(toolkit_report["toolkit_dependencies"])),
        "-DPython3_EXECUTABLE=" + str(paths["host_python"]),
        "-DPython3_LIBRARY=" + str(paths["python_library"]),
        "-DPython3_INCLUDE_DIR=" + str(paths["python_header"].parent),
        "-DPython3_FIND_FRAMEWORK=NEVER", "-DPython3_USE_STATIC_LIBS=ON",
        "-DCORVCAD_IOS_FFI_LIBRARY=" + str(paths["ffi"]),
        "-DCORVCAD_IOS_BZIP2_LIBRARY=" + str(prefix / "lib/libbz2.a"),
        # The reviewed iOS SetupPython patch discovers target Development.Embed
        # separately, then restores this same-version host code generator.
        "-DCMAKE_POLICY_DEFAULT_CMP0190=NEW",
        "-DBUILD_DYNAMIC_LINK_PYTHON=OFF", "-DCMAKE_MACOSX_BUNDLE=OFF",
        "-DINSTALL_TO_SITEPACKAGES=OFF", "-DCMAKE_INSTALL_BINDIR=bin",
        "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_DATADIR=data",
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
    ]
    if "expat" in paths:
        result.append("-DCORVCAD_IOS_EXPAT_LIBRARY=" + str(paths["expat"]))
    return result


def inspect_commands(build, sdk, host, architecture="arm64"):
    """Fail on macOS dependency leakage in target C/C++ compilation."""
    target_config.target(sdk, architecture)
    commands = json.loads((build / "compile_commands.json").read_text())
    if not commands:
        raise ValueError("No target compilation commands")
    for record in commands:
        words = record.get("arguments") or shlex.split(record["command"])
        text = " ".join(words)
        architectures = [words[i+1] for i, word in enumerate(words[:-1]) if word == "-arch"]
        if architectures != [architecture] or "-isysroot " not in text:
            raise ValueError("Missing explicit " + architecture + " SDK compilation: " + record["file"])
        sysroot = words[words.index("-isysroot") + 1].lower()
        expected = "iphonesimulator" if sdk == "iphonesimulator" else "iphoneos"
        if expected not in sysroot or "macosx" in sysroot:
            raise ValueError("Wrong target SDK: " + sysroot)
        for word in words:
            if any(marker in word.replace("\\", "/") for marker in ("/opt/homebrew/include", "/opt/homebrew/opt/", "/usr/local/include", "/usr/local/opt/", "/usr/local/lib/", host.as_posix())):
                raise ValueError("Host dependency in target compilation: " + word)
    return {"schema": 1, "sdk": sdk, "architecture": architecture, "compilation_commands_checked": len(commands),
            "status": "target compiler flags checked; link/runtime acceptance is separate"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("build/freecad-ios"))
    parser.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 3))
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, default="arm64")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("iOS cross-compilation requires macOS and Xcode")
    root = args.root.resolve()
    build = root / ("freecad-" + args.sdk)
    selected = target_config.target(args.sdk, args.architecture)
    target_config.claim(build, selected, "cross-compile-check.json")
    prepare_ios.configure(root / "source-ios/FreeCAD", build, flags(root, args.sdk, args.architecture))
    report = inspect_commands(build, args.sdk, root / "deps/installed" / target_config.host_triplet(), args.architecture)
    (build / "cross-compile-check.json").write_text(json.dumps(report, indent=2) + "\n")
    if not args.configure_only:
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)


if __name__ == "__main__":
    main()

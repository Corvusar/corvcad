#!/usr/bin/env python3
"""Build the optional F4 adapter against the unchanged reviewed F3 source. MIT."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ios"))
import cross
import prepare_ios
import target_config


def build_runtime(folder, jobs):
    # Module Init.py files and material/hole resources belong to upstream ALL
    # copy targets, rather than executable dependencies. Build the complete
    # reviewed headless graph before the strict runtime stager reads it.
    subprocess.run(["cmake", "--build", str(folder), "--parallel", str(jobs)], check=True)


def build(root, sdk, jobs=3, architecture="arm64"):
    if platform.system() != "Darwin":
        raise ValueError("Native F4 cross-build requires macOS and Xcode")
    folder = root / ("freecad-" + sdk)
    flags = cross.flags(root, sdk, architecture) + ["-DCMAKE_PROJECT_FreeCAD_INCLUDE=" + str(HERE / "hook.cmake")]
    prepare_ios.configure(root / "source-ios/FreeCAD", folder, flags)
    report = cross.inspect_commands(folder, sdk, root / "deps/installed" / target_config.host_triplet(), architecture)
    (folder / "f4-cross-compile-check.json").write_text(json.dumps(report, indent=2) + "\n")
    build_runtime(folder, jobs)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path("build/freecad-ios"))
    p.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    p.add_argument("--jobs", type=int, default=3)
    p.add_argument("--architecture", choices=("arm64", "x86_64"), default="arm64")
    a = p.parse_args()
    build(a.root.resolve(), a.sdk, a.jobs, a.architecture)

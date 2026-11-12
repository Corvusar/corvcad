"""Explicit iOS target and native macOS host selection. MIT.

arm64 remains the default. Intel simulator builds are opt-in and must use a
separate build root; archives are never interchangeable merely by SDK name.
"""
from dataclasses import dataclass
import json
from pathlib import Path
import platform
import sys

ARCHITECTURES = ("arm64", "x86_64")
TRIPLETS = ("arm64-ios", "arm64-ios-simulator", "x64-ios-simulator")
MINIMUM = json.loads(Path(__file__).with_name("inputs.json").read_text())["minimum_ios"]


@dataclass(frozen=True)
class Target:
    sdk: str
    architecture: str
    triplet: str
    compiler_target: str
    minimum_ios: str


def target(sdk, architecture="arm64"):
    if sdk not in ("iphoneos", "iphonesimulator") or architecture not in ARCHITECTURES:
        raise ValueError("Unsupported iOS SDK or architecture")
    if sdk == "iphoneos" and architecture != "arm64":
        raise ValueError("Device builds require arm64; x86_64 is simulator-only")
    simulator = sdk == "iphonesimulator"
    triplet = ("arm64" if architecture == "arm64" else "x64") + "-ios" + ("-simulator" if simulator else "")
    triple = architecture + "-apple-ios" + MINIMUM + ("-simulator" if simulator else "")
    return Target(sdk, architecture, triplet, triple, MINIMUM)


def from_triplet(triplet):
    for sdk, architecture in (("iphoneos", "arm64"), ("iphonesimulator", "arm64"), ("iphonesimulator", "x86_64")):
        selected = target(sdk, architecture)
        if selected.triplet == triplet:
            return selected
    raise ValueError("Unsupported iOS dependency triplet: " + triplet)


def host_triplet(machine=None):
    machine = (machine or platform.machine()).lower()
    if machine in ("arm64", "aarch64"):
        return "arm64-osx"
    if machine in ("x86_64", "amd64"):
        return "x64-osx"
    raise ValueError("Unsupported native macOS host architecture: " + machine)


def claim(directory, selected, report_name=None, verify_legacy=None):
    """Reject a cache belonging to another architecture before changing it."""
    marker = directory / "corvcad-target.json"
    expected = {"sdk": selected.sdk, "architecture": selected.architecture}
    if marker.exists():
        if json.loads(marker.read_text()) != expected:
            raise ValueError("Build directory belongs to another SDK/architecture; use a separate root")
        return
    report = directory / report_name if report_name else None
    if directory.exists() and any(directory.iterdir()):
        if report is None or not report.is_file():
            raise ValueError("Unidentified existing target build; use a separate root")
        prior = json.loads(report.read_text())
        # Older accepted arm64 CPython reports encode architecture in target.
        architecture = prior.get("architecture")
        if architecture is None and str(prior.get("target", "")).startswith("arm64-apple-ios"):
            architecture = "arm64"
        if architecture is None and prior.get("sdk") == selected.sdk and verify_legacy is not None:
            architecture = verify_legacy(directory, prior)
        if prior.get("sdk") != selected.sdk or architecture != selected.architecture:
            raise ValueError("Cached build report belongs to another SDK/architecture or is unverified; use a separate root")
    directory.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps(expected, indent=2) + "\n")


if __name__ == "__main__":
    if sys.argv[1:] == ["host-triplet"]:
        print(host_triplet())
    elif len(sys.argv) == 4 and sys.argv[1] == "compiler-target":
        print(target(sys.argv[2], sys.argv[3]).compiler_target)
    else:
        raise SystemExit("Expected host-triplet or compiler-target SDK ARCHITECTURE")

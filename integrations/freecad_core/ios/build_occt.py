#!/usr/bin/env python3
"""Build the OCCT 7.8.1 static toolkit closure required by the F2 FreeCAD slice.

MIT. OCCT retains LGPL-2.1 with its additional exception and source notices.
The closure includes visualization data/presentation libraries used by XCAF,
but excludes OpenGL/OpenGLES drivers, viewers, Draw and desktop window systems.
This is build tooling; a successful build alone does not establish F3 acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import target_config

HERE = Path(__file__).resolve().parent
PIN = {
    "version": "7.8.1", "directory": "OCCT-7_8_1",
    "url": "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V7_8_1.tar.gz",
    "sha256": "7321af48c34dc253bf8aae3f0430e8cb10976961d534d8509e72516978aa82f5",
}
# FreeCAD 1.1.4 cMake/FindOCC.cmake, OCC_LIBRARIES + OCC_OCAF_LIBRARIES.
# Import still compiles its IGES/glTF adapters, so retain their toolkit roots.
# OCCT 7.8.1's STEP/IGES/glTF/STL providers include BinXCAFDrivers.hxx even
# though their EXTERNLIB lists omit TKBinXCAF. Keep the binary XCAF provider
# explicit; the validation below checks actual provider includes before build.
ROOTS = tuple("TKFillet TKMesh TKernel TKG2d TKG3d TKMath TKShHealing TKXSBase "
              "TKBool TKBO TKBRep TKTopAlgo TKGeomAlgo TKGeomBase TKOffset TKPrim "
              "TKHLR TKFeat TKDESTEP TKDEIGES TKDEGLTF TKDESTL TKBin TKBinL TKCAF "
              "TKXCAF TKLCAF TKVCAF TKCDF TKMeshVS TKService TKV3d TKRWMesh TKBinXCAF".split())
FORBIDDEN = {"TKOpenGl", "TKOpenGles", "TKIVtk", "TKD3DHost", "TKDraw", "DRAWEXE"}
NOTICES = ("LICENSE_LGPL_21.txt", "OCCT_LGPL_EXCEPTION.txt")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def extract(archive, destination, directory):
    """Require one source root and reject traversal, links and special files."""
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve() / directory):
                raise ValueError("OCCT archive path escapes source directory")
            if not (member.isfile() or member.isdir()):
                raise ValueError("Unexpected OCCT archive member type")
        package.extractall(destination, filter="data")


def verify_source(archive, directory):
    """Read-only cache verification: never replace or discard a local edit."""
    if directory.is_symlink() or any(p.is_symlink() for p in directory.rglob("*")):
        raise ValueError("Symlink in cached OCCT source")
    expected = {}
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            if member.isfile():
                relative = Path(member.name).relative_to(PIN["directory"])
                path = directory / relative
                expected[relative.as_posix()] = hashlib.sha256(package.extractfile(member).read()).hexdigest()
                if not path.is_file() or path.is_symlink() or digest(path) != expected[relative.as_posix()]:
                    raise ValueError(f"Modified OCCT source: {relative}")
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()}
    if actual != set(expected):
        raise ValueError(f"Unexpected OCCT source files: {sorted(actual - set(expected))[:5]}")
    return expected


def source(root):
    root.mkdir(parents=True, exist_ok=True)
    archive = root / "OCCT-V7_8_1.tar.gz"
    if not archive.exists():
        partial = archive.with_suffix(".download")
        with urllib.request.urlopen(PIN["url"], timeout=60) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output)
        partial.replace(archive)
    if digest(archive) != PIN["sha256"]:
        raise ValueError("OCCT archive checksum mismatch")
    directory = root / PIN["directory"]
    if not directory.exists():
        with tempfile.TemporaryDirectory(dir=root, prefix="extract-") as temporary:
            extract(archive, Path(temporary), PIN["directory"])
            (Path(temporary) / PIN["directory"]).rename(directory)
    return directory, verify_source(archive, directory)


def closure(source_dir, roots=ROOTS):
    """Compute OCCT's toolkit dependency graph from the pinned EXTERNLIB files."""
    graph = {}
    pending = list(roots)
    while pending:
        toolkit = pending.pop()
        if toolkit in graph:
            continue
        if toolkit in FORBIDDEN or "Draw" in toolkit or "Test" in toolkit:
            raise ValueError(f"Viewer/driver toolkit in OCCT closure: {toolkit}")
        path = source_dir / "src" / toolkit / "EXTERNLIB"
        if not path.is_file():
            raise ValueError(f"Unknown OCCT toolkit: {toolkit}")
        dependencies = sorted(set(line.strip() for line in path.read_text().splitlines()
                                  if line.strip().startswith("TK")))
        graph[toolkit] = dependencies
        pending.extend(dependencies)
    return dict(sorted(graph.items()))


def provider_dependencies(source_dir, graph):
    """Check compiled providers' includes against the selected toolkit closure.

    OCCT's EXTERNLIB metadata is incomplete for the data-exchange providers.
    PACKAGE/FILES ownership identifies their direct OCCT header dependencies;
    perform this check before compiling thousands of translation units.
    """
    owners = {}
    packages = {}
    for manifest in sorted((source_dir / "src").glob("TK*/PACKAGES")):
        toolkit = manifest.parent.name
        packages[toolkit] = manifest.read_text().split()
        for package in packages[toolkit]:
            listing = source_dir / "src" / package / "FILES"
            for name in listing.read_text().split():
                if name.endswith((".hxx", ".lxx", ".gxx")):
                    owners[name] = toolkit
    result = {}
    for toolkit in graph:
        dependencies = set()
        for package in packages.get(toolkit, []):
            directory = source_dir / "src" / package
            for name in (directory / "FILES").read_text().split():
                if not name.endswith("_Provider.cxx"):
                    continue
                text = (directory / name).read_text()
                for header in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE):
                    owner = owners.get(header)
                    if owner and owner != toolkit:
                        dependencies.add(owner)
        if dependencies:
            result[toolkit] = sorted(dependencies)
    missing = sorted({dependency for dependencies in result.values()
                      for dependency in dependencies if dependency not in graph})
    if missing:
        raise ValueError("OCCT provider headers need additional toolkit roots: " + ", ".join(missing))
    return result


def command(source_dir, build_dir, stage, sdk, minimum, architecture="arm64"):
    selected = target_config.target(sdk, architecture)
    switches = [f"-DBUILD_MODULE_{module}=OFF" for module in (
        "FoundationClasses", "ModelingData", "ModelingAlgorithms", "Visualization",
        "ApplicationFramework", "DataExchange", "DETools", "Draw")]
    switches += [f"-DUSE_{feature}=OFF" for feature in (
        "TCL", "TK", "FREETYPE", "FREEIMAGE", "FFMPEG", "OPENVR", "RAPIDJSON",
        "DRACO", "TBB", "EIGEN", "XLIB", "OPENGL", "GLES2", "D3D")]
    if shutil.which("ccache"):
        switches += ["-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", "-DCMAKE_C_COMPILER_LAUNCHER=ccache"]
    return ["cmake", "-S", str(source_dir), "-B", str(build_dir), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            f"-DCMAKE_OSX_SYSROOT={sdk}", "-DCMAKE_OSX_ARCHITECTURES=" + selected.architecture,
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={minimum}", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
            "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DBUILD_LIBRARY_TYPE=Static", "-DBUILD_DOC_Overview=OFF", "-DBUILD_USE_PCH=OFF",
            "-DINSTALL_DIR_LAYOUT=Unix", f"-DCMAKE_INSTALL_PREFIX={stage}",
            f"-DINSTALL_DIR={stage}", "-DBUILD_ADDITIONAL_TOOLKITS=" + " ".join(ROOTS), *switches]


def validate_platform(text, sdk, minimum):
    """Inspect every LC_BUILD_VERSION reported by otool for the archive members."""
    records = re.findall(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (\S+)\s+minos (\S+)", text)
    if not records:
        raise ValueError("No archive Mach-O build platforms were reported")
    expected = {"iphonesimulator": {"7", "IOSSIMULATOR"}, "iphoneos": {"2", "IOS"}}[sdk]
    normalize = lambda value: tuple((value.split(".") + ["0", "0"])[:3])
    if any(target.upper() not in expected or normalize(version) != normalize(minimum)
           for target, version in records):
        raise ValueError("OCCT archive contains an unexpected SDK or deployment target")
    return len(records)


def inventory(stage, graph):
    archives = list(stage.rglob("*.a"))
    expected = {"lib" + toolkit + ".a" for toolkit in graph}
    if {path.name for path in archives} != expected or len(archives) != len(expected):
        raise ValueError("Installed OCCT archives differ from the measured toolkit closure")
    if any(path.suffix in {".dylib", ".so", ".framework"} for path in stage.rglob("*")):
        raise ValueError("Dynamic library in static OCCT installation")
    return {p.relative_to(stage).as_posix(): {"sha256": digest(p), "bytes": p.stat().st_size}
            for p in sorted(archives)}


def run(command_line, directory, env):
    print("Running:", *map(str, command_line), flush=True)
    subprocess.run(list(map(str, command_line)), cwd=directory, env=env, check=True)


def cached_architecture(directory, report):
    """Identify an old unlabelled OCCT cache using its actual static archives."""
    stage = directory / "stage"
    architectures = set()
    if not report.get("archives"):
        raise ValueError("Legacy OCCT cache has no archive inventory; use a separate root")
    for relative, record in report["archives"].items():
        path = stage / relative
        if not path.resolve().is_relative_to(stage.resolve()) or digest(path) != record["sha256"]:
            raise ValueError("Legacy OCCT archive differs from its recorded hash")
        architectures.add(subprocess.check_output(["xcrun", "lipo", "-archs", str(path)], text=True).strip())
    if len(architectures) != 1:
        raise ValueError("Legacy OCCT cache has mixed archive architectures")
    return architectures.pop()


def build(root, sdk, jobs, architecture="arm64"):
    selected = target_config.target(sdk, architecture)
    root = root.resolve()
    src, source_hashes = source(root)
    graph = closure(src)
    providers = provider_dependencies(src, graph)
    minimum = json.loads((HERE / "inputs.json").read_text())["minimum_ios"]
    target = root / sdk
    target_config.claim(target, selected, "occt-build.json", verify_legacy=cached_architecture)
    stage = target / "stage"
    env = dict(os.environ)
    for key in ("SDKROOT", "CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
                "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH"):
        env.pop(key, None)
    configure = command(src, target / "build", stage, sdk, minimum, architecture)
    (target / "configure-command.json").write_text(json.dumps(configure, indent=2) + "\n")
    run(configure, target, env)
    run(["cmake", "--build", target / "build", "--parallel", jobs], target, env)
    run(["cmake", "--install", target / "build"], target, env)
    archives = inventory(stage, graph)
    for relative, record in archives.items():
        archive = stage / relative
        archs = subprocess.check_output(["xcrun", "lipo", "-archs", archive], text=True).strip()
        if archs != architecture:
            raise ValueError(f"Unexpected OCCT archive architectures: {relative}: {archs}")
        load_commands = subprocess.check_output(["xcrun", "otool", "-l", archive], text=True)
        (target / (archive.name + ".load-commands.txt")).write_text(load_commands)
        record["verified_build_versions"] = validate_platform(load_commands, sdk, minimum)
    notices = {}
    for name in NOTICES:
        destination = stage / "share/occt" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src / name, destination)
        notices[name] = {"sha256": digest(destination), "bytes": destination.stat().st_size}
    # Verify again: generated outputs must not have changed the pinned sources.
    if verify_source(root / "OCCT-V7_8_1.tar.gz", src) != source_hashes:
        raise ValueError("OCCT sources changed during build")
    report = {"schema": 1, "source": PIN, "sdk": sdk, "minimum_ios": minimum,
              "architecture": architecture, "patches": [], "toolkit_roots": ROOTS,
              "toolkit_dependencies": graph, "archives": archives, "notices": notices,
              "provider_header_dependencies": providers,
              "source_files": source_hashes, "configure_command": configure,
              "presentation_toolkits": sorted(set(graph) & {"TKService", "TKV3d", "TKMeshVS", "TKVCAF"}),
              "viewer_driver_toolkits": [],
              "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
              "sdk_version": subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-version"], text=True).strip()}
    (target / "occt-build.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("build/freecad-ios/occt"))
    parser.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, default="arm64")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("Building iOS requires macOS and Xcode")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build(args.root, args.sdk, args.jobs, args.architecture)

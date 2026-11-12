#!/usr/bin/env python3
"""Recompile the reviewed KiCad native closure against one workspace dependency set.

This supplements, rather than rewrites, Gate 1's inventory.  The existing audit must
verify first.  Desktop Ninja commands supply source-specific flags only; dependency
headers, wx flags, Apple target and compiler dependency files are selected here.
No old iOS sweep objects or merged dependency archives are reused.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
from compile_cache import compile_cached

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
KICAD = REPO / "integrations/kicad_core"
FMT_SOURCE = "kicad/thirdparty/fmt/src/format.cc"
# These audit roots exercise the ABI but do not belong in the app's native archive.
APP_OWNED = {"corvcad/engine/src/schematic_route.cpp"}
TEST_ROOTS = {"corvcad/integrations/kicad_core/session/board_test.c"}
REQUIRED_STANDINS = {"corvcad/integrations/kicad_core/headless/simulator_stub.cpp"}
DROP_DEFINES = {"__WXMAC__", "__WXOSX__", "__WXOSX_COCOA__", "WXUSINGDLL",
                "PROTOBUF_USE_DLLS"}
SOURCE_INCLUDE_OVERRIDES = {"pcbnew/pcbplot.cpp": HERE / "headless/ios"}


def sha(path, text=False):
    data = Path(path).read_bytes()
    return hashlib.sha256(data.replace(b"\r\n", b"\n") if text else data).hexdigest()


def run(args, cwd=None):
    result = subprocess.run([str(x) for x in args], cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{shlex.join([str(x) for x in args])}: {result.stderr.strip()}")
    return result.stdout.strip()


def target(sdk, architecture):
    if sdk == "iphoneos" and architecture != "arm64":
        raise ValueError("iphoneos requires arm64")
    if architecture not in ("arm64", "x86_64") or sdk not in ("iphoneos", "iphonesimulator"):
        raise ValueError("unsupported Apple target")
    return architecture + "-apple-ios17.0" + ("-simulator" if sdk == "iphonesimulator" else "")


def resolve_source(key, kicad, build):
    for prefix, root in (("kicad/", kicad), ("corvcad/", REPO), ("generated/", build / "generated")):
        if key.startswith(prefix):
            return (root / key[len(prefix):]).resolve()
    raise ValueError("Unclassified reviewed source: " + key)


def select_commands(database, sources):
    indexed = {}
    for row in database:
        source = Path(row["file"])
        if not source.is_absolute():
            source = Path(row["directory"]) / source
        indexed.setdefault(source.resolve(), []).append(row)
    selected = []
    for key, source in sources.items():
        rows = indexed.get(source.resolve(), [])
        if len(rows) != 1:
            raise ValueError(f"Expected one compiler command for {key}, found {len(rows)}")
        selected.append((key, rows[0]))
    return selected


def native_sources(profile):
    # A Debug dylib retains KIBIS references that a dead-stripped probe may omit.
    # The legacy packager explicitly adds this stand-in for that reason.
    return sorted(((set(profile["closure"]) | set(profile["direct_objects"]))
                   - APP_OWNED - TEST_ROOTS - {FMT_SOURCE}) | REQUIRED_STANDINS)


def transform_command(row, *, kicad, build, generated, prefix, standin, wx_flags,
                      sdk_path, triple, compiler, obj, depfile, extra):
    """Translate argv directly; never eval a Ninja shell command."""
    argv = row.get("arguments") or shlex.split(row["command"])
    if any(x in ("&&", ";", "|", "||") or x.startswith("@") for x in argv):
        raise ValueError("Shell operations and response files require an explicit reviewed recipe")
    output = [str(compiler), "-I" + str(standin), "-I" + str(prefix / "include"),
              "-I" + str(extra)]
    source = row.get("file")
    if source is None and "-c" in argv:
        source = argv[argv.index("-c") + 1]
    if source is not None:
        source = Path(source)
        if not source.is_absolute():
            source = Path(row["directory"]) / source
        for relative, include in SOURCE_INCLUDE_OVERRIDES.items():
            if source.resolve() == (kicad / relative).resolve():
                output.insert(1, "-I" + str(include))
    skip_arg = {"-o", "-MF", "-MT", "-MQ", "-isysroot", "-target", "-arch"}
    desktop_roots = ("/usr/local", "/opt/homebrew", "/Library/Frameworks/Python")
    i = 1
    while i < len(argv):
        token = argv[i]
        i += 1
        if token in skip_arg:
            i += 1
            continue
        if token in ("-MD", "-MMD", "-MP"):
            continue
        if token.startswith("-D") and token[2:].split("=", 1)[0] in DROP_DEFINES:
            continue
        flag, path = None, None
        if token in ("-I", "-isystem", "-iquote"):
            flag, path = token, argv[i]
            i += 1
        else:
            for option in ("-isystem", "-iquote", "-I"):
                if token.startswith(option) and len(token) > len(option):
                    flag, path = option, token[len(option):]
                    break
        if path is not None:
            if path.startswith(desktop_roots):
                continue
            candidate = Path(path)
            if not candidate.is_absolute():
                candidate = Path(row["directory"]) / candidate
            candidate = candidate.resolve()
            if str(candidate).startswith(desktop_roots) or candidate.is_relative_to(kicad / "thirdparty/fmt"):
                continue
            if candidate.is_relative_to(build / "generated/cpp"):
                candidate = generated / candidate.relative_to(build / "generated/cpp")
            output += [flag, str(candidate)]
            continue
        if token.startswith("-miphoneos-version-min=") or token.startswith("-mmacosx-version-min="):
            continue
        if not token.startswith("-") and Path(token).is_relative_to(build / "generated/cpp"):
            token = str(generated / Path(token).relative_to(build / "generated/cpp"))
        output.append(token)
    # KiCad's bundled fmt 12.1 core.h included format.h. fmt 12.2 makes
    # that deprecated compatibility behavior an explicit public opt-in.
    # Keep KiCad's include contract while using only the union 12.2 headers
    # and compiled library; do not restore the vendored implementation.
    output += ["-DFMT_DEPRECATED_HEAVY_CORE=1"]
    output += list(wx_flags) + ["-isysroot", str(sdk_path), "-target", triple,
                               "-MD", "-MF", str(depfile), "-MT", str(obj), "-o", str(obj)]
    return output


def parse_depfile(text):
    text = text.replace("\\\r\n", "").replace("\\\n", "")
    # The generated target is an absolute macOS object path, without a drive colon.
    if ":" not in text:
        raise ValueError("Compiler dependency file has no target")
    dependencies = shlex.split(text.split(":", 1)[1], comments=False)
    return sorted(set(x.replace("$$", "$") for x in dependencies))


def dependency_key(path, roots):
    path = path.resolve()
    for label, root in roots:
        if path.is_relative_to(root):
            key = label + "/" + path.relative_to(root).as_posix()
            if key.startswith("kicad/thirdparty/fmt/"):
                raise ValueError("Vendored fmt header reached the workspace build: " + key)
            return key
    raise ValueError("Unclassified compiler input (possible desktop header contamination): " + str(path))


def validate_dependency_receipt(data, sdk, architecture, prefix):
    if data.get("schema") != 1 or data.get("status") != "dependency inputs built and target archives verified":
        raise ValueError("Workspace dependency receipt did not pass")
    selected = data.get("target", {})
    if selected.get("sdk") != sdk or selected.get("architecture") != architecture:
        raise ValueError("Workspace dependency receipt target mismatch")
    if Path(data.get("paths", {}).get("prefix", "")).resolve() != prefix.resolve():
        raise ValueError("Workspace dependency receipt prefix mismatch")
    if not data.get("inventory", {}).get("archives") or not data.get("macho", {}).get("archives"):
        raise ValueError("Workspace dependency receipt lacks archive evidence")
    if data["macho"].get("sdk") != sdk or data["macho"].get("architecture") != architecture:
        raise ValueError("Workspace Mach-O evidence target mismatch")


def verify_dependency_inputs(receipt, prefix, protoc):
    prefix = prefix.resolve()
    installed = Path(receipt["paths"]["installed"]).resolve()
    expected_protoc = installed / receipt["target"]["host_triplet"] / "tools/protobuf/protoc"
    if protoc.resolve() != expected_protoc.resolve():
        raise ValueError("protoc must come from the union manifest's host triplet")
    for name in ("inventory", "macho"):
        evidence = Path(receipt["paths"][name])
        if sha(evidence) != receipt[name + "_sha256"] or json.loads(evidence.read_text()) != receipt[name]:
            raise ValueError("Workspace dependency inventory file changed: " + name)
    for relative, expected in receipt["inventory"]["archives"].items():
        archive = (prefix / relative).resolve()
        if not archive.is_relative_to(prefix) or sha(archive) != expected["sha256"]:
            raise ValueError("Workspace dependency archive changed: " + relative)
        if receipt["macho"]["archives"].get(relative, {}).get("sha256") != expected["sha256"]:
            raise ValueError("Workspace archive lacks matching Mach-O evidence: " + relative)
    fmt = prefix / "include/fmt/base.h"
    if "#define FMT_VERSION 120200" not in fmt.read_text():
        raise ValueError("Expected the reviewed union fmt 12.2 headers")


def compile_one(unit, roots, generated, output):
    key, row, command, obj, depfile = unit
    log = output / "logs" / (obj.stem + ".log")
    result = subprocess.run(command, cwd=row["directory"], capture_output=True)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Compile failed for {key}; see {log}")
    dependencies = {}
    for name in parse_depfile(depfile.read_text()):
        path = Path(name)
        if not path.is_absolute():
            path = Path(row["directory"]) / path
        portable = dependency_key(path, roots)
        dependencies[portable] = {"sha256": sha(path), "sha256_lf": sha(path, text=True)}
    # A successful -MD compile must at least report its source.
    if not dependencies:
        raise RuntimeError("Empty compiler dependency evidence for " + key)
    return {"source": key, "object": {"path": str(obj), "sha256": sha(obj)},
            "command": command, "dependencies": sorted(dependencies)}, dependencies


def claim_output(output, expected, resume=False):
    marker = output / "build-claim.json"
    if output.exists():
        if not resume:
            raise ValueError("Choose a new output directory or explicitly resume its matching claim")
        if marker.is_symlink() or not marker.is_file() or json.loads(marker.read_text()) != expected:
            raise ValueError("Output has no matching native build claim; existing files are preserved")
    else:
        if resume:
            raise ValueError("Cannot resume a missing native build directory")
        output.mkdir(parents=True)
        marker.write_text(json.dumps(expected, indent=2) + "\n")


def build(args):
    kicad, desktop, prefix, wx, output = [getattr(args, name).resolve() for name in
                                        ("kicad", "build", "prefix", "wx_build", "output")]
    if output.exists() and not args.resume:
        raise ValueError("Choose a new output directory; existing build evidence is preserved")
    triple = target(args.sdk, args.architecture)
    receipt = json.loads(args.dependency_receipt.read_text())
    validate_dependency_receipt(receipt, args.sdk, args.architecture, prefix)
    verify_dependency_inputs(receipt, prefix, args.protoc)
    run([sys.executable, KICAD / "audit.py", "verify", "--kicad", kicad])
    inventory = json.loads((KICAD / "audit/sources.json").read_text())
    profile = inventory["profiles"]["native"]
    omitted = APP_OWNED | TEST_ROOTS | {FMT_SOURCE}
    keys = native_sources(profile)
    if not REQUIRED_STANDINS.issubset(inventory["files"]):
        raise ValueError("Required stand-in lacks baseline source/license inventory")
    sources = {key: resolve_source(key, kicad, desktop) for key in keys}
    rules = [x for x in run([args.ninja, "-C", desktop, "-t", "rules"]).splitlines() if "_COMPILER__" in x]
    if not rules:
        raise ValueError("No compiler rules in desktop Ninja build")
    database = json.loads(run([args.ninja, "-C", desktop, "-t", "compdb", *rules]))
    selected = select_commands(database, sources)
    sdk_path = Path(run(["xcrun", "--sdk", args.sdk, "--show-sdk-path"])).resolve()
    compiler = Path(run(["xcrun", "--sdk", args.sdk, "--find", "clang++"])).resolve()
    c_compiler = Path(run(["xcrun", "--sdk", args.sdk, "--find", "clang"])).resolve()
    resource = Path(run([compiler, "-print-resource-dir"])).resolve()
    if run([compiler, "--version"]) != receipt["toolchain"]["compiler"]:
        raise ValueError("Dependency compiler differs from native compiler")
    if run(["xcrun", "--sdk", args.sdk, "--show-sdk-version"]) != receipt["toolchain"]["sdk_version"]:
        raise ValueError("Dependency SDK differs from native SDK")
    wx_flags = shlex.split(run([wx / "wx-config", "--cxxflags"]))
    # wx flags must originate in this target build/source, not Homebrew wx.
    if any("/usr/local" in x or "/opt/homebrew" in x for x in wx_flags):
        raise ValueError("Desktop wx flags reached workspace build")
    claim = {"schema": 1, "target": triple, "compiler_sha256": sha(compiler),
             "sdk_version": receipt["toolchain"]["sdk_version"],
             "dependency_receipt_sha256": sha(args.dependency_receipt),
             "baseline_sources_sha256": sha(KICAD / "audit/sources.json"),
             "recipe_sha256_lf": sha(Path(__file__), text=True),
             "cache_recipe_sha256_lf": sha(HERE / "compile_cache.py", text=True),
             "wx_flags": wx_flags,
             "paths": {"kicad": str(kicad), "desktop": str(desktop), "prefix": str(prefix),
                       "wx_build": str(wx), "wx_source": str(args.wx_source.resolve())}}
    claim_output(output, claim, args.resume)
    generated = output / "generated/cpp"
    for folder in (output / "objects", output / "logs", output / "depfiles", generated / "api", output / "extra"):
        folder.mkdir(parents=True, exist_ok=True)
    freetype = output / "extra/freetype2"
    if freetype.is_symlink() and freetype.resolve() == (prefix / "include").resolve():
        pass
    elif freetype.exists() or freetype.is_symlink():
        raise ValueError("Unexpected existing FreeType include shim")
    else:
        freetype.symlink_to(prefix / "include", target_is_directory=True)
    protobuf_sources = sorted({p.relative_to(desktop / "generated/cpp/api").with_suffix(".proto")
                               for p in (desktop / "generated/cpp/api").rglob("*.pb.cc")})
    # foo.pb.cc -> foo.proto, retaining directories. Use the host protoc associated
    # with the same pinned union manifest; its path is explicit, never PATH-selected.
    protos = [str(p).replace(".pb.proto", ".proto") for p in protobuf_sources]
    if protos:
        run([args.protoc.resolve(), "--cpp_out=" + str(generated / "api"),
             "--proto_path=" + str(kicad / "api/proto"), *protos], cwd=kicad / "api/proto")
    else:
        raise ValueError("Missing generated baseline protobuf source list")
    roots = [("generated-target", generated), ("generated-baseline", desktop / "generated"),
             ("kicad", kicad), ("wx-build", wx), ("wx-source", args.wx_source.resolve()),
             ("vcpkg", prefix), ("apple-sdk", sdk_path), ("clang-resource", resource), ("corvcad", REPO)]
    units = []
    for key, row in selected:
        name = hashlib.sha256(key.encode()).hexdigest()[:16] + "-" + Path(key).name
        obj, depfile = output / "objects" / (name + ".o"), output / "depfiles" / (name + ".d")
        command = transform_command(row, kicad=kicad, build=desktop, generated=generated,
                                    prefix=prefix, standin=KICAD / "headless/ios", wx_flags=wx_flags,
                                    sdk_path=sdk_path, triple=triple,
                                    compiler=c_compiler if sources[key].suffix == ".c" else compiler, obj=obj,
                                    depfile=depfile, extra=output / "extra")
        units.append((key, row, command, obj, depfile))
    (output / "commands.json").write_text(json.dumps([u[2] for u in units], indent=2) + "\n")
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(lambda unit: compile_cached(unit, roots, generated, output, compile_one), units))
    compiled, dependencies = [], {}
    for record, observed in results:
        compiled.append(record)
        for key, value in observed.items():
            if key in dependencies and dependencies[key] != value:
                raise RuntimeError("Compiler input changed during build: " + key)
            dependencies[key] = value
    archive = output / "libcorvcad_kicad.a"
    run(["xcrun", "libtool", "-static", "-no_warning_for_no_symbols", "-o", archive,
         *[u[3] for u in units]])
    result = {"schema": 1, "status": "passed", "sdk": args.sdk, "architecture": args.architecture,
              "target": triple, "compiler": run([compiler, "--version"]),
              "protobuf_generator": {"path": str(args.protoc.resolve()), "sha256": sha(args.protoc),
                                     "version": run([args.protoc.resolve(), "--version"]),
                                     "schemas": {p: sha(kicad / "api/proto" / p, text=True) for p in protos}},
              "core": {"path": str(archive), "sha256": sha(archive)},
              "dependency_receipt": {"path": str(args.dependency_receipt.resolve()),
                                     "sha256": sha(args.dependency_receipt)},
              "baseline_audit": {"verified": True, "sources_sha256": sha(KICAD / "audit/sources.json"),
                                 "notices_sha256": sha(KICAD / "audit/notices.json")},
              "replacement": {"omitted": sorted(omitted), "required_standins": sorted(REQUIRED_STANDINS),
                              "fmt": "one union vcpkg header/library implementation",
                              "fmt_core_compatibility": "FMT_DEPRECATED_HEAVY_CORE=1"},
              "supplemental_headers": {
                  "corvcad/integrations/native_workspace/headless/ios/dialog_plot.h": {
                      "sha256_lf": sha(HERE / "headless/ios/dialog_plot.h", text=True),
                      "license": "GPL-3.0-or-later",
                      "scope": "Omit desktop plot-dialog declaration for pcbplot.cpp only; retain the native plot implementation"}},
              "compiled_units": compiled, "source_inventory": dict(sorted(dependencies.items())),
              "recipe_sha256_lf": sha(Path(__file__), text=True),
              "build_claim": claim,
              "physical_device_validation": False}
    (output / "kicad-build.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("kicad", "build", "wx-build", "wx-source", "prefix", "dependency-receipt", "protoc", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--architecture", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--resume", action="store_true", help="Resume a matching owned build; verify object and actual header hashes before reuse")
    args = parser.parse_args()
    try:
        if args.jobs < 1:
            raise ValueError("jobs must be positive")
        result = build(args)
        print(json.dumps({"status": result["status"], "core": result["core"],
                          "units": len(result["compiled_units"])}))
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

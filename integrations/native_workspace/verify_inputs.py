"""Recheck the measured inputs of a mixed native development link. MIT.

This does not approve an app or release. Source inventories and raw archives
must still be present; stale, independently bundled dependencies are rejected.
"""
import importlib.util
import json
from pathlib import Path, PurePosixPath
import sys

import kicad_build as native
import wx_build

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "freecad_core/ios"))
import cross
import prepare_ios


def load(path):
    return json.loads(Path(path).read_text())


def verify_files(records, roots):
    """Recheck portable compiler input keys without accepting escaping paths."""
    for key, record in records.items():
        parts = PurePosixPath(key).parts
        if (len(parts) < 2 or "\\" in key or ":" in key or
                any(part in (".", "..") for part in key.split("/")) or parts[0] not in roots):
            raise ValueError("Unclassified or escaping compiler input: " + key)
        root = Path(roots[parts[0]]).resolve()
        path = (root / Path(*parts[1:])).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError("Compiler input escaped its recorded root: " + key)
        if native.sha(path) != record["sha256"] or native.sha(path, text=True) != record["sha256_lf"]:
            raise ValueError("Compiler input changed: " + key)


def verify_hashes(records):
    for name, expected in records.items():
        path = Path(name)
        expected = expected["sha256"] if isinstance(expected, dict) else expected
        if not path.is_file() or native.sha(path) != expected:
            raise ValueError("Raw native archive changed: " + name)


def wx_header_compatibility(kicad, wx):
    """Permit a new wx dependency configuration only with identical used headers.

    The KiCad compile still records its original wx root. Rebinding that root
    requires checking every wx build input it actually included, including setup.h.
    Source inputs are checked separately against the unchanged pinned source tree.
    """
    records = {key: value for key, value in kicad["source_inventory"].items() if key.startswith("wx-build/")}
    if not records:
        raise ValueError("KiCad receipt has no actual wx configuration/header evidence")
    verify_files(records, {"wx-build": Path(wx["build"])})
    return {"compiled_build": kicad["build_claim"]["paths"]["wx_build"],
            "linked_build": wx["build"], "identical_actual_compiler_inputs": records}


def wx_system_flags(flags, prefix, build):
    """Retain only explicit system links; archives are supplied separately."""
    result = []
    index = 0
    while index < len(flags):
        flag = flags[index]
        index += 1
        if flag == "-framework":
            if index == len(flags):
                raise ValueError("Incomplete wx framework option")
            result += [flag, flags[index]]
            index += 1
        elif flag.startswith("-L"):
            if Path(flag[2:]).resolve() not in ((build / "lib").resolve(), (prefix / "lib").resolve()):
                raise ValueError("Unexpected wx library search path")
        elif flag.endswith(".a") or flag.startswith("-lwx") or flag == "-lexpat":
            continue
        elif flag in ("-lpthread", "-liconv", "-lobjc", "-lm", "-lc++"):
            result.append(flag)
        else:
            raise ValueError("Unreviewed wx link flag: " + flag)
    return result


def verify(args):
    sdk, architecture = args.sdk, args.architecture
    receipt_path = args.dependency_receipt.resolve()
    dep = load(receipt_path)
    prefix = Path(dep["paths"]["prefix"]).resolve()
    native.validate_dependency_receipt(dep, sdk, architecture, prefix)
    protoc = Path(dep["paths"]["installed"]) / dep["target"]["host_triplet"] / "tools/protobuf/protoc"
    native.verify_dependency_inputs(dep, prefix, protoc)
    sdk_path = Path(native.run(["xcrun", "--sdk", sdk, "--show-sdk-path"])).resolve()
    compiler = Path(native.run(["xcrun", "--sdk", sdk, "--find", "clang++"])).resolve()
    resource = Path(native.run([compiler, "-print-resource-dir"])).resolve()
    if (native.run([compiler, "--version"]) != dep["toolchain"]["compiler"] or
            native.run(["xcrun", "--sdk", sdk, "--show-sdk-version"]) != dep["toolchain"]["sdk_version"]):
        raise ValueError("Current link toolchain differs from the shared build")

    kicad_path, wx_path = args.kicad_receipt.resolve(), args.wx_receipt.resolve()
    kicad, wx = load(kicad_path), load(wx_path)
    for data in (kicad, wx):
        reference = data["dependency_receipt"]
        if Path(reference["path"]).resolve() != receipt_path or reference["sha256"] != native.sha(receipt_path):
            raise ValueError("Engine build used another dependency receipt")
        if data["compiler"] != dep["toolchain"]["compiler"]:
            raise ValueError("Engine build compiler differs from shared dependencies")
    if (kicad.get("status") != "passed" or kicad.get("sdk") != sdk or
            kicad.get("architecture") != architecture or
            kicad.get("recipe_sha256_lf") != native.sha(HERE / "kicad_build.py", text=True)):
        raise ValueError("KiCad supplemental build receipt did not match the reviewed recipe/target")
    claim = kicad["build_claim"]
    if claim.get("cache_recipe_sha256_lf") != native.sha(HERE / "compile_cache.py", text=True):
        raise ValueError("KiCad compiler checkpoint recipe changed")
    paths = claim["paths"]
    native.run([sys.executable, native.KICAD / "audit.py", "verify", "--kicad", paths["kicad"]])
    baseline = kicad["baseline_audit"]
    if (baseline["sources_sha256"] != native.sha(native.KICAD / "audit/sources.json") or
            baseline["notices_sha256"] != native.sha(native.KICAD / "audit/notices.json")):
        raise ValueError("KiCad baseline inventory changed")
    roots = {"generated-target": kicad_path.parent / "generated/cpp",
             "generated-baseline": Path(paths["desktop"]) / "generated", "kicad": paths["kicad"],
             "wx-build": paths["wx_build"], "wx-source": paths["wx_source"], "vcpkg": prefix,
             "apple-sdk": sdk_path, "clang-resource": resource, "corvcad": REPO}
    ngspice = native.ngspice_build(paths["desktop"])
    if str(ngspice) != paths.get("ngspice", "None"):
        raise ValueError("KiCad was compiled with another ngspice build")
    if ngspice:
        roots["ngspice"] = ngspice
    verify_files(kicad["source_inventory"], roots)
    profile = load(native.KICAD / "audit/sources.json")["profiles"]["native"]
    if sorted(unit["source"] for unit in kicad["compiled_units"]) != native.native_sources(profile):
        raise ValueError("KiCad build no longer covers the measured native source closure")
    verify_hashes({unit["object"]["path"]: unit["object"]["sha256"] for unit in kicad["compiled_units"]})
    verify_hashes({kicad["core"]["path"]: kicad["core"]["sha256"]})

    if (wx.get("target") != dep["target"] or wx.get("regex_provider", {}).get("kind") != "shared PCRE2-32" or
            wx.get("nanosvg_private_prefixes") != wx_build.NANOSVG_PRIVATE_PREFIXES or
            wx.get("recipe_sha256_lf") != native.sha(HERE / "wx_build.py", text=True)):
        raise ValueError("Combined link requires the reviewed wx shared-PCRE2 build")
    wx_source = Path(paths["wx_source"])
    verify_files(wx["source_inventory"], {"wx-build": Path(wx["build"]), "wx-source": wx_source,
                 "vcpkg": prefix, "apple-sdk": sdk_path, "clang-resource": resource})
    if native.sha(prefix / "lib/libpcre2-32.a") != wx["regex_provider"]["archive_sha256"]:
        raise ValueError("wx shared PCRE2 archive changed")
    verify_hashes(wx["archives"])
    selected_wx = wx_build.selected_archives(wx["link_flags"], Path(wx["build"]), prefix, system_regex=True)
    if {str(path) for path in selected_wx} != set(wx["archives"]):
        raise ValueError("wx raw archive selection differs from its verified receipt")
    compatibility = wx_header_compatibility(kicad, wx)

    # Import the F4 packager under a distinct name, avoiding ambient package modules.
    spec = importlib.util.spec_from_file_location("native_freecad_package", HERE.parent / "freecad_core/session/package.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    freecad_root, freecad_package = args.freecad_root.resolve(), args.freecad_package.resolve()
    _, fc_prefix, _, _, _ = cross.inputs(freecad_root, sdk, architecture)
    if fc_prefix.resolve() != prefix:
        raise ValueError("FreeCAD uses another shared dependency prefix")
    python = load(freecad_root / "python" / sdk / "python-build.json")
    if python.get("system_expat") is not True:
        raise ValueError("Combined link requires target Python with shared Expat")
    source = prepare_ios.verify(freecad_root / "source-ios/FreeCAD")
    manifest = package.verify_package(freecad_package, sdk, architecture)
    core, fc_deps, system = package.link_inputs(freecad_root / ("freecad-" + sdk))
    for key, archives in (("core_inputs", core), ("dependency_inputs", fc_deps)):
        actual = {str(path): native.sha(path) for path in archives}
        if actual != manifest[key]:
            raise ValueError("FreeCAD raw link inputs differ from verified package: " + key)
    for name, expected in manifest["source_files"].items():
        if native.sha(HERE.parent / "freecad_core/session" / name) != expected:
            raise ValueError("FreeCAD session source changed: " + name)
    deps = [prefix / name for name in dep["inventory"]["archives"]]
    deps += [Path(path) for path in wx["archives"]] + fc_deps
    if ngspice:
        # ngspice_ios.sh's static library for this SDK, as ios_package.sh links it.
        library = ngspice / ("build-" + ("device" if sdk == "iphoneos" else "sim")) / "libngspice.a"
        if not library.is_file():
            raise ValueError("Missing ngspice build for this SDK: " + str(library))
        deps.append(library)
    system += wx_system_flags(wx["link_flags"], prefix, Path(wx["build"]))
    # Retain the explicit platform frameworks in the reviewed KiCad iOS link.
    # Security is required by target networking/crypto dependencies and is not
    # part of wx-config's base/core/xml selection.
    system += ["-framework", "Security"]
    return {"sdk": sdk, "architecture": architecture, "prefix": prefix,
            "core": [Path(kicad["core"]["path"]), *core], "dependencies": deps,
            "system": system, "runtime": freecad_package / "FreeCADRuntime",
            "evidence": {"dependency_receipt_sha256": native.sha(receipt_path),
                         "kicad_receipt_sha256": native.sha(kicad_path), "wx_receipt_sha256": native.sha(wx_path),
                         "freecad_manifest_sha256": native.sha(freecad_package / "package-manifest.json"),
                         "freecad_source": source, "wx_header_compatibility": compatibility}}

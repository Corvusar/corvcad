#!/usr/bin/env python3
"""Link the optional mixed native C ABI probe from verified raw inputs. MIT.

This creates link evidence only. Runtime, combined app, physical device and
release acceptance require their own checks; no dependency archives are merged.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

import combine
import select_archives

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "integrations/freecad_core/ios"))
import binary_report
import target_config

SCOPE = "Raw combined probe link only; no runtime, app, physical-device or release acceptance"


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def system_options(words, archives=()):
    """Deduplicate complete framework pairs, never individual argument words."""
    result, seen, i = [], set(), 0
    raw_names = {path.name for path in archives}
    while i < len(words):
        word = words[i]
        if word in ("-framework", "-weak_framework"):
            if i + 1 >= len(words) or not words[i + 1] or words[i + 1].startswith("-"):
                raise ValueError("Incomplete system framework option")
            option = (word, words[i + 1])
            i += 2
        elif word.startswith("-l") and len(word) > 2 and not any(c in word for c in "/\\\n\r"):
            option = (word,)
            i += 1
            if "lib" + word[2:] + ".a" in raw_names:
                continue
        else:
            raise ValueError("Unreviewed system link option: " + word)
        if option not in seen:
            seen.add(option)
            result.extend(option)
    return result


def link_options(selected, core, system):
    selected = list(dict.fromkeys(path.resolve(strict=True) for path in selected))
    core = list(dict.fromkeys(path.resolve(strict=True) for path in core))
    if not core or not set(core).issubset(selected):
        raise ValueError("Archive selection omitted a required native core")
    for path in selected:
        if path.name in combine.BUNDLED or path.suffix != ".a":
            raise ValueError("Expected original raw static archives: " + str(path))
    # Registration objects are mandatory only in the native adapters/modules.
    # Ordinary dependency archives retain normal linker extraction semantics.
    # Retain the reviewed KiCad iOS link policy for unused optional code paths.
    # The preceding whole-input collision scan still examines every object.
    result = ["-Wl,-dead_strip"]
    result.extend(word for path in core for word in ("-Xlinker", "-force_load", "-Xlinker", str(path)))
    result.extend(str(path) for path in selected if path not in core)
    result.extend(system_options(system, selected))
    return result


def compile_flags(prefix, sdk_path, sdk, architecture):
    selected = target_config.target(sdk, architecture)
    # clang++ resolves to the same executable as clang on Xcode. Keep the
    # language driver explicit even after canonicalizing the executable path,
    # so object-only links still select libc++ and the C++ exception runtime.
    return ["--driver-mode=g++", "-std=c++20", "-target", selected.compiler_target, "-isysroot", str(sdk_path),
            "-DQT_CORE_LIB", "-DQT_STATIC", "-I" + str(REPO / "engine/include"),
            "-I" + str(prefix / "include/Qt6"), "-I" + str(prefix / "include/Qt6/QtCore")]


def dependencies(depfile):
    text = depfile.read_text().replace("\\\n", " ")
    if ":" not in text:
        raise ValueError("Malformed compiler dependency file")
    paths = {Path(word).resolve(strict=True) for word in shlex.split(text.split(":", 1)[1], comments=False)}
    if not paths or any(not path.is_file() for path in paths):
        raise ValueError("Missing compiler input dependency")
    return paths


def snapshot(paths):
    return {str(path.resolve(strict=True)): combine.digest(path)
            for path in sorted(set(paths), key=str)}


def unchanged(before):
    for name, checksum in before.items():
        path = Path(name)
        if not path.is_file() or combine.digest(path) != checksum:
            raise ValueError("Link input changed: " + name)


def compose(args):
    # Claim a new output atomically before writing any evidence. Never resume or
    # overwrite an earlier failed link directory.
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Combined link output must be new: " + str(output))
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    selection_file, preflight_file = output / "input-selection.json", output / "preflight.json"
    write_json(selection_file, {"schema": 1, "status": "not run", "scope": SCOPE})
    write_json(preflight_file, {"schema": 1, "status": "not run", "scope": SCOPE})
    report = {"schema": 1, "status": "failed", "stage": "verification", "scope": SCOPE,
              "runtime_validation": False, "app_validation": False, "physical_device_validation": False,
              "commands": []}

    def save():
        write_json(output / "link-recipe.json", report)

    def run(command, log, stage):
        report["stage"] = stage
        record = {"stage": stage, "argv": list(map(str, command)), "log": log.name}
        report["commands"].append(record)
        save()
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(record["argv"], cwd=REPO, stdout=stream, stderr=subprocess.STDOUT, text=True)
        record["returncode"] = process.returncode
        save()
        if process.returncode:
            raise RuntimeError(stage + " failed; see " + str(log))

    try:
        # The verifier owns source/header/package/runtime provenance. Keep it
        # separate from the mechanical archive-selection and link operations.
        import verify_inputs
        sources = [HERE / "probe/mixed.cpp", REPO / "engine/src/schematic_route.cpp"]
        receipt_paths = [args.dependency_receipt, args.wx_receipt, args.kicad_receipt,
                         args.freecad_package / "package-manifest.json"]
        recipe_paths = [Path(__file__), Path(combine.__file__), Path(select_archives.__file__),
                        Path(verify_inputs.__file__)]
        before = snapshot([*sources, *receipt_paths, *recipe_paths])
        report["input_sha256"] = before
        verified = verify_inputs.verify(args)
        unchanged(before)
        if verified["sdk"] != args.sdk or verified["architecture"] != args.architecture:
            raise ValueError("Verified inputs belong to another target")
        report["verified_inputs"] = verified["evidence"]
        report["runtime"] = str(verified["runtime"])
        report["target"] = {"sdk": args.sdk, "architecture": args.architecture, "minimum_ios": "17.0"}
        report["stage"] = "archive selection"
        all_archives = [*verified["core"], *verified["dependencies"]]
        before.update(snapshot(all_archives))
        try:
            selected, selection = select_archives.select(all_archives)
        except select_archives.OmissionError as error:
            write_json(selection_file, error.report)
            raise
        write_json(selection_file, selection)
        unchanged(before)
        options = link_options(selected, verified["core"], verified["system"])
        report["stage"] = "raw archive preflight"
        preflight = combine.inspect(selected, args.sdk, args.architecture)
        write_json(preflight_file, preflight)
        unchanged(before)
        if preflight["strong_symbol_conflicts"]:
            raise ValueError("Raw archive preflight found strong-symbol collisions")
        response = output / "mixed-link.rsp"
        response.write_text(shlex.join(options) + "\n", encoding="utf-8")
        sdk_path = Path(subprocess.check_output(["xcrun", "--sdk", args.sdk, "--show-sdk-path"], text=True).strip()).resolve(strict=True)
        compiler = Path(subprocess.check_output(["xcrun", "--sdk", args.sdk, "--find", "clang++"], text=True).strip()).resolve(strict=True)
        report["compiler"] = {"path": str(compiler), "version": subprocess.check_output([str(compiler), "--version"], text=True).strip()}
        common = [str(compiler), *compile_flags(verified["prefix"], sdk_path, args.sdk, args.architecture)]
        unchanged(before)
        before.update(snapshot([compiler, response]))
        units = []
        # Hash the compiler's actual complete header closure before compilation,
        # including SDK headers. Then require the real compile to use that set.
        for source in sources:
            stem = source.stem
            scan, actual, obj = (output / (stem + suffix) for suffix in ("-scan.d", ".d", ".o"))
            run([*common, "-M", "-MT", stem, "-MF", str(scan), str(source)],
                output / (stem + "-scan.log"), "dependency scan " + stem)
            observed = dependencies(scan)
            if source.resolve() not in observed:
                raise ValueError("Compiler dependencies omit source: " + str(source))
            added = snapshot(observed)
            unchanged(before)
            before.update(added)
            units.append((source, actual, obj, observed))
        report["input_sha256"] = before
        save()
        for source, depfile, obj, observed in units:
            run([*common, "-MD", "-MF", str(depfile), "-c", str(source), "-o", str(obj)],
                output / (source.stem + "-compile.log"), "compile " + source.stem)
            if dependencies(depfile) != observed:
                raise ValueError("Compiler input closure changed: " + str(source))
            unchanged(before)
        objects = snapshot([unit[2] for unit in units])
        before.update(objects)
        binary, link_map = output / "mixed-probe", output / "mixed-link.map"
        command = [*common, *[str(unit[2]) for unit in units], "@" + str(response),
                   "-Xlinker", "-map", "-Xlinker", str(link_map), "-o", str(binary)]
        run(command, output / "link.log", "link")
        unchanged(before)
        report["stage"] = "binary validation"
        arch = subprocess.check_output(["xcrun", "lipo", "-archs", str(binary)], text=True).strip()
        if arch != args.architecture:
            raise ValueError("Mixed probe architecture differs from requested target")
        platform_text = subprocess.check_output(["xcrun", "vtool", "-show-build", str(binary)], text=True)
        dynamic_text = subprocess.check_output(["xcrun", "otool", "-L", str(binary)], text=True)
        (output / "macho-build.txt").write_text(platform_text, encoding="utf-8")
        (output / "dynamic-links.txt").write_text(dynamic_text, encoding="utf-8")
        links = binary_report.check(platform_text, dynamic_text, args.sdk)
        if not binary.is_file() or not binary.stat().st_size or not link_map.is_file() or not link_map.stat().st_size:
            raise ValueError("Missing or empty mixed probe/link map")
        unchanged(before)
        report.update(status="link passed", stage="complete", input_hashes_unchanged=True,
                      input_sha256=before, objects_sha256=objects,
                      binary={"path": str(binary), "sha256": combine.digest(binary), "bytes": binary.stat().st_size},
                      link_map={"path": str(link_map), "sha256": combine.digest(link_map), "bytes": link_map.stat().st_size},
                      dynamic_system_links=links)
        save()
        return report
    except Exception as error:
        report["error"] = str(error)
        save()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("dependency-receipt", "wx-receipt", "kicad-receipt", "freecad-root", "freecad-package", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, required=True)
    args = parser.parse_args()
    for name in ("dependency_receipt", "wx_receipt", "kicad_receipt", "freecad_root", "freecad_package"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    try:
        result = compose(args)
        print(json.dumps({"status": result["status"], "binary": result["binary"], "scope": result["scope"]}))
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

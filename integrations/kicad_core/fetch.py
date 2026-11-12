#!/usr/bin/env python3
"""Fetch exact KiCad 10 sources, apply CorvCAD's patch series, and pack the symbol and
footprint libraries (the NATIVE_KICAD app bundles both; no 3D models, see docs/3D_MODELS.md).

This prepares inputs for the source port and the probe in integrations/kicad_core/probe; it
does not build or link KiCad. Outputs stay outside the tracked source tree by default.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
from audit import SourceTree, patches, sha as source_sha

LOCK = json.loads(Path(__file__).resolve().with_name("upstream.json").read_text(encoding="utf-8"))
# What the probe compiles; the rest (demos, translations, other tools) is left out, which also
# avoids over-long demo paths on Windows.
SPARSE_PATHS = ("3d-viewer", "api", "cmake", "common", "eeschema", "include", "kicad", "libs",
                "pcbnew", "resources", "scripting", "thirdparty", "utils")


def git(*args: str, cwd: Path | None = None) -> str:
    command = ["git"]
    if sys.platform == "win32":
        command += ["-c", "http.sslBackend=openssl", "-c", "core.longpaths=true"]
    command += list(args)
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def checkout(spec: dict, target: Path, sparse: bool) -> None:
    if not target.exists():
        git("clone", "--filter=blob:none", "--depth=1", "--branch", spec["tag"],
            *(["--sparse"] if sparse else []), spec["repository"], str(target))
    if not (target / ".git").exists():
        raise RuntimeError(f"{target} is not an upstream Git checkout")
    actual = git("rev-parse", "HEAD", cwd=target)
    if actual != spec["commit"]:
        raise RuntimeError(f"{target} is at {actual}; expected {spec['commit']}")
    if sparse:
        git("sparse-checkout", "set", *SPARSE_PATHS, cwd=target)
    elif (target / ".git" / "info" / "sparse-checkout").exists():
        git("sparse-checkout", "disable", cwd=target)


def apply_patches(target: Path) -> None:
    # Reverse-checking one patch fails when a later patch overlaps it. Replay the
    # entire declared series in an isolated index, then compare the final state.
    tree = SourceTree(target)
    try:
        tree.check_worktree(tree.changed)
        print("Verified complete patch series (already applied)")
        return
    except RuntimeError:
        ignored = {LOCK["schematic_source"]["tag"], LOCK["schematic_source"]["tag"] + "-dirty"}
        changed = set(git("diff", "HEAD", "--name-only", cwd=target).splitlines())
        changed.update(git("ls-files", "--others", "--exclude-standard", cwd=target).splitlines())
        if changed - ignored:
            raise RuntimeError("Integration checkout has partial patches or undeclared edits; "
                               "preserve it and fetch into a new output directory")
    for patch in patches():
        git("apply", "--whitespace=nowarn", str(patch), cwd=target)
        print(f"Applied: {patch.name}")
    tree.check_worktree(tree.changed)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def symbol_index(packed: Path) -> None:
    """index.json beside the packed libraries, so the app can search all symbols without
    loading a library: [library, name, reference prefix, description, keywords, default
    footprint, pin count, unit count, footprint filters (ki_fp_filters)] per symbol. A derived
    symbol (extends) takes its base's pins and units. KiCad writes one top-level symbol per
    tab-indented "(symbol" line."""
    rows = []
    for library in sorted(packed.glob("*.kicad_sym")):
        text = library.read_text(encoding="utf-8")
        symbols = {}
        for block in re.split(r"\n\t\(symbol ", text)[1:]:
            name = re.match(r'"((?:[^"\\]|\\.)*)"', block).group(1)
            props = dict(re.findall(r'\n\t\t\(property "([^"]+)" "((?:[^"\\]|\\.)*)"', block))
            base = re.search(r'\n\t\t\(extends "((?:[^"\\]|\\.)*)"\)', block)
            units = {int(u) for u in re.findall(r'\(symbol "' + re.escape(name) + r'_(\d+)_\d+"', block)} - {0}
            symbols[name] = {"props": props, "base": base.group(1) if base else None,
                             "pins": len(re.findall(r"\(pin \w+ \w+", block)), "units": max(units, default=1)}
        for name, symbol in symbols.items():
            source = symbols.get(symbol["base"], symbol) if symbol["base"] else symbol
            props = {**source["props"], **symbol["props"]}
            rows.append([library.stem, name, props.get("Reference", ""), props.get("Description", ""),
                         props.get("ki_keywords", ""), props.get("Footprint", ""), source["pins"], source["units"],
                         props.get("ki_fp_filters", "")])
    (packed / "index.json").write_text(json.dumps(rows, ensure_ascii=False, separators=(",", ":")) + "\n",
                                       encoding="utf-8")
    print(f"Indexed {len(rows)} symbols")


def pack_footprints(output: Path) -> None:
    """The official .pretty libraries and their table, which already refers to
    ${KICAD10_FOOTPRINT_DIR}. Footprints only: KiCad's 3D models are not bundled."""
    pin = LOCK["footprint_libraries"]
    source = output / "kicad-footprints"
    checkout(pin, source, False)
    if source_sha((source / "LICENSE.md").read_bytes()) != pin["license_sha256_lf"]:
        raise RuntimeError("Pinned footprint library license does not match the reviewed notice")
    packed = output / "footprints-packed"
    shutil.rmtree(packed, ignore_errors=True)
    libraries = sorted(source.glob("*.pretty"))
    if len(libraries) != pin["library_count"] or not (source / "fp-lib-table").is_file():
        raise RuntimeError(f"Incomplete KiCad 10 footprint set: {len(libraries)} libraries")
    for library in libraries:
        shutil.copytree(library, packed / library.name)
    shutil.copy2(source / "fp-lib-table", packed / "fp-lib-table")
    shutil.copy2(source / "LICENSE.md", packed / "LICENSE-KICAD-FOOTPRINTS.md")
    (packed / "manifest.json").write_text(json.dumps({
        "upstream_commit": pin["commit"], "license": pin["license"],
        "libraries": {library.name: {file.name: digest(file) for file in sorted(library.glob("*.kicad_mod"))}
                      for library in libraries}
    }, indent=1) + "\n", encoding="utf-8")
    print(f"Packed {len(libraries)} KiCad footprint libraries into {packed}")
    footprint_index(packed)


def footprint_index(packed: Path) -> None:
    """index.json beside the packed footprints, for the app's footprint chooser:
    [library, name, pad count, description] per footprint. The pad count is CvPcb's: distinct
    non-empty pad numbers (an exposed pad numbered like a pin counts once)."""
    rows = []
    for library in sorted(packed.glob("*.pretty")):
        for file in sorted(library.glob("*.kicad_mod")):
            text = file.read_text(encoding="utf-8")
            pads = {number for number in re.findall(r'\(pad "((?:[^"\\]|\\.)*)"', text) if number}
            description = re.search(r'\(descr "((?:[^"\\]|\\.)*)"\)', text)
            rows.append([library.stem, file.stem, len(pads), description.group(1) if description else ""])
    (packed / "index.json").write_text(json.dumps(rows, ensure_ascii=False, separators=(",", ":")) + "\n",
                                       encoding="utf-8")
    print(f"Indexed {len(rows)} footprints")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/kicad-core"))
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--libraries-only", "--symbols-only", dest="libraries_only", action="store_true")
    parser.add_argument("--full-source", action="store_true",
                        help="Checkout all KiCad paths for a corresponding-source archive")
    args = parser.parse_args()
    if args.source_only and args.libraries_only:
        parser.error("Choose at most one of --source-only and --libraries-only")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    if not args.libraries_only:
        checkout(LOCK["schematic_source"], output / "kicad", not args.full_source)
        apply_patches(output / "kicad")
    if not args.source_only:
        symbols = output / "kicad-symbols"
        checkout(LOCK["symbol_libraries"], symbols, False)
        if source_sha((symbols / "LICENSE.md").read_bytes()) != LOCK["symbol_libraries"]["license_sha256_lf"]:
            raise RuntimeError("Pinned symbol library license does not match the reviewed notice")
        packed = output / "symbols-packed"
        packed.mkdir(exist_ok=True)
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        subprocess.run([sys.executable, str(symbols / LOCK["symbol_libraries"]["packer"]),
                        "--input", str(symbols), "--output", str(packed),
                        "--table", str(symbols / "sym-lib-table")],
                       check=True, env=environment)
        libraries = sorted(packed.glob("*.kicad_sym"))
        if len(libraries) != LOCK["symbol_libraries"]["library_count"] or not (packed / "sym-lib-table").is_file():
            raise RuntimeError(f"Incomplete KiCad 10 symbol bundle: {len(libraries)} libraries")
        shutil.copy2(symbols / "LICENSE.md", packed / "LICENSE-KICAD-SYMBOLS.md")
        # KiCad installs the libraries' SPICE models beside them: Simulation_SPICE.sp holds the
        # models of the Simulation_SPICE symbols, which schematics name as
        # ${KICAD<n>_SYMBOL_DIR}/Simulation_SPICE.sp for the simulator.
        models = sorted(symbols.glob("*.sp"))
        for model in models:
            shutil.copy2(model, packed / model.name)
        (packed / "manifest.json").write_text(json.dumps({
            "upstream_commit": LOCK["symbol_libraries"]["commit"],
            "license": LOCK["symbol_libraries"]["license"],
            "libraries": {file.name: digest(file) for file in libraries},
            "spice_models": {file.name: digest(packed / file.name) for file in models}
        }, indent=2) + "\n", encoding="utf-8")
        print(f"Packed {len(libraries)} KiCad symbol libraries into {packed}")
        symbol_index(packed)
        pack_footprints(output)
    print("Verified pinned upstream inputs; no KiCad source was built or linked")


if __name__ == "__main__":
    main()

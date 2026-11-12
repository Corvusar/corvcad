"""Prepare and verify an immutable, local F5 input packet. MIT. No device access."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import uuid

HERE = Path(__file__).resolve().parent
CORE = HERE.parent
REPO = CORE.parents[1]
CASES = {
    "demo": (REPO / "engine/tests/fixtures", ("demo.kicad_pcb",)),
    "roundtrip": (REPO / "engine/tests/fixtures/roundtrip",
                  ("RoundTrip.kicad_pcb", "RoundTrip.kicad_pro", "RoundTrip.kicad_sch")),
}


def record(path):
    data = Path(path).read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def save(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")


def inside(root, relative):
    if not isinstance(relative, str):
        raise ValueError("Expected a packet-relative regular file")
    part = PurePosixPath(relative)
    if not isinstance(relative, str) or part.is_absolute() or not part.parts or ".." in part.parts or "\\" in relative or ":" in relative:
        raise ValueError("Expected a packet-relative regular file")
    candidate = root.joinpath(*part.parts)
    if candidate.is_symlink() or not candidate.is_file() or not candidate.resolve().is_relative_to(root.resolve()):
        raise ValueError("Packet file is missing or escapes its root: " + relative)
    return candidate


def prepare(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=REPO, text=True).strip())
    manifest = {"schema": 1, "gate": "F5-preparation", "f5_accepted": False,
                "revision": revision, "working_tree_dirty": dirty, "run_id": uuid.uuid4().hex, "cases": {}}
    for name, (source, filenames) in CASES.items():
        case = output / name
        case.mkdir()
        for filename in filenames:
            shutil.copyfile(source / filename, case / filename)
        board = case / filenames[0]
        expected = json.loads((CORE / "fixtures" / (name + ".json")).read_text(encoding="utf-8-sig"))
        expected["source"]["sha256"] = record(board)["sha256"]
        save(case / "expected-input.json", expected)
        save(case / "request.json", {"schema": 1, "board": board.name})
        manifest["cases"][name] = {"board": board.name, "files": {
            path.name: record(path) for path in sorted(case.iterdir())}}
    manifest["tool_sources"] = {}
    for path in sorted(HERE.glob("*.py")):
        manifest["tool_sources"][path.name] = record(path)
    manifest["app_sources"] = {name: record(REPO / "app/Sources" / name) for name in (
        "MechanicalDeviceDiagnostics.swift", "MechanicalSession.swift", "Diagnostics.swift")}
    save(output / "packet.json", manifest)
    return manifest


def verify(packet):
    packet = Path(packet)
    manifest = json.loads((packet / "packet.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != 1 or set(manifest.get("cases", {})) != set(CASES):
        raise ValueError("Unsupported or incomplete F5 packet")
    run = manifest.get("run_id", "")
    if len(run) != 32 or any(c not in "0123456789abcdef" for c in run):
        raise ValueError("Invalid packet run identity")
    for name, case in manifest["cases"].items():
        if not case.get("files") or case.get("board") not in case["files"]:
            raise ValueError("Missing packet board")
        for relative, expected in case["files"].items():
            if record(inside(packet, name + "/" + relative)) != expected:
                raise ValueError("F5 packet hash mismatch: " + relative)
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "verify"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    result = prepare(args.path) if args.action == "prepare" else verify(args.path)
    print(json.dumps({"run_id": result["run_id"], "cases": list(result["cases"]), "f5_accepted": False}))

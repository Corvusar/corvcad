"""Desktop KiCad CLI adapter. The iPad app does not execute this module."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def find_cli() -> str:
    override = os.environ.get("KICAD_CLI")
    if override:
        if not Path(override).is_file():
            raise FileNotFoundError(f"KICAD_CLI does not exist: {override}")
        return override
    found = shutil.which("kicad-cli")
    if found:
        return found
    if os.name == "nt":
        candidates = sorted(Path("C:/Program Files/KiCad").glob("*/bin/kicad-cli.exe"), reverse=True)
        if candidates:
            return str(candidates[0])
    raise FileNotFoundError("kicad-cli not found; install KiCad or set KICAD_CLI")


def run(cli: str, *args: str) -> None:
    command = [cli, *args]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.stdout:
        print(result.stdout.rstrip())
    if result.returncode:
        raise RuntimeError(f"KiCad command failed ({result.returncode}): {' '.join(args)}")


def check(cli: str, source: Path, output: Path) -> int:
    if source.suffix not in (".kicad_pcb", ".kicad_sch"):
        raise ValueError("Check requires a .kicad_pcb or .kicad_sch file")
    command = ("sch", "erc") if source.suffix == ".kicad_sch" else ("pcb", "drc")
    output.parent.mkdir(parents=True, exist_ok=True)
    run(cli, *command, "--format", "json", "--output", str(output), str(source))
    report = json.loads(output.read_text(encoding="utf-8"))
    violations = report.get("violations")
    if violations is None:
        violations = [item for sheet in report.get("sheets", [])
                      for item in sheet.get("violations", [])]
    unconnected = report.get("unconnected_items", [])
    print(f"KiCad {command[1]} report: {output} "
          f"({len(violations)} violations, {len(unconnected)} unconnected items)")
    return len(violations) + len(unconnected)


def fabricate(cli: str, source: Path, output: Path, allow_violations: bool = False) -> None:
    if source.suffix != ".kicad_pcb":
        raise ValueError("Fabrication output requires a .kicad_pcb file")
    output.mkdir(parents=True, exist_ok=True)
    issues = check(cli, source, output / "drc.json")
    if issues and not allow_violations:
        raise RuntimeError(f"KiCad DRC found {issues} issues; fabrication export stopped")
    gerbers = output / "gerbers"
    drills = output / "drill"
    gerbers.mkdir(exist_ok=True)
    drills.mkdir(exist_ok=True)
    run(cli, "pcb", "export", "gerbers", "--output", str(gerbers), str(source))
    run(cli, "pcb", "export", "drill", "--output", str(drills), str(source))
    run(cli, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
        "--output", str(output / "positions.csv"), str(source))
    run(cli, "pcb", "export", "step", "--force", "--output", str(output / "board.step"), str(source))
    print(f"KiCad fabrication outputs: {output}")


def schematic(cli: str, source: Path, output: Path, allow_violations: bool = False) -> None:
    if source.suffix != ".kicad_sch":
        raise ValueError("Schematic output requires a .kicad_sch file")
    output.mkdir(parents=True, exist_ok=True)
    issues = check(cli, source, output / "erc.json")
    if issues and not allow_violations:
        raise RuntimeError(f"KiCad ERC found {issues} issues; schematic export stopped")
    run(cli, "sch", "export", "netlist", "--format", "kicadsexpr",
        "--output", str(output / "netlist.net"), str(source))
    run(cli, "sch", "export", "bom", "--output", str(output / "bom.csv"), str(source))
    run(cli, "sch", "export", "pdf", "--output", str(output / "schematic.pdf"), str(source))
    print(f"KiCad schematic outputs: {output}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "verify", "fabricate", "schematic"))
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--allow-violations", action="store_true",
                        help="export even when KiCad reports DRC/ERC issues")
    args = parser.parse_args(argv)
    source = args.source.resolve()
    if not source.is_file():
        parser.error(f"Input does not exist: {source}")
    try:
        cli = find_cli()
        if args.command in ("check", "verify"):
            count = check(cli, source, args.output.resolve())
            if args.command == "verify" and count:
                raise RuntimeError(f"KiCad validation found {count} issues")
        elif args.command == "fabricate":
            fabricate(cli, source, args.output.resolve(), args.allow_violations)
        else:
            schematic(cli, source, args.output.resolve(), args.allow_violations)
    except (FileNotFoundError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

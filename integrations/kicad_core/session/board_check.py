"""Native board checks (port gate 5) against kicad-cli.

  board_check.py reference <reference.json> <board.kicad_pcb>...
      With the installed KiCad (10.0.6 here): kicad-cli DRC without and with --refill-zones,
      and the board it saves after the refill. Writes a compact summary per board.
  board_check.py compare <reference.json> <board_test> <board.kicad_pcb>...
      Runs session/board_test.c's program on each board and compares with the summary.
  board_check.py fab-reference <fab_reference.json> <board.kicad_pcb>...
      With the installed KiCad: Gerbers, job file, Excellon drill and placement as
      cc_board_native_fab_outputs writes them, digested without dates and version lines.
  board_check.py fab <fab_reference.json> <board_test> [--keep=<dir>] <board.kicad_pcb>...
      Runs `board_test fab` on each board and compares the files' digests.
  board_check.py schematic <board_test> <bom_reference.json> [--keep=<dir>] <project.kicad_pro>...
      KiCad's schematic tools (session API 6 to 8) on a copy of each project; BOM and plot
      against kicad-cli's. notes-parity <kept project dir>... then compares, on Windows,
      kicad-cli's netlist and ERC of each edited copy with the session's.
  board_check.py route <board_test> <board.kicad_pcb> [net]
      Routes the board's ratsnest with KiCad's router after stripping its tracks and checks it
      with KiCad's DRC (no new violations, fewer unconnected items).

A DRC list is compared by type, severity, item UUIDs and positions (as probe/compare_pcb.sh
does), except the footprint-library checks: they compare footprints with the installed
libraries, which kicad-cli here has and the Mac and iPad do not; those are counted apart. A
refill is compared per zone and layer: the filled area and the outline vertex count (teardrops
are regenerated first, as kicad-cli does). Boards are matched by file name, and their
LF-normalised SHA-256 must agree.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


LIBRARY_CHECKS = {"lib_footprint_issues", "lib_footprint_mismatch"}
# ERC's library checks: they depend on which symbol and footprint libraries resolve where ERC runs.
ERC_LIBRARY_CHECKS = {"footprint_link_issues", "lib_symbol_issues", "lib_symbol_mismatch"}


def drc_items(report: Path) -> list[list]:
    data = json.loads(report.read_text(encoding="utf-8"))
    return sorted([key, v["type"], v["severity"],
                   sorted([i.get("uuid"), round(i["pos"]["x"], 4), round(i["pos"]["y"], 4)] for i in v.get("items", []))]
                  for key in ("violations", "unconnected_items") for v in data.get(key, []))


def blocks(text: str, head: str):
    """Each balanced s-expression starting with `(head` (strings may hold parentheses)."""
    for match in re.finditer(r"\(" + head + r"[\s)]", text):
        depth, quoted, i = 0, False, match.start()
        while True:
            c = text[i]
            if quoted:
                if c == "\\":
                    i += 1
                elif c == '"':
                    quoted = False
            elif c == '"':
                quoted = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    yield text[match.start():i + 1]
                    break
            i += 1


def fills(board: Path) -> dict[str, list]:
    """Zone uuid/layer -> [filled area mm^2, outline vertex count]. Filled polygons are stored
    fractured (holes joined to the outline), so the shoelace sum is the copper area."""
    result: dict[str, list] = {}
    teardrops: dict[str, list] = {}
    for index, zone in enumerate(blocks(board.read_text(encoding="utf-8"), "zone")):
        if re.search(r"\(attr\s*\(teardrop", zone):
            # A refill regenerates teardrops with new UUIDs, in no fixed order: compare them as
            # a sorted list per layer.
            for polygon in blocks(zone, "filled_polygon"):
                layer = re.search(r'\(layer "([^"]+)"\)', polygon).group(1)
                points = [(float(x), float(y)) for x, y in re.findall(r"\(xy ([-\d.]+) ([-\d.]+)\)", polygon)]
                area = abs(sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]))) / 2
                teardrops.setdefault(f"teardrops/{layer}", []).append(round(area, 4))
            continue
        uuid = re.search(r'\((?:uuid|tstamp) "?([^")]+)"?\)', zone)
        uuid = uuid.group(1) if uuid else f"#{index}"  # older footprint zones have neither
        for polygon in blocks(zone, "filled_polygon"):
            layer = re.search(r'\(layer "([^"]+)"\)', polygon).group(1)
            points = [(float(x), float(y)) for x, y in re.findall(r"\(xy ([-\d.]+) ([-\d.]+)\)", polygon)]
            area = abs(sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]))) / 2
            entry = result.setdefault(f"{uuid}/{layer}", [0.0, 0])
            entry[0] += area
            entry[1] += len(points)
    summary = {key: [round(area, 4), count] for key, (area, count) in sorted(result.items())}
    # Same shape as a zone entry: [total area, polygon count], plus the sorted areas.
    for key, areas in sorted(teardrops.items()):
        summary[key] = [round(sum(areas), 4), len(areas), sorted(areas)]
    return summary


def reference(output: Path, boards: list[Path]) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "kicad_cli"))
    from bridge import find_cli
    cli, summary = find_cli(), {}
    for board in boards:
        with tempfile.TemporaryDirectory() as work:
            copy = Path(work) / "board"
            shutil.copytree(board.parent, copy)
            target = copy / board.name
            run = lambda *extra, out: subprocess.run(
                [cli, "pcb", "drc", "--format", "json", "--units", "mm", *extra, "-o", str(Path(work) / out), str(target)],
                check=True, capture_output=True)
            run(out="drc.json")
            run("--refill-zones", out="drc-refilled.json")
            run("--refill-zones", "--save-board", out="drc-saved.json")
            summary[board.name] = {"sha256": digest(board), "drc": drc_items(Path(work) / "drc.json"),
                                   "drc_refilled": drc_items(Path(work) / "drc-refilled.json"),
                                   "fills": fills(target)}
        entry = summary[board.name]
        print(board.name, len(entry["drc"]), len(entry["drc_refilled"]), len(entry["fills"]), "zone layers")
    output.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")


def compare(references: Path, program: Path, boards: list[Path]) -> int:
    expected, failed = json.loads(references.read_text(encoding="utf-8")), 0
    for board in boards:
        want = expected.get(board.name)
        if not want:
            print(f"{board.name}: no reference")
            failed = 1
            continue
        if want["sha256"] != digest(board):
            print(f"{board.name}: not the referenced file")
            failed = 1
            continue
        with tempfile.TemporaryDirectory() as work:
            run = subprocess.run([str(program), str(board), work], capture_output=True, text=True)
            print(board.name, run.stdout.strip(), run.stderr.strip()[-400:], f"(exit {run.returncode})" if run.returncode else "",
                  flush=True)
            if run.returncode:
                failed = 1
                continue
            got = {"drc": drc_items(Path(work) / "drc.json"), "drc_refilled": drc_items(Path(work) / "drc-refilled.json"),
                   "fills": fills(Path(work) / "filled.kicad_pcb")}
            edges = (Path(work) / "ratsnest.txt").read_text(encoding="utf-8").splitlines()
            rules = (Path(work) / "netrules.txt").read_text(encoding="utf-8").splitlines()
        # Net rules: the Default netclass first, with positive track width and via sizes.
        default = rules[0].split(" ") if rules else []
        rules_ok = len(default) >= 4 and all(float(v) > 0 for v in default[:3])
        print(f"  net rules: {len(rules)} lines, Default {' '.join(default[:4]) or 'missing'} mm"
              f" ({'ok' if rules_ok else 'WRONG'})")
        failed |= not rules_ok
        # kicad-cli's DRC lists each unrouted ratsnest edge as an unconnected item.
        unconnected = sum(item[0] == "unconnected_items" for item in want["drc"])
        print(f"  ratsnest: {len(edges)} unrouted edges native, {unconnected} unconnected items kicad-cli"
              f" ({'same' if len(edges) == unconnected else 'DIFFER'})")
        failed |= len(edges) != unconnected
        for key in ("drc", "drc_refilled"):
            library = [sum(i[1] in LIBRARY_CHECKS for i in x[key]) for x in (got, want)]
            got[key], want[key] = ([i for i in x[key] if i[1] not in LIBRARY_CHECKS] for x in (got, want))
            if any(library):
                print(f"  {key}: footprint-library checks not compared ({library[0]} native, {library[1]} kicad-cli)")
            same = got[key] == want[key]
            print(f"  {key}: {'identical' if same else 'DIFFERS'} ({len(got[key])} native, {len(want[key])} kicad-cli)")
            if not same:
                failed = 1
                for item in [i for i in want[key] if i not in got[key]][:3]:
                    print("    only kicad-cli:", json.dumps(item)[:200])
                for item in [i for i in got[key] if i not in want[key]][:3]:
                    print("    only native:", json.dumps(item)[:200])
        a, b = got["fills"], want["fills"]
        close = lambda x, y: math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-3)
        area = [k for k in a.keys() & b.keys() if not close(a[k][0], b[k][0])
                or (len(a[k]) > 2 and (len(a[k][2]) != len(b[k][2]) or not all(map(close, a[k][2], b[k][2]))))]
        vertices = [k for k in a.keys() & b.keys() if a[k][1] != b[k][1]]
        same = a.keys() == b.keys() and not area
        verdict = "same areas" if same else "DIFFER"
        print(f"  fills: {verdict} ({len(a)} zone layers native, {len(b)} kicad-cli; "
              f"{len(vertices)} with other vertex counts)")
        if not same:
            failed = 1
            for key in sorted(a.keys() ^ b.keys())[:3]:
                print("    only", "native:" if key in a else "kicad-cli:", key)
            for key in area[:3]:
                print(f"    {key}: native {a[key]}, kicad-cli {b[key]}")
    return failed


def footprints(text: str) -> dict[str, tuple[str, dict[str, str]]]:
    """Reference -> (footprint block, pad number -> net name)."""
    result = {}
    for block in blocks(text, "footprint"):
        ref = re.search(r'\(property "Reference" "([^"]*)"', block) or re.search(r'\(fp_text reference "?([^"\s)]*)', block)
        pads = {}
        for pad in blocks(block, "pad"):
            number = re.match(r'\(pad "?([^"\s]*)"?', pad).group(1)
            net = re.search(r'\(net (?:\d+ )?"([^"]*)"\)', pad)
            pads[number] = net.group(1) if net else ""
        if ref:
            result[ref.group(1)] = (block, pads)
    return result


def update(program: Path, board: Path, netlist: Path, reference: str) -> int:
    """Delete `reference`'s footprint, run Update PCB from Schematic with the netlist, and check
    that it comes back from the footprint libraries with every footprint's pad nets as before."""
    original = footprints(board.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory() as work:
        project = Path(work) / "project"
        shutil.copytree(board.parent, project)
        target = project / board.name
        target.write_text(board.read_text(encoding="utf-8").replace(original[reference][0], ""), encoding="utf-8")
        run = subprocess.run([str(program), "update", str(target), str(netlist), work], capture_output=True, text=True)
        print(run.stdout.strip()[-1500:], run.stderr.strip()[-400:])
        if run.returncode:
            return 1
        updated = footprints((Path(work) / "updated.kicad_pcb").read_text(encoding="utf-8"))
    want = {ref: pads for ref, (_, pads) in original.items()}
    got = {ref: pads for ref, (_, pads) in updated.items()}
    same = want == got
    print(f"update: {reference} {'restored' if reference in got else 'MISSING'}; pad nets of "
          f"{len(got)} footprints {'identical' if same else 'DIFFER'} to the original board")
    for ref in sorted(set(want) | set(got)):
        if want.get(ref) != got.get(ref):
            print(f"  {ref}: original {want.get(ref)}, updated {got.get(ref)}")
    return 0 if same else 1


def swap_update(program: Path, board: Path, netlist: Path, reference: str, footprint: str, keep: Path) -> int:
    """Update PCB from Schematic with `reference` given `footprint` in the netlist: the board's
    footprint is exchanged (headless PCB_EDIT_FRAME::ExchangeFootprint), every footprint keeps
    its pad nets, and the result is kept with the session's DRC in keep/<board> for edit-parity."""
    original = footprints(board.read_text(encoding="utf-8"))
    text = netlist.read_text(encoding="utf-8")
    comp = next(b for b in blocks(text, "comp") if f'(ref "{reference}")' in b)
    changed = re.sub(r'\(footprint "[^"]*"\)', f'(footprint "{footprint}")', comp, count=1)
    target = keep / board.stem
    shutil.copytree(board.parent, target, dirs_exist_ok=True)
    (target / "changed.net").write_text(text.replace(comp, changed), encoding="utf-8")
    run = subprocess.run([str(program), "update", str(target / board.name), str(target / "changed.net"), str(target)],
                         capture_output=True, text=True)
    if run.returncode:
        print(f"swap update: failed {run.stderr.strip()[-400:]}")
        return 1
    updated_board = target / "updated.kicad_pcb"
    shutil.move(str(updated_board), str(target / board.name))
    (target / "drc-run").mkdir(exist_ok=True)
    drc = subprocess.run([str(program), str(target / board.name), str(target / "drc-run")], capture_output=True, text=True)
    if (target / "drc-run" / "drc.json").exists():
        shutil.copy(target / "drc-run" / "drc.json", target / "drc.json")
    updated = footprints((target / board.name).read_text(encoding="utf-8"))
    exchanged = reference in updated and f'(footprint "{footprint}"' in updated[reference][0]
    nets = {r: p for r, (_, p) in updated.items()} == {r: p for r, (_, p) in original.items()}
    print(f"swap update: {reference} {'now' if exchanged else 'NOT'} {footprint}; pad nets of {len(updated)} footprints "
          f"{'identical' if nets else 'DIFFER'}; {len([l for l in run.stdout.splitlines() if 'Changed' in l])} changed")
    return 0 if exchanged and nets and drc.returncode == 0 else 1


def ratsnest(program: Path, board: Path, expected: int) -> int:
    """Strip every track from the board and compare the native ratsnest's edge count with
    kicad-cli's unconnected items on the same stripped board (`expected`, measured with
    kicad-cli 10.0.6), so the counts are not trivially zero."""
    with tempfile.TemporaryDirectory() as work:
        project = Path(work) / "project"
        shutil.copytree(board.parent, project)
        target = project / board.name
        text = board.read_text(encoding="utf-8")
        for segment in list(blocks(text, "segment")):
            text = text.replace(segment, "")
        target.write_text(text, encoding="utf-8")
        run = subprocess.run([str(program), str(target), work], capture_output=True, text=True)
        if run.returncode:
            print(run.stderr.strip()[-400:])
            return 1
        edges = (Path(work) / "ratsnest.txt").read_text(encoding="utf-8").splitlines()
    print(f"ratsnest without tracks: {len(edges)} unrouted edges native, {expected} unconnected items kicad-cli"
          f" ({'same' if len(edges) == expected else 'DIFFER'})")
    return 0 if len(edges) == expected else 1


def route(program: Path, board: Path, net: str | None = None) -> int:
    """Strip every track and via, route the ratsnest with KiCad's router (board_test route), and
    check with KiCad's DRC on both boards that routing added no violation (clearance, shorts,
    crossing tracks, holes...) and removed one unconnected item per route at least."""
    with tempfile.TemporaryDirectory() as work:
        project = Path(work) / "project"
        shutil.copytree(board.parent, project)
        target = project / board.name
        text = board.read_text(encoding="utf-8")
        for item in list(blocks(text, "segment")) + list(blocks(text, "via")) + list(blocks(text, "arc")):
            text = text.replace(item, "")
        target.write_text(text, encoding="utf-8")
        # Routed in a second copy of the project under the board's own name, so the router and
        # both DRC runs use the project's rules (netclasses, text and clearance constraints).
        routed_project = Path(work) / "routed"
        shutil.copytree(project, routed_project)
        command = [str(program), "route", str(target), str(routed_project)] + ([net] if net else [])
        run = subprocess.run(command, capture_output=True, text=True)
        print(run.stdout.strip(), run.stderr.strip()[-600:], f"(exit {run.returncode})" if run.returncode else "")
        if run.returncode < 0 and shutil.which("lldb"):
            # A crash: the same run under lldb for the backtrace (the copy is re-made first).
            shutil.rmtree(routed_project)
            shutil.copytree(project, routed_project)
            debug = subprocess.run(["lldb", "--batch", "-o", "run", "-k", "bt 30", "-k", "quit", "--"] + command,
                                   capture_output=True, text=True)
            print(debug.stdout[-6000:])
        counts = re.search(r"route: (\d+) of (\d+)", run.stdout)
        if run.returncode or not counts:
            return 1
        routed_board = routed_project / board.name
        reports = {}
        for name, path in (("before", target), ("after", routed_board)):
            out = Path(work) / name
            out.mkdir()
            check = subprocess.run([str(program), str(path), str(out)], capture_output=True, text=True)
            if check.returncode:
                print(name, check.stderr.strip()[-400:])
                return 1
            for kind in ("drc", "drc-refilled"):
                data = json.loads((out / f"{kind}.json").read_text(encoding="utf-8"))
                reports[name, kind] = (data.get("violations", []), len(data.get("unconnected_items", [])))
    routed, total = int(counts.group(1)), int(counts.group(2))
    failed = 0
    # Zones are fills, not router obstacles: KiCad refills them after an edit, so the refilled
    # DRC is the router's check; as loaded, the stale fills are reported apart.
    for kind in ("drc-refilled", "drc"):
        before, after = reports["before", kind], reports["after", kind]
        added = Counter(v["type"] for v in after[0]) - Counter(v["type"] for v in before[0])
        ok = not added and after[1] <= before[1] - routed
        print(f"route ({kind}): violations {len(before[0])} before, {len(after[0])} after"
              f"{', new: ' + ', '.join(sorted(added.elements())) if added else ' (none new)'}; unconnected items "
              f"{before[1]} -> {after[1]} for {routed} routes ({'ok' if ok or kind == 'drc' else 'WRONG'})")
        known = {json.dumps(v, sort_keys=True) for v in before[0]}
        for v in after[0]:
            if json.dumps(v, sort_keys=True) not in known and v["type"] in added:
                print(f"  new {v['type']}: {v['description']} | " + " | ".join(i["description"] for i in v.get("items", [])))
        failed |= kind == "drc-refilled" and not ok
    return failed


# kicad-cli invocations that cc_board_native_fab_outputs reproduces (see board_native_c_api.h).
FAB_COMMANDS = [
    ["pcb", "export", "gerbers", "--board-plot-params"],
    ["pcb", "export", "drill", "--format", "excellon", "--drill-origin", "absolute", "--excellon-units", "mm",
     "--excellon-zeros-format", "decimal", "--excellon-separate-th"],
]
# Lines that carry the date, the KiCad build or the absolute path differ between runs and builds.
FAB_VOLATILE = re.compile(r"\d{4}-\d{2}-\d{2}|KiCad|Pcbnew|\"Version\"|CreationDate|GenerationSoftware")


def fab_digests(directory: Path) -> dict[str, str]:
    """Per output file, the SHA-256 of its LF-normalised lines without the volatile ones."""
    result = {}
    for path in sorted(directory.iterdir()):
        lines = path.read_bytes().replace(b"\r\n", b"\n").decode("utf-8", "replace").split("\n")
        kept = "\n".join(line for line in lines if not FAB_VOLATILE.search(line))
        result[path.name] = hashlib.sha256(kept.encode()).hexdigest()
    return result


def fab_reference(output: Path, boards: list[Path]) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "kicad_cli"))
    from bridge import find_cli
    cli, summary = find_cli(), {}
    for board in boards:
        with tempfile.TemporaryDirectory() as work:
            out = Path(work) / "out"
            out.mkdir()
            for command in FAB_COMMANDS:
                subprocess.run([cli, *command, "-o", str(out) + "/", str(board)], check=True, capture_output=True)
            subprocess.run([cli, "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both",
                            "-o", str(out / f"{board.stem}-pos.csv"), str(board)], check=True, capture_output=True)
            summary[board.name] = {"sha256": digest(board), "files": fab_digests(out)}
        print(board.name, len(summary[board.name]["files"]), "files")
    output.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")


def fab(references: Path, program: Path, boards: list[Path], keep: Path | None) -> int:
    expected, failed = json.loads(references.read_text(encoding="utf-8")), 0
    for board in boards:
        want = expected.get(board.name)
        if not want or want["sha256"] != digest(board):
            print(f"{board.name}: no reference for this board")
            failed = 1
            continue
        work = Path(tempfile.mkdtemp())
        out = (keep / board.stem) if keep else work / "out"
        run = subprocess.run([str(program), "fab", str(board), str(out)], capture_output=True, text=True)
        if run.returncode:
            print(f"{board.name}: fab failed {run.returncode}: {run.stderr.strip()[-400:]}")
            failed = 1
            continue
        got = fab_digests(out)
        differs = sorted(n for n in set(got) | set(want["files"]) if got.get(n) != want["files"].get(n))
        print(f"{board.name}: {len(got)} files, " + ("identical to kicad-cli" if not differs else
              "DIFFERS: " + ", ".join(f"{n} ({'missing' if n not in got else 'extra' if n not in want['files'] else 'content'})"
                                      for n in differs)))
        failed |= bool(differs)
        shutil.rmtree(work, ignore_errors=True)
    return failed


def plot(program: Path, boards: list[Path]) -> int:
    """PDF and SVG plots: a PDF with one page per layer and one parseable SVG per layer."""
    import xml.etree.ElementTree as ET
    failed = 0
    for board in boards:
        with tempfile.TemporaryDirectory() as work:
            run = subprocess.run([str(program), "plot", str(board), work], capture_output=True, text=True)
            pdfs, svgs = sorted(Path(work).glob("*.pdf")), sorted(Path(work).glob("*.svg"))
            pages = len(re.findall(rb"/Type\s*/Page[^s]", pdfs[0].read_bytes())) if len(pdfs) == 1 else -1
            try:
                for svg in svgs:
                    ET.parse(svg)
                parsed = True
            except ET.ParseError:
                parsed = False
            ok = run.returncode == 0 and pages == len(svgs) > 0 and parsed and pdfs[0].read_bytes()[:4] == b"%PDF"
            print(f"{board.name}: plot {'ok' if ok else 'WRONG'} ({len(svgs)} SVG layers, {pages} PDF pages)"
                  + (f" {run.stderr.strip()[-300:]}" if run.returncode else ""))
            failed |= not ok
    return failed


def setup(program: Path, board: Path) -> int:
    """Board Setup round trip on a copy of the board's project: four copper layers, a rule, a
    netclass and an assignment applied, then read back by a fresh process from the files and
    checked with KiCad's DRC."""
    with tempfile.TemporaryDirectory() as work:
        copy = Path(work) / "board"
        shutil.copytree(board.parent, copy)
        target = copy / board.name
        read = lambda: subprocess.run([str(program), "setup", str(target)], capture_output=True, text=True)
        before = read()
        if before.returncode:
            print(f"setup: read failed: {before.stderr.strip()}")
            return 1
        lines = [l for l in before.stdout.splitlines() if not l.startswith(("layers\t", "assign\t"))]
        lines = [l if not l.startswith("rule\tmin_clearance\t") else "rule\tmin_clearance\t0.15" for l in lines]
        lines += ["layers\t4", "class\tCorvPower\t0.3\t0.5\t0.9\t0.45\t0.25\t0.2", "assign\t*GND*\tCorvPower"]
        (Path(work) / "setup.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        applied = subprocess.run([str(program), "setup", str(target), str(Path(work) / "setup.txt")],
                                 capture_output=True, text=True)
        after = read()
        got = set(after.stdout.splitlines())
        # Then a dielectric of the new stack-up (a second apply, as the layer count came first).
        dielectric = next((l for l in after.stdout.splitlines() if l.startswith("dielectric\t0\t")), None)
        stacked = False
        if dielectric:
            fields = dielectric.split("\t")
            fields[3:6] = ["0.2", "FR408", "3.7"]
            (Path(work) / "setup2.txt").write_text(after.stdout.replace(dielectric, "\t".join(fields)), encoding="utf-8")
            second = subprocess.run([str(program), "setup", str(target), str(Path(work) / "setup2.txt")],
                                    capture_output=True, text=True)
            stacked = second.returncode == 0 and "\t".join(fields) in read().stdout.splitlines() \
                and "FR408" in target.read_text(encoding="utf-8")
        want = {"layers\t4", "rule\tmin_clearance\t0.15", "class\tCorvPower\t0.3\t0.5\t0.9\t0.45\t0.25\t0.2",
                "assign\t*GND*\tCorvPower"}
        drc = subprocess.run([str(program), str(target), work], capture_output=True, text=True)
        ok = not applied.returncode and not (want - got) and '"In1.Cu"' in target.read_text(encoding="utf-8") \
            and drc.returncode == 0 and stacked
        print(f"setup on {board.name}: {'ok' if ok else 'WRONG'}; missing after reload: {sorted(want - got)}; "
              f"dielectric 0 set to 0.2 mm FR408 Er 3.7 and read back: {stacked}"
              f"{' ' + applied.stderr.strip() if applied.returncode else ''}"
              f"{' DRC: ' + drc.stderr.strip()[-300:] if drc.returncode else ''}")
        return int(not ok)


def plot_summary(directory: Path) -> dict:
    """What a schematic plot must match kicad-cli on: PDF pages, SVG sheet count, and the
    drawn elements per SVG (stroke text is paths, so a missing field or wire shows)."""
    pdfs = sorted(directory.glob("*.pdf"))
    pdf = pdfs[0].read_bytes() if len(pdfs) == 1 else b""
    return {"pdf_pages": len(re.findall(rb"/Type\s*/Page[^s]", pdf)) if pdf[:4] == b"%PDF" else -1,
            "svg_elements": sorted(len(re.findall(r"<(?:path|polyline|polygon|circle|rect|text)\b",
                                                  svg.read_text(encoding="utf-8")))
                                   for svg in directory.glob("*.svg"))}


def bom_reference(output: Path, schematics: list[Path]) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "kicad_cli"))
    from bridge import find_cli
    cli, summary = find_cli(), {}
    for schematic in schematics:
        with tempfile.TemporaryDirectory() as work:
            out = Path(work) / "bom.csv"
            subprocess.run([cli, "sch", "export", "bom", "--group-by", "Value,Footprint", "-o", str(out), str(schematic)],
                           check=True, capture_output=True)
            plot = Path(work) / "plot"
            plot.mkdir()
            for kind in ("pdf", "svg"):
                subprocess.run([cli, "sch", "export", kind, "-o", str(plot / "schematic.pdf" if kind == "pdf" else plot),
                                str(schematic)], check=True, capture_output=True)
            summary[schematic.name] = {"sha256": digest(schematic),
                                       "bom": out.read_bytes().replace(b"\r\n", b"\n").decode("utf-8"),
                                       "plot": plot_summary(plot)}
        print(schematic.name, summary[schematic.name]["bom"].count("\n") - 1, "BOM rows",
              summary[schematic.name]["plot"])
    output.write_text(json.dumps(summary, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def schematic(program: Path, references: Path, projects: list[Path], keep: Path | None = None) -> int:
    """KiCad's schematic tools through the session on a copy of each project; its BOM is
    compared with kicad-cli's (bom_reference.json, made on Windows). With `keep`, each edited
    copy and the session's ERC and netlist of it are kept in keep/<project> for notes-parity."""
    expected, failed = json.loads(references.read_text(encoding="utf-8")), 0
    for project in projects:
        sch = project.with_suffix(".kicad_sch")
        want = expected.get(sch.name)
        with tempfile.TemporaryDirectory() as work:
            copy = Path(work) / "project"
            shutil.copytree(project.parent, copy)
            run = subprocess.run([str(program), "schematic", str(copy / project.name), work],
                                 capture_output=True, text=True)
            print(run.stdout.strip() or f"{project.name}: schematic tools failed {run.returncode}: {run.stderr.strip()[-400:]}")
            got = (Path(work) / "bom.csv").read_text(encoding="utf-8") if (Path(work) / "bom.csv").exists() else None
            same = bool(want) and want["sha256"] == digest(sch) and got == want["bom"]
            print(f"  BOM: {'identical to kicad-cli' if same else 'DIFFERS'} ({(got or '').count(chr(10)) - 1} rows)")
            if not same and got is not None:
                print("  native:\n" + got[:1500])
            plot = plot_summary(Path(work) / "plot")
            plotted = bool(want) and plot == want.get("plot")
            print(f"  plot: {'matches kicad-cli' if plotted else 'DIFFERS'} {plot} (kicad-cli {want and want.get('plot')})")
            failed |= bool(run.returncode) or not same or not plotted
            if keep:
                shutil.copytree(work, keep / project.stem, dirs_exist_ok=True)
    return failed


def edit(program: Path, keep: Path, boards: list[Path]) -> int:
    """KiCad's board edits through the session (cc_board_native_edit) on a copy of each board's
    project in keep/<board>: the first footprint flipped, the first track to 0.4 mm on B.Cu and
    an aligned dimension on Cmts.User. Checks the saved file; edit-parity then compares DRC."""
    failed = 0
    for board in boards:
        text = board.read_text(encoding="utf-8")
        footprint = next(blocks(text, "footprint"))
        reference = re.search(r'\(property "Reference" "([^"]+)"', footprint).group(1)
        side = re.search(r'\(layer "([FB])\.Cu"\)', footprint).group(1)
        segment = next(blocks(text, "segment"), None)
        track = re.search(r'\(uuid "([^"]+)"\)', segment).group(1) if segment else None
        target = keep / board.stem
        shutil.copytree(board.parent, target, dirs_exist_ok=True)
        copy = target / board.name
        requests = [f"flip\t{reference}", "dimension\tCmts.User\t0\t0\t20\t0\t-3"] + ([f"track\t{track}\t0.4\tB.Cu"] if track else [])
        zone = next((z for z in blocks(text, "zone") if re.search(r'\(layer "[^"]*\.Cu"\)', z) and "(teardrop" not in z), None)
        zone_id = re.search(r'\(uuid "([^"]+)"\)', zone).group(1) if zone else None
        # A library footprint placed, and a resistor (not the flipped one) changed to another.
        requests.append("place\tResistor_SMD:R_0805_2012Metric\tR900\t5\t5")
        before = footprints(text)
        swapped = next((ref for ref, (block, pads) in before.items()
                        if ref != reference and re.search(r'\(footprint "[^":]*:R_', block) and set(pads) == {"1", "2"}), None)
        if swapped:
            requests.append(f"swap\t{swapped}\tResistor_SMD:R_1206_3216Metric")
        if zone:
            net = re.search(r'\(net_name "([^"]*)"\)', zone)
            net_name = net.group(1) if net else ""
            requests.append(f"zone\t{zone_id}\t{net_name}\t3\t0.4\t0.3\tsolid\t0.5\t0.5\thatched")
        run = subprocess.run([str(program), "edit", str(copy), str(target), *requests], capture_output=True, text=True)
        edited = copy.read_text(encoding="utf-8") if run.returncode == 0 else ""
        flipped = next((b for b in blocks(edited, "footprint") if f'"Reference" "{reference}"' in b), "")
        moved = next((b for b in blocks(edited, "segment") if track and f'"{track}"' in b), "")
        zoned = next((b for b in blocks(edited, "zone") if zone_id and f'"{zone_id}"' in b), "")
        after = footprints(edited) if edited else {}
        checks = {"flip": f'(layer "{"B" if side == "F" else "F"}.Cu")' in flipped,
                  "place": "R900" in after and '(footprint "Resistor_SMD:R_0805_2012Metric"' in after["R900"][0],
                  "swap": swapped is not None and (swapped in after and '(footprint "Resistor_SMD:R_1206_3216Metric"' in after[swapped][0]
                                              and after[swapped][1] == before[swapped][1]),
                  "zone": zone_id is None or all(s in zoned for s in ("(priority 3)", "(connect_pads yes", "(clearance 0.4)",
                                                                       "(min_thickness 0.3)", "(mode hatch)")),
                  "dimension": "(dimension" in edited and "(height -3)" in edited,
                  "track": track is None or ("(width 0.4)" in moved and '(layer "B.Cu")' in moved)}
        print(f"{board.name}: {run.stdout.strip() or run.stderr.strip()[-300:]} {checks}")
        failed |= run.returncode != 0 or not all(checks.values())
    return failed


def edit_parity(kept: list[Path]) -> int:
    """On Windows: kicad-cli opens each board edited by `edit` and its DRC (as compare: type,
    severity, items) matches the session's drc.json; footprint-library checks set apart.
    Unconnected items are compared by count: a flipped footprint leaves its tracks dangling,
    and which of several equal ratsnest pairs KiCad names then follows the C++ library's sort
    order (macOS and Windows differ; on unedited boards the pairs match)."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "kicad_cli"))
    from bridge import find_cli
    cli, failed = find_cli(), 0
    for folder in kept:
        # The board the folder is named after (a demo folder can hold other boards).
        named = folder / f"{folder.name}.kicad_pcb"
        board = named if named.exists() else next(folder.glob("*.kicad_pcb"))
        with tempfile.TemporaryDirectory() as work:
            subprocess.run([cli, "pcb", "drc", "--format", "json", "--units", "mm", "-o", str(Path(work) / "drc.json"),
                            str(board)], check=True, capture_output=True)
            want = [i for i in drc_items(Path(work) / "drc.json") if i[1] not in LIBRARY_CHECKS]
        got = [i for i in drc_items(folder / "drc.json") if i[1] not in LIBRARY_CHECKS]
        unconnected = [sum(i[0] == "unconnected_items" for i in x) for x in (got, want)]
        got, want = ([i for i in x if i[0] != "unconnected_items"] for x in (got, want))
        same = got == want and unconnected[0] == unconnected[1]
        print(f"{board.name}: kicad-cli opened the edited board; DRC {'identical' if same else 'DIFFERS'} "
              f"({len(got)} session, {len(want)} kicad-cli; unconnected items {unconnected[0]} and {unconnected[1]})")
        for item in ([i for i in want if i not in got][:3] if not same else []):
            print("    only kicad-cli:", json.dumps(item)[:200])
        for item in ([i for i in got if i not in want][:3] if not same else []):
            print("    only session:", json.dumps(item)[:200])
        failed |= not same
    return failed


def import_project(program: Path, keep: Path, name: str, board: str, schematic: str) -> int:
    """KiCad's importers through the session into a new project keep/<name> (a .kicad_pro first, as
    the app makes one): the schematic (cc_sch_import) and the board (cc_board_native_import), "-"
    for none. The sources are copied to keep/<name>/source for import-parity on Windows."""
    target = keep / name
    (target / "source").mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).resolve().parents[3] / "engine" / "tests" / "fixtures" / "roundtrip" / "RoundTrip.kicad_pro"
    shutil.copy(fixture, target / f"{name}.kicad_pro")
    failed = 0
    for kind, source, extension in (("import-sch", schematic, ".kicad_sch"), ("import-board", board, ".kicad_pcb")):
        if source == "-":
            continue
        shutil.copy(source, target / "source" / Path(source).name)
        run = subprocess.run([str(program), kind, source, "auto", str(target / f"{name}{extension}")],
                             capture_output=True, text=True)
        print(f"{name}: {run.stdout.strip() or run.stderr.strip()[-300:]}")
        failed |= run.returncode != 0
    return failed


def import_parity(kept: list[Path]) -> int:
    """On Windows: each board imported by `import` against kicad-cli pcb import of the same file
    (footprint references, pad nets, and segment, via and zone counts; the session re-links
    footprints to the project's import library, which kicad-cli does not), then kicad-cli's DRC
    with --schematic-parity on the imported project when it has a schematic too."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "kicad_cli"))
    from bridge import find_cli
    cli, failed = find_cli(), 0
    for folder in kept:
        board = folder / f"{folder.name}.kicad_pcb"
        sources = [p for p in (folder / "source").iterdir()] if (folder / "source").exists() else []
        source = next((p for p in sources if board.exists() and p.suffix.lower() not in (".sch", ".schdoc") and "SCH_" not in p.name), None)
        if board.exists() and source:
            with tempfile.TemporaryDirectory() as work:
                reference = Path(work) / "reference.kicad_pcb"
                if subprocess.run([cli, "pcb", "import", "-o", str(reference), str(source)], capture_output=True).returncode:
                    print(f"{folder.name}: kicad-cli pcb import does not take {source.suffix} (no reference)")
                    source = None
                else:
                    want, got = reference.read_text(encoding="utf-8"), board.read_text(encoding="utf-8")
        if source:
            count = lambda text: {k: sum(1 for _ in blocks(text, k)) for k in ("segment", "via", "zone", "footprint")}
            nets = lambda text: {r: p for r, (_, p) in footprints(text).items()}
            same = count(got) == count(want) and nets(got) == nets(want)
            print(f"{folder.name}: board {'identical to' if same else 'DIFFERS from'} kicad-cli pcb import in footprints, "
                  f"pad nets and counts {count(got)}" + ("" if same else f" kicad-cli {count(want)}"))
            failed |= not same
        if board.exists() and (folder / f"{folder.name}.kicad_sch").exists():
            with tempfile.TemporaryDirectory() as work:
                run = subprocess.run([cli, "pcb", "drc", "--schematic-parity", "--format", "json", "--units", "mm",
                                      "-o", str(Path(work) / "drc.json"), str(board)], capture_output=True, text=True)
                if not (Path(work) / "drc.json").exists():
                    print(f"{folder.name}: kicad-cli could not load the imported board: {run.stdout.strip()[-200:]}")
                    failed = 1
                    continue
                parity = json.loads((Path(work) / "drc.json").read_text(encoding="utf-8")).get("schematic_parity", [])
            print(f"{folder.name}: schematic parity between the two imports: {len(parity)} items "
                  f"{dict(Counter(i['type'] for i in parity))}")
    return failed


def notes_parity(kept: list[Path]) -> int:
    """On Windows: kicad-cli's netlist (pin groups and names) and ERC (violation types) of each
    project edited by `schematic --keep` (API 6 tools, then API 8's no-connect, mirror and notes)
    against the session's erc.json and netlist.net written beside it."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "corvprobe"))
    from gate4 import desktop, erc_types, nets
    from bridge import find_cli
    cli, failed = find_cli(), 0
    for folder in kept:
        root = next((folder / "project").glob("*.kicad_pro")).with_suffix(".kicad_sch")
        with tempfile.TemporaryDirectory() as work:
            net, erc, _ = desktop(cli, root, Path(work))
        session_net = (folder / "netlist.net").read_text(encoding="utf-8")
        session_erc = (folder / "erc.json").read_text(encoding="utf-8")
        def checked(report):
            return {k: n for k, n in erc_types(report).items() if k not in ERC_LIBRARY_CHECKS}
        apart = {k: (erc_types(erc)[k], erc_types(session_erc)[k]) for k in ERC_LIBRARY_CHECKS
                 if erc_types(erc)[k] or erc_types(session_erc)[k]}
        same_net, same_erc = nets(net) == nets(session_net), checked(erc) == checked(session_erc)
        print(f"{folder.name}: netlist {'identical' if same_net else 'DIFFERS'} ({len(nets(net))} nets), "
              f"ERC {'identical' if same_erc else 'DIFFERS'} {checked(erc)}"
              + ("" if same_erc else f" session {checked(session_erc)}")
              + (f"; library checks apart (kicad-cli, session): {apart}" if apart else ""))
        failed |= not (same_net and same_erc)
    return failed


def projects(program: Path, a: Path, b: Path) -> int:
    """Two projects in one process, both orders: a project's library-table ERC findings must not
    depend on which project was open before it (KiCad's active project supplies the tables)."""
    counts = {}
    for first, second in ((a, b), (b, a)):
        run = subprocess.run([str(program), "projects", str(first), str(second)], capture_output=True, text=True)
        print(run.stdout.strip() or f"projects failed {run.returncode}: {run.stderr.strip()[-400:]}")
        found = re.search(r"findings (-?\d+) \(A\), (-?\d+) \(B after A\)", run.stdout)
        if run.returncode or not found:
            return 1
        counts[first.name, "first"], counts[second.name, "second"] = int(found[1]), int(found[2])
    same = all(counts[p.name, "first"] == counts[p.name, "second"] for p in (a, b))
    print(f"projects: each project's library findings {'independent of' if same else 'DEPEND ON'} the one before")
    return int(not same)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "projects":
        sys.exit(projects(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])))
    if len(sys.argv) >= 4 and sys.argv[1] == "bom-reference":
        sys.exit(bom_reference(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]]))
    if len(sys.argv) >= 5 and sys.argv[1] == "schematic":
        keep = Path(sys.argv[4][len("--keep="):]) if sys.argv[4].startswith("--keep=") else None
        sys.exit(schematic(Path(sys.argv[2]), Path(sys.argv[3]), [Path(p) for p in sys.argv[4 + bool(keep):]], keep))
    if len(sys.argv) >= 3 and sys.argv[1] == "notes-parity":
        sys.exit(notes_parity([Path(p) for p in sys.argv[2:]]))
    if len(sys.argv) >= 5 and sys.argv[1] == "edit":
        sys.exit(edit(Path(sys.argv[2]), Path(sys.argv[3]), [Path(p) for p in sys.argv[4:]]))
    if len(sys.argv) == 8 and sys.argv[1] == "swap-update":
        sys.exit(swap_update(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), sys.argv[5], sys.argv[6], Path(sys.argv[7])))
    if len(sys.argv) == 7 and sys.argv[1] == "import":
        sys.exit(import_project(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]))
    if len(sys.argv) >= 3 and sys.argv[1] == "import-parity":
        sys.exit(import_parity([Path(p) for p in sys.argv[2:]]))
    if len(sys.argv) >= 3 and sys.argv[1] == "edit-parity":
        sys.exit(edit_parity([Path(p) for p in sys.argv[2:]]))
    if len(sys.argv) >= 4 and sys.argv[1] == "plot":
        sys.exit(plot(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]]))
    if len(sys.argv) == 4 and sys.argv[1] == "setup":
        sys.exit(setup(Path(sys.argv[2]), Path(sys.argv[3])))
    if len(sys.argv) >= 4 and sys.argv[1] == "reference":
        reference(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
    elif len(sys.argv) >= 4 and sys.argv[1] == "fab-reference":
        fab_reference(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
    elif len(sys.argv) >= 5 and sys.argv[1] == "fab":
        keep = Path(sys.argv[4][len("--keep="):]) if sys.argv[4].startswith("--keep=") else None
        sys.exit(fab(Path(sys.argv[2]), Path(sys.argv[3]), [Path(p) for p in sys.argv[4 + bool(keep):]], keep))
    elif len(sys.argv) >= 5 and sys.argv[1] == "compare":
        sys.exit(compare(Path(sys.argv[2]), Path(sys.argv[3]), [Path(p) for p in sys.argv[4:]]))
    elif len(sys.argv) == 5 and sys.argv[1] == "ratsnest":
        sys.exit(ratsnest(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4])))
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "route":
        sys.exit(route(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4] if len(sys.argv) == 5 else None))
    elif len(sys.argv) == 6 and sys.argv[1] == "update":
        sys.exit(update(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), sys.argv[5]))
    else:
        sys.exit(__doc__)

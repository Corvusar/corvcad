"""Gate 4 measurements on a physical iPad through the CorvProbe tweak (desktop side).

  py -3 tools/corvprobe/gate4.py <ipad-ip> <token> <output.json> [project-folder ...]

Default projects are KiCad's own demos (interop-sized, hierarchical and the large Jetson board),
taken from the local KiCad 10 install. For each project it uploads the KiCad files to
Documents/Gate4/<name>/ on the iPad, asks the app (CORVCAD_DIAGNOSTICS build) to open the
schematic with its session backend and produce KiCad's netlist and ERC there, parses the board
with the app's board reader and, when the app links KiCad's board code, runs KiCad's DRC with
and without a zone refill. It compares the netlist (pin groups and names), the ERC violation
types and the DRC lists with kicad-cli on this desktop, and records memory, app size and fonts.
"""

from __future__ import annotations

import collections
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integrations" / "kicad_cli"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integrations" / "kicad_core" / "session"))
from bridge import find_cli  # noqa: E402
from board_check import LIBRARY_CHECKS, digest, drc_items, fab_digests, plot_summary  # noqa: E402

DEMOS = Path("C:/Program Files/KiCad/10.0/share/kicad/demos")
SESSION = Path(__file__).resolve().parents[2] / "integrations" / "kicad_core" / "session"
# kicad-cli 10.0.6's BOM, schematic plot summary and fabrication outputs for the demos.
BOM_REFERENCE = json.loads((SESSION / "bom_reference.json").read_text(encoding="utf-8"))
FAB_REFERENCE = json.loads((SESSION / "fab_reference.json").read_text(encoding="utf-8"))
DEFAULT = ["ecc83", "complex_hierarchy", "video", "jetson-agx-thor-baseboard"]


class Probe:
    def __init__(self, host: str, token: str):
        self.base, self.token = f"http://{host}:8765", token

    def call(self, route: str, data: bytes | None = None, timeout: float = 600, **query):
        query["token"] = self.token
        url = f"{self.base}{route}?{urllib.parse.urlencode(query)}"
        request = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
        return json.loads(body) if response.headers.get_content_type() == "application/json" else body

    def fetch(self, files: dict[str, int], target: Path) -> Path:
        """Downloads a diagnostics reply's files (paths under Documents) into target."""
        target.mkdir(parents=True, exist_ok=True)
        for rel in files:
            (target / Path(rel).name).write_bytes(self.call("/get", path=rel))
        return target


def nets(netlist: str) -> dict[frozenset[str], str]:
    """Pin groups -> net name from KiCad's s-expression netlist."""
    result = {}
    # kicad-cli writes one field per line, the session one line per node; accept any whitespace.
    for block in re.split(r"\(net\s+\(code\s", netlist)[1:]:
        name = re.search(r'\(name "((?:[^"\\]|\\.)*)"\)', block)
        pins = frozenset(f"{r}.{p}" for r, p in re.findall(r'\(node\s+\(ref "([^"]+)"\)\s+\(pin "([^"]+)"\)', block))
        if pins and name:
            result[pins] = name.group(1)
    return result


def erc_types(report: str) -> collections.Counter:
    data = json.loads(report)
    return collections.Counter(v["type"] for sheet in data.get("sheets", []) for v in sheet.get("violations", []))


def desktop(cli: str, schematic: Path, work: Path) -> tuple[str, str, float]:
    start = time.monotonic()
    subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadsexpr", "--output", str(work / "net"), str(schematic)],
                   check=True, capture_output=True)
    subprocess.run([cli, "sch", "erc", "--format", "json", "--severity-all", "--output", str(work / "erc.json"), str(schematic)],
                   check=True, capture_output=True)
    return (work / "net").read_text(encoding="utf-8"), (work / "erc.json").read_text(encoding="utf-8"), time.monotonic() - start


def main() -> int:
    host, token, output = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    folders = [Path(p) for p in sys.argv[4:]] or [DEMOS / name for name in DEFAULT]
    probe, cli = Probe(host, token), find_cli()
    keep = output.with_suffix("")  # device reports, beside the JSON
    keep.mkdir(parents=True, exist_ok=True)
    report = {"metrics_idle": probe.call("/metrics"), "fonts": probe.call("/fonts"), "projects": {}}
    print(json.dumps(report["metrics_idle"], indent=1))
    for folder in folders:
        project = next(folder.glob("*.kicad_pro"))
        name = project.stem
        # The project's own symbol and footprint libraries too: its tables refer to them.
        files = [p for p in folder.rglob("*") if p.suffix in (".kicad_pro", ".kicad_sch", ".kicad_pcb", ".kicad_dru",
                                                              ".kicad_sym", ".kicad_mod")
                 or p.name in ("sym-lib-table", "fp-lib-table")]
        uploaded = 0
        for path in files:
            rel = f"Gate4/{name}/{path.relative_to(folder).as_posix()}"
            uploaded += probe.call("/put", data=path.read_bytes(), path=rel)["bytes"]
        entry = {"files": len(files), "bytes": uploaded}
        entry["schematic"] = device = probe.call("/run", command="schematic", path=f"Gate4/{name}/{project.name}")
        board = folder / f"{name}.kicad_pcb"
        if board.is_file():
            entry["board"] = result = probe.call("/run", command="board", path=f"Gate4/{name}/{board.name}")
            # KiCad's DRC on the device (NATIVE_KICAD board code) against kicad-cli here.
            with tempfile.TemporaryDirectory() as work:
                for key, extra in (("drc", []), ("drc_refilled", ["--refill-zones"])):
                    if not result.get(key):
                        continue
                    here, there = Path(work) / f"{key}-desktop.json", Path(work) / f"{key}-device.json"
                    subprocess.run([cli, "pcb", "drc", "--format", "json", "--units", "mm", *extra, "-o", str(here), str(board)],
                                   check=True, capture_output=True)
                    there.write_text(result.pop(key), encoding="utf-8")
                    shutil.copy(there, keep / f"{name}-{key}-device.json")
                    shutil.copy(here, keep / f"{name}-{key}-desktop.json")
                    # Footprint-library checks depend on the installed libraries (board_check.py).
                    a, b = drc_items(here), drc_items(there)
                    lib = [sum(i[1] in LIBRARY_CHECKS for i in x) for x in (a, b)]
                    a, b = ([i for i in x if i[1] not in LIBRARY_CHECKS] for x in (a, b))
                    types = [collections.Counter(i[1] for i in x) for x in (a, b)]
                    result[f"{key}_compare"] = {"desktop": len(a), "device": len(b), "identical": a == b,
                                                "library_checks": {"desktop": lib[0], "device": lib[1]},
                                                "only_desktop": dict(types[0] - types[1]),
                                                "only_device": dict(types[1] - types[0])}
            if result.get("native_backend"):
                # Fabrication outputs against kicad-cli's (fab_reference.json), and the board plot.
                rel = f"Gate4/{name}/{board.name}"
                entry["fab"] = fab = probe.call("/run", command="fab", path=rel)
                want = FAB_REFERENCE.get(board.name)
                if fab.get("files") and want and want["sha256"] == digest(board):
                    got = fab_digests(probe.fetch(fab["files"], keep / f"{name}-fab"))
                    fab["differs"] = sorted(n for n in set(got) | set(want["files"]) if got.get(n) != want["files"].get(n))
                entry["board_plot"] = probe.call("/run", command="board-plot", path=rel)
                entry["board_setup"] = probe.call("/run", command="board-setup", path=rel)
        with tempfile.TemporaryDirectory() as work:
            netlist, erc, seconds = desktop(cli, project.with_suffix(".kicad_sch"), Path(work))
        entry["desktop_seconds"] = round(seconds, 2)
        if device.get("netlist"):
            here, there = nets(netlist), nets(device["netlist"])
            entry["netlist"] = {"desktop_nets": len(here), "device_nets": len(there),
                                "groups_equal": here.keys() == there.keys(),
                                "names_equal": here == there,
                                "differences": sorted(" ".join(sorted(g)) for g in here.keys() ^ there.keys())[:10]}
        if device.get("erc"):
            here, there = erc_types(erc), erc_types(device["erc"])
            entry["erc"] = {"desktop": sum(here.values()), "device": sum(there.values()),
                            "only_desktop": dict(here - there), "only_device": dict(there - here)}
        want = BOM_REFERENCE.get(project.with_suffix(".kicad_sch").name)
        if want and want["sha256"] == digest(project.with_suffix(".kicad_sch")):
            if device.get("bom") is not None:
                entry["bom_identical"] = device["bom"] == want["bom"]
            if device.get("files"):
                plot = plot_summary(probe.fetch(device["files"], keep / f"{name}-schplot"))
                entry["schematic_plot"] = {"device": plot, "desktop": want["plot"], "identical": plot == want["plot"]}
        for key in ("netlist", "erc", "bom"):
            if device.get(key):  # large texts: kept beside the report, not in it
                (keep / f"{name}-{key}-device.txt").write_text(device.pop(key), encoding="utf-8")
        report["projects"][name] = entry
        print(name, json.dumps({k: v for k, v in entry.items() if k != "schematic"}, indent=1),
              json.dumps(device, indent=1))
    report["metrics_end"] = probe.call("/metrics")
    report["timeline"] = probe.call("/timeline")
    output.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("report:", output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

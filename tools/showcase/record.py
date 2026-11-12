#!/usr/bin/env python3
"""Record one showcase take on the iMac's simulator (app/UITests/ShowcaseTests.swift).

  record.py <take e.g. testA_SchematicToBoard> <derived-data dir> <out dir> [--seed]

Boots "CorvCAD iPad Pro 11", installs the app from <derived-data>, optionally seeds the projects
(KiCad's ColdFire/Xilinx kit and rectifier demos), starts `simctl io recordVideo`, runs the take with
xcodebuild test-without-building, stops the recorder and writes <out>/<take>.mp4 plus
<out>/<take>.json: each SHOWCASE-MARK as seconds from the recording's start.
"""
import json, os, re, shutil, signal, subprocess, sys, time
from pathlib import Path

take, derived, out = sys.argv[1], Path(sys.argv[2]).expanduser(), Path(sys.argv[3]).expanduser()
out.mkdir(parents=True, exist_ok=True)
repo = Path.home() / "corvcad"
demos = Path.home() / "kicad-core/kicad/demos"
sim = re.search(r"CorvCAD iPad Pro 11 \(([0-9A-F-]{36})\)",
                subprocess.check_output(["xcrun", "simctl", "list", "devices"], text=True)).group(1)
subprocess.run(["xcrun", "simctl", "boot", sim], stderr=subprocess.DEVNULL)
subprocess.run(["xcrun", "simctl", "bootstatus", sim, "-b"], stdout=subprocess.DEVNULL, check=True)
app = derived / "Build/Products/Debug-iphonesimulator/CorvCAD.app"
subprocess.run(["xcrun", "simctl", "install", sim, str(app)], check=True)
data = Path(subprocess.check_output(["xcrun", "simctl", "get_app_container", sim, "org.corvcad.CorvCAD", "data"], text=True).strip())
projects = data / "Documents/Projects"
if "--seed" in sys.argv:
    for name in ("kit-dev-coldfire-xilinx_5213", "sim-rectifier"):
        shutil.rmtree(projects / name, ignore_errors=True)
    shutil.copytree(demos / "kit-dev-coldfire-xilinx_5213", projects / "kit-dev-coldfire-xilinx_5213")
    rect = projects / "sim-rectifier"
    shutil.copytree(demos / "simulation/rectifier", rect)
    (rect / "rectifier.kicad_pro").rename(rect / "sim-rectifier.kicad_pro")
    (rect / "rectifier.kicad_sch").rename(rect / "sim-rectifier.kicad_sch")
    shutil.copy(repo / "engine/tests/fixtures/sim-rectifier/sim-rectifier.kicad_pcb", rect)
    print("seeded", projects)
gerbers = os.environ.get("SHOWCASE_GERBERS")
if gerbers:  # Gerbers for the viewer take, from the KiCad Fabricate take's export
    shutil.rmtree(data / "Documents/Gerbers", ignore_errors=True)
    shutil.copytree(gerbers, data / "Documents/Gerbers")

video = out / f"{take}.mp4"
recorder = subprocess.Popen(["xcrun", "simctl", "io", sim, "recordVideo", "--codec=h264", "--force", str(video)],
                            stderr=subprocess.PIPE, text=True)
started = None
while started is None:
    line = recorder.stderr.readline()
    if not line:
        break
    if "Recording started" in line:
        started = time.time()
print("recording started", started)
log = out / f"{take}.log"
with open(log, "w") as f:
    test = subprocess.run(["xcodebuild", "test-without-building", "-project", str(repo / "app/CorvCAD.xcodeproj"),
                           "-scheme", "CorvCAD", "-destination", f"id={sim}", "-derivedDataPath", str(derived),
                           f"-only-testing:CorvCADUITests/ShowcaseTests/{take}"],
                          stdout=f, stderr=subprocess.STDOUT, env={**os.environ, "TEST_RUNNER_SHOWCASE": "1"})
time.sleep(1.5)
recorder.send_signal(signal.SIGINT)
recorder.wait(timeout=120)
marks = []
for line in open(log, errors="replace"):
    m = re.search(r"SHOWCASE-MARK (\S+) ([\d.]+)", line)
    if m and started:
        marks.append({"name": m[1], "t": round(float(m[2]) - started, 3)})
seen, unique = set(), []
for m in marks:  # xcodebuild echoes test output; keep the first of each
    if (m["name"], m["t"]) not in seen:
        seen.add((m["name"], m["t"])); unique.append(m)
viewports = {}
for line in open(log, errors="replace"):
    m = re.search(r"SHOWCASE-VIEWPORT (\S+) ([-\d. ]+)$", line.strip())
    if m:
        viewports.setdefault(m[1], [float(v) for v in m[2].split()])
json.dump({"take": take, "passed": test.returncode == 0, "marks": unique, "viewports": viewports},
          open(out / f"{take}.json", "w"), indent=1)
print("take", take, "exit", test.returncode, "marks", [(m["name"], m["t"]) for m in unique])
for line in open(log, errors="replace"):
    if re.search(r"error:|failed|XCTAssert", line):
        print(line.rstrip()[:300])

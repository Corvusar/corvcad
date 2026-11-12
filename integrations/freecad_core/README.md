# FreeCAD port groundwork

This directory contains the accepted **F1 desktop model proof** and the
completed **F2 desktop source-build/input audit**. Linux and macOS source builds,
model reruns, upstream suites and fresh dependency/content lock comparisons pass. This is not an embedded FreeCAD backend or an iOS port. The default engine and app do not import, build or link any of this code.
See [the port gates](../../docs/FREECAD_PORT.md).

## What is implemented

- `upstream.json` pins FreeCAD 1.1.4 to its full source commit and SHA-256 hashes
  of the official Windows portable archive and Linux AppImage.
- `fetch.py` verifies a stock archive or fetches a clean, unmodified source
  checkout into ignored `build/freecad-core/`. Existing mismatched inputs are
  rejected; no source checkout is reset or patched.
- `prepare.py` runs in **KiCad 10's Python**, loads a board through `pcbnew`, and
  exclusively creates a JSON input. Board outlines use polygon vertices rather
  than the bounding box of the Edge.Cuts stroke. Only rectangular, axis-aligned
  boards with one closed loop and no slots are accepted. Only circular NPTH pads
  in footprints whose library ID contains `MountingHole` become mounts. Other
  drills, such as the demo's TP1, are not treated as screw holes.
- `model.py` runs in **FreeCAD's interpreter**. A Spreadsheet drives fully
  constrained Sketcher profiles, a tray Pad and cavity Pocket, boss Pad, one
  plain through Hole per mount, and a separate lid Body/Pad. Pocket implements
  the shell while allowing independent wall and floor thickness. There are no
  face-index attachments, GUI imports or mesh-to-solid conversions.
- `run.py` launches `proof.py` through FreeCADCmd with isolated user/system
  settings and a fresh output directory. It rejects a nonzero process exit,
  missing success report, wrong input hash or altered artifacts. This matters:
  FreeCADCmd can return zero after an exception in a script.

The JSON boundary has schema `1`, units `mm`, board bounds `[xmin,ymin,xmax,ymax]`,
board thickness and mounting-hole coordinates/diameters. It records the input
board's byte hash and KiCad version. XY coordinates retain KiCad's values and Z
points up. This is a desktop experiment contract; the future mechanical C ABI
is still F4. Generation validates finite dimensions, boss separation and cavity
fit. A future session must validate **every** parameter edit; desktop users can
still enter invalid dimensions directly in FreeCAD.

The lid remains a plate at the tray top plus `lid_gap`, inset by that gap at its
edges, matching the fallback layout. It has **no fastening or locating features**.
Mounting bores use board drill diameters and pass through boss and floor. There
are no threads, inserts, seal, connector cutouts or component-height checks.
These are prototype geometry tests, not a manufacturing qualification.

## Reproduce on Windows

Use Python 3.10+ for the host tools. Do not mix KiCad's and FreeCAD's binary
Python modules in one process. From the repository root in PowerShell:

```powershell
python integrations/freecad_core/fetch.py windows-x86_64
& 'C:\Program Files\7-Zip\7z.exe' x build/freecad-core/FreeCAD_1.1.4-Windows-x86_64-py311.7z '-obuild/freecad-core/stock-1.1.4'
& 'C:\Program Files\KiCad\10.0\bin\python.exe' integrations/freecad_core/prepare.py engine/tests/fixtures/demo.kicad_pcb build/freecad-core/demo.json
& 'C:\Program Files\KiCad\10.0\bin\python.exe' integrations/freecad_core/prepare.py engine/tests/fixtures/roundtrip/RoundTrip.kicad_pcb build/freecad-core/roundtrip.json
$fc = 'build/freecad-core/stock-1.1.4/FreeCAD_1.1.4-Windows-x86_64-py311/bin/freecadcmd.exe'
python integrations/freecad_core/run.py --freecad $fc --input build/freecad-core/demo.json --output build/freecad-core/demo-proof
python integrations/freecad_core/run.py --freecad $fc --input build/freecad-core/roundtrip.json --output build/freecad-core/roundtrip-proof
python -m unittest discover -s integrations/freecad_core -p test_contract.py -v
& 'C:\Program Files\KiCad\10.0\bin\python.exe' -m unittest discover -s integrations/freecad_core -p test_prepare.py -v
```

Input JSON files and proof output directories must be new. Use new names when
repeating a run. Each successful output contains `enclosure.FCStd`, combined
`enclosure.step`, separate `tray.step`/`lid.step`, `tray.stl`/`lid.stl`, a complete
`report.json` and `freecad.log`. Generated FCStd/STEP files may contain timestamps;
artifact hashes identify a run, not a promise of byte-identical exports.

## Linux workflow

The manual-only [FreeCAD F1 desktop model proof](../../.github/workflows/freecad-proof.yml)
workflow installs KiCad 10 from its official release PPA, runs the regression
tests, verifies and extracts the pinned FreeCAD AppImage, then runs both boards
through its headless executable. The KiCad package can receive 10.x updates;
each input records the actual version. FreeCAD's binary and bundled dependencies
are fixed by the archive checksum. The launcher passes `freecadcmd` as AppRun's
first argument, following [the upstream launcher](https://github.com/FreeCAD/FreeCAD-Bundle/blob/main/conda/linux/AppDir/AppRun).
Linux execution [passed on 2026-10-25](https://github.com/Corvusar/corvcad/actions/runs/40135562084)
at commit `d57a584f9a366f4329cd04b1271c3c4903acb7c7`, using KiCad 10.0.7.
Both Linux reports are retained in `evidence/linux-*.json`. It uploads
generated geometry and logs even when the proof fails; it does not publish an
app, commit results, or run on push.

## Checks and measured evidence

Measured on Windows on 2026-10-25 with official FreeCAD 1.1.4 at commit
`4fd3bf320d9566a27e60069fc8387448aaa3a094`, bundled OCCT 7.8.1 and KiCad 10.0.6:

| Board | Mounts | Tray volume (mm³) | Lid volume (mm³) |
| --- | ---: | ---: | ---: |
| `demo.kicad_pcb` | 1 | 7783.2424723701 | 4700.5 |
| `roundtrip/RoundTrip.kicad_pcb` | 2 | 17472.0849447402 | 12440.5 |

Both proofs check:

- One valid, closed solid per body; analytic volumes and bounding coordinates.
- Empty drill cylinders plus material probes around each bore in floor and boss.
- Saved FCStd reopen, touching/recomputing every object, fully constrained sketches.
- Changing and restoring each of eight enclosure controls, with the same numeric
  and bore checks after every recompute.
- STL reimport, solidity, no self-intersections/nonmanifold edges, two faces per
  welded edge, consistent winding, positive oriented volume, and agreement with
  the exact solid to 0.5% volume and 0.0001 mm bounding-coordinate tolerances.
- Combined STEP reimport with exactly two solids; independent tray/lid STEP
  reimports, volumes, bounds and bores. B-rep coordinate tolerance is 0.000001 mm;
  volume tolerance is max(0.00001 mm³, expected volume × 1e-8).
- Synthetic models without mounts and with mixed drill sizes, negative board
  origin and a mount at XY zero. The negative-origin case caught a spreadsheet
  quantity parsing issue: values now use explicit formula syntax.

The retained [Windows and Linux reports](evidence) record runtime versions, source/input
hashes, authoring-tool hashes, dimensions, mesh counts, parameter trials and
artifact hashes. The stock-runtime reports are F1 evidence; the separate F2
source-build reports are described below. Contract/mesh/runner, KiCad extraction,
source-preservation and inventory regressions run separately. No simulator or
device result is claimed.

## F2 desktop acceptance

The isolated source build has been measured on Linux and macOS in
[run 40182424954](https://github.com/Corvusar/corvcad/actions/runs/40182424954)
at repository commit `7676f80e6efb2345f9fa95ef5a4cf32e893616a5`:
673 translation-unit objects and all 12 target artifacts were produced on each
platform. Both F1 models passed against both source-built runtimes, as did 669
upstream tests per platform, with three explicit skips and no failures. The
declared second patch removes unused GUI imports, makes existing gear/polygon
App helpers load headlessly, and skips one Draft-only test. The other two skips
are upstream Sketcher secant and GUI mesh primitive-count cases.

Reports and runtime versions are retained in `evidence/f2-*.json`. The measured
[source snapshot](audit/README.md) records 1,339 source/header inputs, generated
file hashes, resources, link inspections and 14 source-tree notice files. Its
read-only verifier detects source, profile, patch and retained-notice changes;
the complete dependency/generator audit is described in the F2 handoff.

See [F2_SOURCE_BUILD.md](../../docs/history/FREECAD_F2_SOURCE_BUILD.md) and the [F2 handoff](../../docs/history/FREECAD_F2_HANDOFF.md) for the reviewed locks, generator inputs, licence catalogues and payload exclusions. Fresh Linux/macOS validation passed in [run 40189353513](https://github.com/Corvusar/corvcad/actions/runs/40189353513). All 55 local regressions pass. The catalogue distinguishes source grants from material/document metadata; 35 reserved-rights assets and two font-related files are excluded from future production payloads. F3 must still measure iOS static registration/linking; F4
adds the mechanical session/app edition, and F5 requires device validation.

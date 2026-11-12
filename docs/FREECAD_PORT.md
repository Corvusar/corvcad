# FreeCAD engine port: research and gates

Researched 2026-10-24; desktop proof added 2026-10-25 by Codex. This is an experimental port record, not a shipped feature. The isolated [F1 tooling](../integrations/freecad_core/README.md) now generates and validates a parametric enclosure in stock FreeCAD 1.1.4 on Windows and Linux. The pinned source has also been fetched and verified outside the tracked tree, and the isolated F2 headless source slice now passes both model proofs, upstream suites and reviewed input-lock comparisons on Linux and macOS. FreeCAD is not linked into the engine or app. The owner decided on 2026-10-24 that the mechanical side of CorvCAD (enclosure, bores, cutouts, lid, STEP) belongs to a FreeCAD engine port, the way the electronics side is moving to KiCad's own model ([KiCad core port](KICAD_CORE_PORT.md)). The current dependency-free C++ enclosure mesh stays as the fallback until a gate below replaces it.

## Why FreeCAD, and what "port" means here

The [OCCT on iPadOS notes](OPEN_CASCADE_IOS.md) show that Open CASCADE alone can give exact B-rep solids and STEP on iPad. FreeCAD adds what CorvCAD would otherwise have to rewrite on top of OCCT:

- **Parametric documents:** a feature tree that recomputes when a parameter changes, stored as `.FCStd`, which desktop FreeCAD opens.
- **Modelling features:** PartDesign (Pad, Pocket, Hole with thread standards, Fillet, Chamfer, Thickness/shell), Sketcher with its constraint solver, and Spreadsheet-driven parameters.
- **Exchange:** the Import module's STEP/IGES with names and colours, and Mesh for STL/3MF.

The port is **headless**: FreeCAD's App layer (`Base`, `App` and the App half of each module) runs inside the CorvCAD app. The iPad UI stays SwiftUI with CorvCAD's own preview. FreeCAD's Gui layer (Qt Widgets, Coin3D, Quarter, PySide) is not ported. This is the same split as the KiCad port: the upstream model and algorithms, the CorvCAD touch front end.

## Pinned input

| Input | Pin | Notes |
| --- | --- | --- |
| FreeCAD | tag `1.1.4`, commit `4fd3bf320d9566a27e60069fc8387448aaa3a094` (released 2026-09-28) | Latest stable. Upstream has moved to calendar versions; `26.3` is at release candidate (`26.3rc2`, 2026-10-18). Re-evaluate the pin when 26.3 is final. |

## Evidence: existing ports

| Port | What it proves | Limits |
| --- | --- | --- |
| [freecad-android](https://github.com/tzhazuma/freecad-android) (unofficial) | Upstream FreeCAD cross-compiled for arm64 Android with OCCT 7.9.3, CPython 3.13, Boost, Xerces-C, yaml-cpp and FreeType. Headless `FreeCADCmd` loads and runs 18 workbenches on a device; patches are kept as a series in `patches/`. | Loads modules as shared `.so` files, which iOS does not allow from arbitrary paths. GUI via gl4es; Fem, CAM, BIM and OpenSCAD disabled. Debug APK about 302 MB, of which about 220 MB is Python and workbench assets. |
| [freecad-web](https://github.com/Virtastic/freecad-web) (unofficial, LGPL-2.1-or-later) | FreeCAD 1.1.3 for wasm64 with Qt 6.11, OCCT 7.8.1 and CPython 3.13. Every Python extension is linked statically and registered in the interpreter's inittab before startup, because WebAssembly has no `dlopen`: the same constraint CorvCAD prefers on iOS. About 500 upstream unit tests pass (PartDesign, Part, Sketcher, Spreadsheet, Mesh, Materials, among others). A PartDesign pad measured 8262.4 mm³ against the analytic 8262.4. Subprocess calls are guarded rather than removed. [Porting notes](https://dev.to/virtastic/porting-freecad-to-wasm64-what-broke-after-the-compile-succeeded-5g52). | The patch series is 7,162 lines against FreeCAD, much of it browser and GUI specific. Its notes warn that a module missing from the inittab or the link "builds fine and fails to import". Precompiled bytecode cut `import Draft` from 1.6 s to 0.36 s, and startup time needs the same care on iPad. |
| Upstream | The FreeCAD project closed an iPad request as [not planned](https://github.com/FreeCAD/FreeCAD/issues/20292), citing the OpenGL GUI. | There is no upstream iOS support to rely on; CorvCAD would carry its own patch series, as it does for KiCad. |

No iOS port of FreeCAD, headless or otherwise, was found.

## What the headless slice needs

### Build switches

FreeCAD's CMake defaults enable the GUI and many modules (`cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake`, inspected at the 1.1.4 pin on 2026-10-25). The slice would use `BUILD_GUI=OFF` and turn off every module except the ones below. Actual cache names include `BUILD_MATERIAL`, `BUILD_PART_DESIGN`, `BUILD_MESH_PART` and `BUILD_FLAT_MESH`. Disable FEM and MeshPart to disable SMESH; FlatMesh and CAM independently enable pybind11. FreeCAD 1.0 had a reported configure failure with `BUILD_GUI=OFF` ([#18928](https://github.com/FreeCAD/FreeCAD/issues/18928), a translation macro called outside the GUI check). F2 source builds established that the retained LinguistTools and Spreadsheet/Draft configure adaptations are needed for this 1.1.4 profile; both are in the hashed patch series.

| Module | Why | Notes |
| --- | --- | --- |
| Base, App | Document, properties, expressions, recompute, `.FCStd` | Link QtCore (the GUI-free Qt parts: Core, Concurrent, Network, Xml), Xerces-C, Boost, zlib, fmt, and the embedded Python. |
| Part | B-rep shapes, booleans, STEP via OCCT, tessellation | 1.1.4's `Part/App` links `FreeCADApp`, `Materials`, OCCT and optional FreeType. |
| Materials | Required by Part | Uses yaml-cpp and ships material card resources. |
| PartDesign, Sketcher | Feature-based tray and lid, holes with thread standards, fillets, shells | Sketcher's planegcs solver is C++ and Eigen. |
| Spreadsheet | Named parameters (wall, gap, boss sizes) driving the model | Mirrors today's enclosure sliders. |
| Import, Mesh | STEP with names and colours; STL and 3MF | Skip `MeshPart`: it switches on SMESH (`FREECAD_USE_SMESH`) and is not needed when tessellation comes from Part/OCCT. |
| Measure (optional) | Dimension checks in tests | Can be added later. |

Excluded: all Gui directories, FEM (SMESH, Netgen, VTK, external solvers), CAM, BIM, Draft (pure Python, Pivy), OpenSCAD and Plot (subprocesses, matplotlib), TechDraw (later, with the drawings milestone), Assembly, Robot, Points, ReverseEngineering, Inspection, Web, Start, Help, AddonManager and Tux. Each exclusion removes dependencies; together they also avoid PySide, Coin3D, VTK, SMESH and Qt GUI entirely.

### iOS rules that shape the build

- **Python is mandatory.** FreeCAD's App initialises an embedded interpreter, and every module's `Init.py` runs at startup. CPython supports iOS officially from 3.13 ([PEP 730](https://peps.python.org/pep-0730/), [Using Python on iOS](https://docs.python.org/3/using/ios.html)); BeeWare's [Python-Apple-support](https://github.com/beeware/Python-apple-support) packages it. iOS has no Python executable, only embedding.
- **Native modules: static plus inittab.** Python's documented iOS mechanism needs one framework per binary extension inside `Frameworks/`. The simpler path, proven by the WebAssembly port, is to link FreeCAD's module libraries statically into the app and register each `PyInit_<Module>` with `PyImport_AppendInittab` before `Py_Initialize`. Both are legitimate; static linking also matches how the KiCad edition is linked. The gate should confirm FreeCAD's module loader (`Base::Interpreter().loadModule`) resolves inittab modules without patches.
- **No subprocesses and no JIT.** iOS apps cannot spawn processes. The selected modules do not need external tools; guard any remaining `QProcess` and `subprocess` call sites as the WebAssembly port did. Nothing here needs JIT.
- **Qt.** F3 builds Qt 6.11.1 Core, Concurrent, Network and Xml statically through the pinned vcpkg baseline; Linguist tools run on the host only. The command harness opts out of Qt's UIKit entry point and retains its own `main`. No Qt window is needed for document recompute.
- **Resources.** F3 stages the Python standard library, each module's `Init.py` and selected helpers, and material cards as read-only runtime resources. Bytecode is precompiled with checked hashes using the same-source host Python 3.13.7.
- **Memory and size.** The Android build is about 302 MB with all workbenches. A Part/PartDesign/Sketcher slice should be much smaller, but this is an estimate; gate F3 measures it.

### Dependencies for iOS

| Dependency | Source | Licence |
| --- | --- | --- |
| CPython 3.13.7 | Checksum-pinned upstream source, separate same-source host and static iOS builds | PSF-2.0 |
| Qt 6.11.1 Core, Concurrent, Network, Xml | Static vcpkg target builds; host-only Linguist tools | LGPL-3.0 (or commercial) |
| OCCT 7.8.1 | Checksum-pinned upstream source; measured 35-toolkit STEP/XCAF closure retains required presentation/data toolkits, excludes OpenGL/GLES drivers and viewers | LGPL-2.1 with the Open CASCADE exception |
| Boost, Xerces-C, yaml-cpp, fmt, zlib, FreeType, Eigen | vcpkg iOS triplets, as already used by `integrations/kicad_core/ios_deps.sh` | BSL-1.0, Apache-2.0, MIT, MIT, Zlib, FTL/GPL-2.0, MPL-2.0 |
| FreeCAD 1.1.4 headless slice + CorvCAD patches | Pinned tag plus separately hashed static and iOS patch profiles under `integrations/freecad_core/ios/` | LGPL-2.1-or-later, with some files under other compatible licences |

## How CorvCAD would use it

1. **A mechanical session behind the C ABI,** like the schematic session. `open(project) / set_parameter / recompute / tessellate / export_step / export_stl / save`. It is backed by a FreeCAD document `enclosure.FCStd` in the project folder, next to the KiCad files, replacing the parameters in `enclosure.corvcad.json`. The engine stays UI-free; the app only sees triangles, part IDs and errors.
2. **The enclosure as a parametric FreeCAD model,** generated once from the board outline and holes (from the KiCad board) and driven by a Spreadsheet. The tray is a PartDesign body: outline offset, pad, shell, standoff bosses with Hole features (real screw bores and thread standards), and connector cutouts as pockets. The lid is a separate body. Desktop FreeCAD can open and continue editing the same file.
3. **Preview from the same solids:** Part tessellation feeds SceneKit (later Metal), so preview, STL and STEP come from one exact model, as the OCCT notes require.
4. **An edition switch** (`NATIVE_FREECAD`, mirroring `NATIVE_KICAD`), so the default build keeps the dependency-free mesh path until the device gate passes.

The existing mechanical cards (watertight tray, screw bores and lid fastening, connector cutouts, multiple board loops, OCCT iOS gate) become features of this model rather than separate work on the mesh engine.

## Gates

Each gate ends with evidence in this file, the way the KiCad port does.

### F1 accepted 2026-10-25 (Windows and Linux)

Codex implemented the independent KiCad-to-JSON input adapter, pinned binary/source
fetcher, Spreadsheet/Sketcher/PartDesign model and regression harness under
[`integrations/freecad_core/`](../integrations/freecad_core/README.md). The official
Windows 1.1.4 portable archive's SHA-256 was verified; its runtime reports the
exact source pin and OCCT 7.8.1. KiCad 10.0.6 loaded both repository boards without
modifying them. The tray is an outer Pad hollowed by a Pocket, allowing floor and
wall to vary independently, with fused boss Pads and plain through Hole features.
The lid is a separate Body and Pad, with no fastening features.

Both fixture proofs passed valid-solid, analytic volume/bounds, bore position,
FCStd save/reopen/recompute, eight parameter change/restore, STL manifoldness and
STEP reimport checks. Combined STEP has two solids; separate STEP and STL files
are also checked. Synthetic cases cover no mounts, mixed drill sizes, negative
origins and a mount at XY zero. There are 15 dependency-free regression tests and
five tests using KiCad's loader. Detailed tolerances and the retained Windows
reports are linked from [the tooling guide](../integrations/freecad_core/README.md#checks-and-measured-evidence).

The manual-only [Linux workflow](../.github/workflows/freecad-proof.yml)
[passed](https://github.com/Corvusar/corvcad/actions/runs/40135562084) at commit
`d57a584f9a366f4329cd04b1271c3c4903acb7c7`. Both downloaded model reports passed,
using stock FreeCAD 1.1.4, OCCT 7.8.1 and KiCad 10.0.7. The reports are retained
in `integrations/freecad_core/evidence/linux-*.json`; F1 is closed. F2's headless source builds and both model reruns passed on Linux and macOS
in [run 40182424954](https://github.com/Corvusar/corvcad/actions/runs/40182424954),
along with 669 upstream tests per platform (three explicit skips, zero failures).
Both builds produced 673 objects and all 12 target artifacts. Hashed patches
adapt the GUI-coupled test/helper imports without weakening model assertions.
F2 desktop build/input-audit criteria are complete as of 2026-10-25. The retained
version/content locks, generator inputs and per-file licence records passed fresh
Linux/macOS verification in [run 40189353513](https://github.com/Corvusar/corvcad/actions/runs/40189353513). The
[F2 handoff](history/FREECAD_F2_HANDOFF.md) records dependency notices and mandatory
production exclusions for 35 reserved-rights assets and two font-related files.
See [the F2 record](history/FREECAD_F2_SOURCE_BUILD.md). No
iOS link, simulator or device result is implied.

### Gate acceptance criteria

F3 was claimed by **Codex** on 2026-10-25. Its separate
[`ios/` investigation](history/FREECAD_F3_IOS_PORT.md) starts with
static CPython embedding and arm64 simulator/device dependency builds on GitHub
macOS runners. It uses KiCad's vcpkg registry baseline and records actual package
ABIs to assess the owner's shared-library proposal; Eigen 3 is an explicit
exception. On 2026-10-26 full simulator build `6adf5ba69da373ce40d9741a` at
`3bfde68` passed FreeCAD static linking, all nine built-in imports and both
unchanged F1 proofs, including desktop geometry parity, STEP/STL and persistence
checks. The [simulator receipt](../integrations/freecad_core/ios/evidence/codemagic-simulator-accepted.json)
records a 91,861,192-byte executable, 123,428,595-byte staged runtime and
155,664,384-byte peak process RSS. These are simulator development measurements.
The expanded device inputs and final device FreeCAD compile/link/staging
passed in `6adf997fc5102ac14cbfaa4f`. Its [device receipt](../integrations/freecad_core/ios/evidence/codemagic-device-accepted.json)
records a 91,986,392-byte executable and 123,552,613-byte staged runtime with
verified arm64 IOS metadata and system-only dynamic links. **F3 accepted
2026-10-26.** The [F3 handoff](history/FREECAD_F3_HANDOFF.md) records evidence and the
separate F4 app/F5 physical-device gates. No physical iPad execution is implied.

- **F1, desktop model proof.** Using stock FreeCAD 1.1.4 headless (`FreeCADCmd`) on Linux CI, build the enclosure from a KiCad board as a PartDesign/Spreadsheet document: tray, bosses with bores, a separate lid. Export STEP and STL. Check that the solids are valid, that the STEP reimports with 2 parts, the bounding boxes, the bore positions and the STL manifoldness. Change a spreadsheet parameter and recompute. This fixes the model CorvCAD will generate, with no porting risk.
- **F2, headless slice build and inventory.** Build the pinned FreeCAD with `BUILD_GUI=OFF` and only the modules above on Linux and macOS. Record the compiled translation units, the dependency pins and the licences, as gate 1 did for KiCad. Run F1's script in this slice and upstream's unit tests for the included modules.
- **F3, iOS link.** Build the dependencies and the slice statically for the iOS simulator and device (arm64), register the modules in the inittab and run F1's script inside a simulator test executable. Compare the STEP, volumes and bounding boxes with desktop, and measure binary size, memory and recompute time.
- **F4, session C ABI and app edition.** Add the mechanical session API, the `NATIVE_FREECAD` edition, the preview from tessellation, and STEP/STL export from the app, with UI tests in the simulator.
- **F5, device validation.** Run on a physical iPad and open the exported files in desktop FreeCAD and a slicer. Only after F5 may the UI or README present STEP on iPad (`AGENTS.md`).

F4 was claimed by Codex on 2026-10-26. The optional mechanical C ABI, persistent
FCStd session, exact-shape preview/export adapter and native app edition are
implemented in source. Desktop regressions and both native C ABI fixtures pass
on the Intel iPad simulator, including exact exports, persistence and history.
Portrait/landscape native UI tests, the default-edition undo regression and the
arm64 device app compile/link pass. **F4 accepted 2026-10-28** for the optional
FreeCAD edition. Physical iPad validation and user-facing STEP remain F5;
combined KiCad/FreeCAD dependencies are tracked separately. See the
[F4 implementation and validation handoff](history/FREECAD_F4_HANDOFF.md).

## Licensing

FreeCAD is LGPL-2.1-or-later. Qt is LGPL-3.0 and OCCT is LGPL-2.1 with an exception. Statically linking LGPL code into an iOS app requires that users can relink: ship the object files or the complete corresponding source and build scripts, plus notices. LGPL-3.0 also asks for installation information. A sideloaded unsigned IPA that users can rebuild and re-sign is the intended route; this needs a proper review before any distribution. CorvCAD's own code stays MIT. If combined in the GPL-3.0 KiCad edition, all of these licences are compatible with GPL-3.0. Per the distribution decision recorded in the [KiCad port packaging section](KICAD_CORE_PORT.md), nothing is distributed until the owner's go.

## Open questions for the gates

- F2 established that 1.1.4 needs the retained configure and App test/helper patches for this headless profile; review their long-term upstream disposition.
- Does any selected module import Python packages that are not in the standard library (for example numpy) on its startup path?
- Do the App-side document `Init.py` scripts or the Materials loader assume a writable install directory or a user configuration folder? iOS gives an app its Documents and Library directories only.
- How large are the binary and the resident memory for a typical enclosure recompute?
- Pin 1.1.4 or wait for 26.3: compare the 26.3 changes to Part, PartDesign and App once it is final.

# F2 headless source build and input audit

F1 was accepted after [the Linux proof](https://github.com/Corvusar/corvcad/actions/runs/40135562084)
passed on 2026-10-25. This directory now contains a source-build profile and audit
for the same FreeCAD 1.1.4 pin. F2 desktop build/input-audit criteria are **complete as of 2026-10-25**. This is not app integration or native binary release approval.
Linux and macOS source builds, upstream suites and both F1 reruns passed in
[run 40182424954](https://github.com/Corvusar/corvcad/actions/runs/40182424954),
at repository commit `7676f80e6efb2345f9fa95ef5a4cf32e893616a5`. Each platform
produced 673 objects and all 12 target artifacts, and ran 669 upstream tests with
three skips and zero failures. The full retained locks subsequently passed fresh verification, builds and both model proofs in [run 40189353513](https://github.com/Corvusar/corvcad/actions/runs/40189353513).

## Selected build and adaptations

`slice.py configure` fetches or verifies the pinned source, verifies the hashes
in `slice.json`, applies the declared patch series, and generates CMake's code
model, toolchain and compile-command metadata. It refuses undeclared/partial
source edits instead of resetting a checkout. It accepts an already fully
patched checkout only when all edited paths and their content match the profile.

All workbench defaults found in the pinned build-options file are disabled,
then Material, Part, Sketcher, PartDesign, Spreadsheet, Import, Mesh and the App
test module are enabled. Base/App remain upstream core targets. `BUILD_GUI` is
off, as are FEM, MeshPart, FlatMesh, Assembly, CAM and the C++ developer-test
targets. The last are separate from the upstream Python App suites run here.
Release builds use `-O1 -DNDEBUG` for this initial compile proof; no performance
claim is made. No static build or iOS extension registration is attempted.

The first [patch](../../integrations/freecad_core/patches/README.md) guards Spreadsheet's Draft dependency check
behind `BUILD_GUI`, and finds Qt LinguistTools for the App translation resources
in a headless build. Both changes are based on pinned source inspection. Further
configure/compiler failures must be recorded and fixed explicitly. The second
patch makes the upstream App tests importable without Draft/Pivy/GUI packages:
one Draft-specific test is explicitly skipped, unused GUI imports are removed,
and the existing involute-gear helper is copied for headless use. A focused
local precheck that blocks those GUI imports passed 24 tests with one Draft
skip; the complete source-built suites subsequently passed on both platforms.
Besides that Draft case, upstream skips its Sketcher secant case and GUI-only
Mesh primitive-count case. Model and solver assertions were retained.

The current full Import App target links OCCT's visualization-related libraries
in `FindOCC.cmake`, even though no FreeCAD GUI sources are selected. The first
measurement retains that upstream link list. Removing it before measuring which
glTF/mesh exchange code needs it would hide a real dependency. The inventory
rejects FreeCADGui, Qt Gui/Widgets, Coin, Quarter and PySide runtime links, and
records OCCT links separately for the F3 minimization review.

## Run and inspect

From GitHub Actions, dispatch **FreeCAD F1 desktop model proof** on this branch
with **source_slice=true**. That selects the reusable manual F2 workflow instead
of the stock binary proof. The two OS jobs run independently. Locally, after
installing the dependencies listed in the workflow:

```sh
python3 integrations/freecad_core/slice.py configure -DPython3_EXECUTABLE=/usr/bin/python3
python3 integrations/freecad_core/verify_inventory.py --source build/freecad-core/FreeCAD
cmake --build build/freecad-core/headless --parallel 3
python3 integrations/freecad_core/inventory.py --source build/freecad-core/FreeCAD --build build/freecad-core/headless --output build/freecad-core/evidence --require-complete
python3 integrations/freecad_core/test_slice.py --build build/freecad-core/headless --output build/freecad-core/evidence
```

`test_slice.py` runs the core suites registered by the upstream Test App module,
plus Part, PartDesign, Sketcher, Spreadsheet,
Mesh and Materials App tests, requiring a nonempty successful result from each
suite. It then runs the unchanged F1 geometry proof against both accepted input
snapshots, whose board hashes must match the repository fixtures after newline
normalization. The snapshots in `fixtures/` were produced by KiCad 10.0.7 in the
successful Linux F1 run; they avoid loading KiCad and FreeCAD binary Python
modules into the same process. This pin has no Import App test suite, so the
independent F1 STEP export/reimport checks cover that path. A FreeCADCmd zero exit
without the expected success report is still a failure.

The harness has a measured stock Windows baseline: 669 upstream tests, two
upstream skips and zero failures. Its report is
`evidence/windows-upstream-baseline.json`; this validates the test orchestration,
not the F2 source build. Initial Linux configure passed with the declared patches;
the first builds exposed OCCT's `libtbb-dev` and `libfontconfig1-dev` link requirements. Initial macOS runs
exposed separate ICU/Qt component discovery paths. Dependency setup now handles
those paths explicitly and selects Homebrew's Eigen 3 branch, consistent with
the pinned upstream Pixi manifest's Eigen `<5` constraint.

The inventory distinguishes planned compile commands from produced objects
and linked target artifacts. Missing shared libraries remain a partial build
even if every object compiled. It checks both ELF sonames and macOS framework
names for GUI dependencies. It records source hashes and available SPDX declarations, compiler-found
inputs, generated files, copied resources, source licence/notice texts, and
runtime link inspection. Its raw SPDX scan marks missing declarations for review; the separate full licence catalogues now resolve them using retained file/component grants.
Artifacts retain configure/build/test logs even when a stage fails.

The apt/Homebrew package lists are **measured host snapshots**, not dependency
locks. The actual selected inputs now have reviewed version/content locks,
dependency notices, per-file source/asset classification and generator provenance
under `audit/`. Fresh captures must match them exactly; missing or upgraded
packages fail instead of silently changing the baseline.
The [retained snapshot and read-only verifier](../../integrations/freecad_core/audit/README.md) cover 1,339 shared
source/header inputs, generated output hashes, resources, link inspections and
14 source-tree notice files. The full lock includes 2,145/2,157 source/generator/resource inputs, 7,482/8,855
external input files and 129/27 package records on Linux/macOS. See the
[F2 handoff](FREECAD_F2_HANDOFF.md), including the production guard
for 37 excluded example/material/font files. OCCT versions were
7.6.3 on Linux and 7.9.3 on macOS. No inventory produced here substitutes for a
complete corresponding-source archive. No iOS or device result is implied.

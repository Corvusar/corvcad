# Mixed native C ABI smoke probe

`mixed.cpp` checks KiCad and FreeCAD in one process, through their public C ABIs.
It does not initialize `QCoreApplication` itself. Run it in **two fresh processes**
with `kicad-first` and `freecad-first`: this means C ABI initialization order,
since linked static-library initializers necessarily run before `main`.

The probe links the freshly rebuilt native KiCad core and FreeCAD adapter/module
archives, not default stubs. QtCore supplies JSON/SHA helpers already required
by FreeCAD. No Python/FreeCAD/KiCad C++ headers are included. The only engine
header dependency is the public C ABI. Default enclosure settings are copied
exactly from the current `cc_default_settings()` implementation into a commented
local constant, avoiding an unnecessary portable-engine adapter link.

The workspace compositor owns the raw link recipe. Its response file must force
load the fresh KiCad core and reviewed FreeCAD session/modules, then link each
shared raw dependency, OCCT/Python and wx archive once with the union system
framework/library flags. Do not use separately merged `*_deps.a` packages.
Both native backend strings/capabilities are checked at runtime, so a stub or
schematic-only replacement cannot silently pass.

For example, after the compositor supplies `mixed-link.rsp` and the shared
target prefix (replace the architecture for an arm64 simulator):

```sh
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
xcrun --sdk iphonesimulator clang++ -std=c++20 \
  -target x86_64-apple-ios17.0-simulator -isysroot "$SDK" \
  -I engine/include -I "$PREFIX/include/Qt6" -I "$PREFIX/include/Qt6/QtCore" \
  integrations/native_workspace/probe/mixed.cpp engine/src/schematic_route.cpp \
  @mixed-link.rsp \
  -Wl,-map,mixed-link.map -o mixed-probe
```

The response file must not introduce a Qt UIKit command-line entry point:
the program retains its own `main`, as the accepted FreeCAD command probes do.
Its link map and archive identities remain required compositor evidence.

Each invocation takes:

```text
mixed-probe ORDER FREECAD_RUNTIME SOURCE_PROJECT F1_INPUT_JSON FRESH_OUTPUT
```

Use `engine/tests/fixtures/roundtrip` with
`integrations/freecad_core/fixtures/roundtrip.json`. The output's parent must
exist, and its final directory must be new. Run iOS-simulator binaries through
an owned simulator, with fresh process-local KiCad configuration directories;
do not execute them as macOS binaries or reuse a user's configuration/project.
The runtime is the verified shared FreeCAD package's `FreeCADRuntime` directory.

Each process copies the complete original project into `output/project`, then:

- Initializes both actual native backends in the requested order, with netlist,
  ERC, DRC (without/with refill) and ratsnest calls on the same project.
- Keeps both sessions live and a schematic transaction open while FreeCAD
  changes a wall parameter and recomputes/tessellates exact tray/lid solids.
- Exercises both histories, a rejected mechanical edit, and repeated checks
  after the other engine runs. Snapshots compare supported content independent
  of item traversal order; native report texts are retained separately.
- Changes the PCB's actual `min_clearance` Board Setup rule through the native
  board ABI, recomputes FreeCAD while the PCB edit is live, and saves both returned
  board/project texts into a new complete `board-copy` with project libraries.
  It reopens that copy and checks every Board Setup field, recomputes FreeCAD
  again, then reopens the original PCB and verifies its original settings.
  The original PCB UUID multiset (24 UUID-bearing items in the current fixture)
  must survive native serialization and reopening, covering the outline,
  footprints, pads and properties without relying on their traversal order.
  KiCad may add missing mandatory footprint fields: stock 10.0.6 materializes
  five Datasheet and five Description fields in this fixture, adding ten UUIDs
  while retaining all original 24. Original UUIDs must remain a multiset subset,
  and the complete serialized set must stay identical after reopen/recompute.
  The ABI reloads unsaved board edits on the next board call, so it checks the
  edited setup after both returned files have been saved into the separate copy.
- Writes the schematic edit to a separate copy, reopens it and checks that its
  label and all supported items survived serialization. It saves/reopens the
  exact FCStd beside the original project files and exports separate STEP/STL.
- Compares every original KiCad input byte against both the original fixture
  and working project copy. The accepted F1 input uses the actual PCB byte hash,
  accounting for checkout line endings without modifying source data.

`mixed-report.json` is written only after those operations succeed. The runner
must also require process exit code zero: teardown/global-state failures after
the report are still failures. Recheck the report's source hashes against both
the original fixture and `output/project` after process exit as well, covering
any process-lifetime settings cleanup. Keep failed logs/output trees intact.
Also recheck `board_edit`'s board/project byte hashes under its `board-copy`
directory and the retained `board-setup-before.txt`/`board-setup-after.txt` hashes
after process exit. Those additional reports cover serialization of the PCB edit;
they do not assert a PCB undo capability that the board ABI does not expose.
The existing accepted KiCad and FreeCAD probes retain responsibility for broad
feature, numerical geometry and format parity regressions. This smaller probe
checks shared native runtime behavior; it does not accept the combined app,
physical iPad execution, public STEP availability or release packaging.
The current project fixture is `{}` and contains no unknown-field sentinel.
The edited-copy checks establish the tested settings and PCB item identities;
they do not establish preservation of unsupported fields inside edited files.

After linking, run the executable on the Mac with the owned simulator runner:

```sh
python3 integrations/native_workspace/probe/run.py \
  --binary /absolute/path/to/mixed-probe \
  --runtime /absolute/path/to/package/sim/FreeCADRuntime \
  --output build/native-workspace/mixed-simulator-001 \
  --architecture x86_64
```

The default source project and F1 input are the RoundTrip fixtures above;
`--source-project` and `--input` can select another matching accepted pair.
The output directory must be new and outside both input trees. The runner
checks Mach-O simulator metadata, chooses an installed compatible iPad runtime,
creates its own simulator, and runs both orders through `simctl spawn
--standalone`. Each process receives separate home, KiCad configuration and
temporary directories. It hashes every runtime file before execution and after
each process, checks all original project bytes and reported exports after
process exit, and requires both exit code zero and a passing native report.

`mixed-simulator.json` records both results and raw input/runtime hashes.
`simulator.json`, `simulator-cleanup.json`, per-order logs, configuration trees
and probe outputs remain available on failure. The runner attempts shutdown
and deletion only for the UUID created by this invocation; deletion failure or
any input mutation prevents acceptance. It never boots an existing simulator.
These portable runner checks use mocked simulator commands:

```sh
python3 -m unittest discover -s integrations/native_workspace/probe -p test_run.py -v
```

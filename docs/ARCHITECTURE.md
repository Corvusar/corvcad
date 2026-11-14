# Architecture

The engine owns file parsing, dimensions, mesh generation, and export. `engine.hpp`, `eda.hpp`, and `project.hpp` are the C++ APIs; `c_api.h` gives Swift and future platform bindings a C boundary. UI code never parses KiCad syntax or writes geometry itself. The portable project creator writes a named KiCad project with blank schematic and starter rectangular board. The iPad app chooses a writable location under Documents/Projects, lists local project folders, and loads the generated files through the same import paths used for external files. It stores enclosure control values in an app-specific `enclosure.corvcad.json` sidecar; KiCad ignores this file. It does not overwrite an existing project.

The parser reads an S-expression tree, extracts one `Edge.Cuts` outer loop, and lists footprint placement, mounting-hole drill coordinates, nets, tracks, vias and pads. Enclosure parameters use millimetres. The enclosure is a rectangular tray based on the board bounds, with a separate lid. Preview meshes distinguish floor, walls and rim; STL outputs are separate tray and lid files. The board preview is not included in either STL export.

The separate schematic subset reads one sheet's placed symbols, wires, junctions, and labels. UUID-targeted source edits move a symbol with its property anchors or change an existing Value/Footprint property. It preserves unknown expressions. `schematic_nets` computes one sheet's nets using KiCad 10's rules:
- items meet at shared points;
- a label joins a wire anywhere along it;
- a wire end or pin on a wire's interior joins it only through a junction;
- labels of the same name join;
- power symbols, and hidden power inputs, join by name.

It names nets as KiCad does. `integrations/kicad_cli/net_parity.py` compares it with `kicad-cli` on KiCad's demo projects in CI (the **Schematic net parity** workflow). It does not model buses, sheet pins, names across sheets, or project-wide synchronization. The iPad overview draws the CorvCAD two-pin body and pin ends; other imported symbols use abstract markers. See [the two-track KiCad plan](KICAD_CORE_PORT.md).

The small touch-authoring layer adds new top-level expressions to local project files. It embeds a two-pin symbol definition, writes wires and labels, and creates a two-pad board footprint or non-plated mounting hole. Project-local symbol and footprint libraries use `${KIPRJMOD}` paths for desktop portability. The SwiftUI canvases map taps to KiCad millimetre coordinates. A touch surface captures press location and live finger movement; the schematic view snaps to a 2.54 mm grid or nearby known electrical anchors and previews orthogonal routes. A hold opens direct actions. The C++ engine validates locations, rejects overlapping CorvCAD symbols, and edits selected source spans for deletion and renaming; the app uses atomic replacement for local project files. This is a narrow compatible writer, not a port of KiCad connectivity, pad-net assignment, ERC, DRC, or PCB routing.

STEP is an optional adapter compiled only when Open CASCADE is installed. The desktop KiCad CLI adapter performs authoritative DRC/ERC and fabrication output with an installed KiCad. FreeCAD, KiCad core, and slicers can later be added as independent adapters or interoperability routes without making their code a requirement for the current iPad app. Technical drawings can consume a future exact boundary representation and dimension model.

The experimental [FreeCAD F1 proof](../integrations/freecad_core/README.md) has a separate process boundary: installed KiCad 10's `pcbnew` produces a narrow, validated JSON input, then pinned stock FreeCAD 1.1.4 generates a Spreadsheet/Sketcher/PartDesign document. Its exact tray/lid solids produce both tessellated STL and STEP; no mesh is converted back to an exact shape. The two Python runtimes never share a process. Windows and Linux desktop proof results are retained; an isolated source-built headless slice now passes the model proofs and 669 upstream tests (three skips) on Linux and macOS. F2 now retains verified dependency/content locks, generator inputs and per-file licence evidence, including exclusions for restricted example assets. ~~The mechanical C ABI and app integration remain open; see the [F2 handoff](history/FREECAD_F2_HANDOFF.md). The app still uses the portable mesh path and JSON sidecar.~~ **UPDATE 2026-11-14:** the mechanical C ABI and the `NATIVE_FREECAD` app edition were accepted in F4 (below); the default edition still uses the portable mesh path and JSON sidecar. See the [F2 handoff](history/FREECAD_F2_HANDOFF.md).

The separate F3 development harness now statically links FreeCAD and executes
both unchanged F1 fixtures on an arm64 iPad simulator. It verifies nine built-in
modules, desktop geometry parity, STEP/STL exports, persistence and runtime
resource immutability, with measured size, RSS and recompute timings. Expanded
device dependencies and final device-target FreeCAD link/staging checks pass,
completing F3. The device executable has verified arm64 IOS metadata and only
system dynamic dependencies; it has not executed on a physical iPad.
This harness is separate from the app; the mechanical C ABI and app edition
~~remain F4, and physical iPad validation remains F5~~ (**UPDATE 2026-11-14:** F4 accepted
2026-10-28; FreeCAD in the app passed the iPad STEP gate on 2026-10-31). See the
[F3 evidence](history/FREECAD_F3_HANDOFF.md).

The optional F4 session adapter now passes both native C ABI fixtures on the
Intel iPad simulator, including transaction history, FCStd persistence and
exact STEP/STL exports. Portrait/landscape app UI checks and default-edition
undo/redo pass, and the arm64 device app links. F4 is accepted for the optional
edition. ~~Physical iPad validation remains F5;~~ **UPDATE 2026-11-14:** its exact STEP
passed on a physical iPad Air (M2) on 2026-10-31 and Fabricate offers it; the combined KiCad/FreeCAD edition
has a separate dependency-integration card. The default edition keeps its
portable mesh boundary. See the [F4 handoff](history/FREECAD_F4_HANDOFF.md).

The combined edition's [shared dependency integration](NATIVE_WORKSPACE.md)
uses a separate per-SDK prefix and supplemental compiler-input inventory. Its
combined C ABI probe passes both initialization orders on the x86_64 iPad
simulator, and the combined app's KiCad and FreeCAD UI tests pass there. Its
unsigned arm64 app links; ~~device execution and physical-iPad results remain open.~~ **UPDATE 2026-11-14:** a combined diagnostics IPA ran on a physical iPad Air (M2) on 2026-10-31 (KiCad parity and the STEP gate); its touch UI on a device remains open. The hidden
[F5 diagnostics](history/FREECAD_F5_PREP.md) collect exact exports and measurements in
fresh directories; simulator rehearsal cannot establish physical acceptance.

The mobile app compiles the C++ core into its binary. It does not interpret downloaded code or rely on JIT. OCCT already has iOS implementations; [our findings and integration gate](OPEN_CASCADE_IOS.md) describe how to bring an exact geometry backend into CorvCAD. A CorvCAD OCCT iOS build should be pinned, reproducible, license reviewed, and tested on device before the UI offers STEP there. **UPDATE 2026-11-14:** the FreeCAD edition's pinned OCCT 7.8.1 met this on 2026-10-31, and only that edition offers STEP.

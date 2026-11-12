# KiCad integration boundary

CorvCAD can read selected KiCad board data on iPad and now ships a desktop adapter that invokes the installed `kicad-cli` for authoritative DRC/ERC and fabrication exports. This is **not a port of KiCad's editing engine**. The iPad UI inspects imported nets, tracks, vias, pads, footprints and outlines. It can move one footprint by rewriting that footprint's top-level position in a copy of the original `.kicad_pcb` file, preserving all other source text. It does not reroute tracks or update copper zones; run KiCad DRC on the edited copy.

## Desktop bridge

With KiCad 10 available in `PATH` (or set `KICAD_CLI` to the executable):

```sh
python integrations/kicad_cli/bridge.py check board.kicad_pcb out/drc.json
python integrations/kicad_cli/bridge.py check schematic.kicad_sch out/erc.json
python integrations/kicad_cli/bridge.py verify board.kicad_pcb out/drc.json
python integrations/kicad_cli/bridge.py fabricate board.kicad_pcb out/fab
python integrations/kicad_cli/bridge.py schematic schematic.kicad_sch out/schematic
```

`check` writes a KiCad report and prints the counts. `verify` does the same but exits unsuccessfully when KiCad reports any violation or unconnected item. `fabricate` generates a KiCad DRC report, Gerbers, drill files, a position CSV, and a board STEP model. `schematic` generates an ERC report, native KiCad netlist, BOM CSV and PDF. Both export commands stop when KiCad reports an issue; pass `--allow-violations` only when you deliberately need diagnostic exports. Review manufacturing settings before ordering; the adapter does not change the source files. The repository's `engine/tests/fixtures/demo.kicad_pcb` is validated with KiCad 10.0.6 at zero DRC violations and zero unconnected items.

## On-device engine work

KiCad 10's [IPC API](https://dev-docs.kicad.org/en/apis-and-binding/ipc-api/for-addon-developers/index.html) controls a running KiCad GUI. The KiCad developers state that versions 9 and 10 do not expose a standalone library for loading/editing files, and KiCad 11 adds headless IPC support. This does **not** make an offline iPad port impossible: KiCad is native C++ code and can be adapted and compiled ahead of time. It means CorvCAD cannot simply link an officially supported standalone KiCad PCB/schematic library. An on-device port would need to isolate and build the relevant KiCad code and dependencies for iPadOS, replace platform-specific assumptions, define a supported headless execution path, maintain the fork or upstream changes, test file round trips, and satisfy licenses. No KiCad code is vendored or linked into this app today.

The existing C++ reader and new transactional schematic session are deliberately limited, independent format tools. The session traverses project-local sheet references and exposes a C ABI, but cannot calculate KiCad nets or run ERC. Its PCB overview omits zones, graphics, custom pad shapes, and full layer stackup. Its footprint move writer changes only the `(at ...)` expression and never serializes the entire board, which limits format loss; that narrow edit still needs KiCad DRC afterward. It does not replace KiCad DRC or routing. New electronics operations should be implemented through a KiCad-backed adapter where the platform can run it, with the on-device port tracked separately rather than described as complete.

The [two-track engine plan and source audit](KICAD_CORE_PORT.md) describes the new one-sheet schematic subset and the desktop adapter using KiCad's own `pcbnew` model. For a representative project to test and send back, follow the [KiCad test-project steps](KICAD_TEST_PROJECT.md).

With KiCad's bundled Python, the desktop reference adapter can inspect a board or compare the schematic's KiCad-exported XML netlist against KiCad's PCB model:

```powershell
& 'C:\Program Files\KiCad\10.0\bin\python.exe' integrations/kicad_native/bridge.py inspect board.kicad_pcb out/board.json
& 'C:\Program Files\KiCad\10.0\bin\python.exe' integrations/kicad_native/bridge.py audit board.kicad_pcb schematic.kicad_sch out/audit.json
```

`audit` reports missing/extra footprints, footprint-ID differences, missing pads, extra assigned board nets, and net-name differences on matching pins. A board-only mounting hole is informational. The audit exits unsuccessfully on the other differences; some old KiCad demos intentionally retain legacy footprint library IDs, which this reports. It does not replace ERC/DRC or prove that copper is routed.

KiCad code is predominantly GPL licensed. If CorvCAD later links or incorporates that code, the combined distribution will need to satisfy the applicable GPL terms. See KiCad's [source repository](https://github.com/KiCad/kicad-source-mirror) and license files before that integration.

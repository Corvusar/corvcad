# KiCad electronics engine: two tracks

Reviewed 2026-10-21. CorvCAD needs offline PCB and schematic editing on iPad while remaining compatible with KiCad projects. This work proceeds on two tracks with a common C ABI, and their status must remain distinct.

## Current implementation

- **Portable compatible core:** `engine/src/engine.cpp` reads a limited board subset and performs a source-preserving footprint position edit. `engine/src/eda.cpp` reads placed symbols, wires, buses, junctions, local/global/hierarchical labels, no-connect markers and child-sheet references. `engine/src/schematic_session.cpp` opens a project-local sheet hierarchy, gives each instance a UUID path, groups source-preserving edits into transactions, supports undo/redo, and saves copies or stages edited sheets with rollback on reported failures after checking for external changes. A crash between multi-file replacements still needs recovery design. Its C ABI is in `schematic_session_c_api.h` and reports `compatible-source-edit`. It does **not** calculate KiCad connectivity, ERC, or a netlist, and does not expose arbitrary library pins.
- **Actual KiCad internals, desktop reference:** `integrations/kicad_native/bridge.py` runs under KiCad 10's bundled `pcbnew` Python binding. It loads a board with KiCad's own PCB model, reports footprints/pads/tracks/vias/zones, and can save a moved-footprint copy. `integrations/kicad_cli/bridge.py` calls the official CLI for DRC/ERC and exports. Neither adapter runs on iPad or constitutes an iPad port.
- **Desktop source-built proof:** `integrations/kicad_core/probe/` builds KiCad 10.0.6 sources on macOS. It links headless executables that load, netlist and save schematics, and load and save boards, through KiCad's own code. With CorvCAD's patch series and headless replacements, the schematic slice links without any KiCad editor file. These are measurement harnesses, not part of the app; see "Gate 2 desktop proof" below. A desktop executable also implements the schematic session C ABI over KiCad's own model; see "Gate 3 desktop and simulator proof".
- **Ported KiCad source in the app:** none yet. The KiCad 10.0.6 source and complete 10.0.6 symbol libraries are pinned in `integrations/kicad_core/upstream.json`; `fetch.py` recreates the checkouts and packed library artifact. Neither is linked or bundled in the app. Do not describe the desktop binding, independent parser, or session ABI as a KiCad source port.

## KiCad 10.0.6 source audit and reproducible inputs

The compatibility baseline is official KiCad tag `10.0.6`, commit `caf7377e9cb6fa1535ec3596dcb8c99bf44a996e`. The separate symbol repository at its `10.0.6` tag is commit `7800d91437ce44e2ed0928f2ad31a287457b8a68`. Its official `tools/kicad_lib_pack.py` produced 223 packed libraries (244,495,253 bytes on this checkout); KiCad 10.0.6 successfully exported `Device:R` from that bundle. The checkout is under the task workspace's `work/` directory, outside the repository. Run `python integrations/kicad_core/fetch.py --output build/kicad-core` on an ordinary build host to recreate it. `--full-source` is reserved for a corresponding-source archive once binaries actually contain KiCad code.

KiCad 10 already has a non-UI read path in `eeschema/eeschema.cpp`: `SCH::readSchematicFromFile` loads through `SCH_IO`, constructs `SCHEMATIC`, restores symbol instances, updates multi-unit references and recalculates `CONNECTION_GRAPH`; `generateSchematicNetlist` then uses `NETLIST_EXPORTER_KICAD`. This is the concrete extraction target. It is not a standalone library: that function calls `Pgm().GetSettingsManager()`, and the underlying files use `PROJECT`, `SCH_EDIT_FRAME` types, wxWidgets, project services and other common targets. KiCad 10's `eeschema/CMakeLists.txt` builds `eeschema_kiface_objects` from editor and simulator files as well as model files. The source project requires C++20; CorvCAD's portable engine and app now also build as C++20, so a source-built slice needs no language bridge. Desktop and simulator source-built schematic and board executables now run ERC/DRC and match KiCad's tools. The native session C ABI is also proven on desktop and simulator; arm64 probe executables link. App packaging and physical-iPad validation remain open. A wx-free slice is not required: the selected wx iOS types and services are part of the measured dependency set below.

| KiCad 10 source seam | Direct obstacle found | Required extraction |
| --- | --- | --- |
| `eeschema/eeschema.cpp` read/netlist functions | Static functions inside the GUI module and `Pgm().GetSettingsManager()` | Move the model-only sequence to an adapter with explicit project and settings ownership. |
| `eeschema/sch_io/kicad_sexpr/` | The native file reader includes `sch_edit_frame.h` for `SYMBOL_ORIENTATION_T`; library cache uses wx filesystem/logging | Relocate the enum to a neutral header and retain only non-visual wx services. |
| `eeschema/connection_graph.cpp` | Includes frame, UI helpers, configuration singleton and thread pool | Separate the calculation path from display and editor callbacks. |
| `eeschema/erc/erc.cpp` | Includes frame, graphics, project settings, Kiway and simulation services | Pass an explicit headless report sink and project settings; compile the checks actually required by the proof. |
| `eeschema/CMakeLists.txt` | `eeschema_kiface_objects` aggregates GUI, simulator and model sources | Define a dedicated C++20 library from measured model translation units; do not link the full editor object target. |

The candidate list in `upstream.json` is **not** the measured closure. The probe writes each closure to `build/kicad-probe/*.closure.txt`; the 2026-10-18 results are checked in under `integrations/kicad_core/probe/`. They cover the unpatched schematic probe (`closure-10.0.6.txt`), the patched headless schematic slice (`closure-10.0.6-headless.txt`) and the board probe (`closure-10.0.6-board.txt`).

## Gate 1 inventory (verified 2026-10-21)

The [measured source inventory](../integrations/kicad_core/audit/README.md) now records the headless schematic (336 archive translation units plus the probe), board (415 plus the probe) and native session (336 plus three direct harness/route objects) profiles. It contains 1,585 hashed source, header, generator and build inputs, including 1,522 KiCad files, the complete eight-patch series and per-file license evidence. Linker maps establish which translation units load; Ninja compiler dependency records establish which vendored headers they use. The historical GUI closure remains a study artifact.

The dependency inventory records vcpkg's exact baseline, installed versions/features/ABIs, port recipe hashes and per-link archive inputs for simulator and device proofs. It also records wxWidgets' pinned archive and a complete comparison of its 10,158 regular extracted files with that archive. All 137 retained license/notice texts have checksums, including the official symbol-library notice and wx's bundled codecs. License exceptions and the distinction between MIT CorvCAD files and GPL-derived code are documented in `license-policy.json` and `THIRD_PARTY.md`.

`audit.py verify` replays the declared patch series in an isolated Git index, compares source content, and detects source, recipe, license and dependency-pin drift. It passes against a fresh Windows checkout as well as the Mac proof inputs. The full audit and refresh instructions are in [the gate 1 handoff](history/KICAD_GATE1_HANDOFF.md). This completes source provenance and dependency mapping; a complete corresponding-source release artifact, native app packaging and physical device validation retain their separate release gates.

## Gate 2 desktop proof (measured 2026-10-18)

Host: Intel iMac, macOS 26.3.1, Apple clang from Xcode 26.2, Homebrew wxWidgets 3.3.4, Boost, GLM, libgit2 1.9.7, Protobuf 36.2, unixODBC, zstd, Cairo, Pixman, FreeType, HarfBuzz, Fontconfig and CURL. KiCad source: 10.0.6 at `caf7377`. Reference: the official KiCad 10.0.7 macOS app (`kicad-cli` and its bundled `pcbnew` Python module).

- **Harness.** KiCad's top-level CMake needs OCC, ngspice, SWIG and the full editor, so `probe/CMakeLists.txt` builds its own object libraries from pinned sources:
  - About 1,030 schematic and common candidates.
  - About 690 board candidates, with pcbnew's include roots and the `PCBNEW` define.
  - The CorvCAD headless replacements.

  It generates the lexers, Lemon grammars, version header, OpenGL shader strings and KiCad API protobuf code the way KiCad does. It stubs only two headers: the OCC version and `ngspice/sharedspice.h`. With `-DNGSPICE_BUILD=<ngspice_ios.sh work directory>`, the ngspice header forwards to ngspice's own for `ngspice.cpp`, `spice_simulator.cpp` and the session, which are then compiled with `CORVCAD_NGSPICE_STATIC` (patch 0011). The native closure then includes KiCad's simulator, and the programs link `libngspice.a` (SPICE phase S2, `docs/NGSPICE_PROPOSAL.md`). Without it, the simulator stays out, as before. The OpenCASCADE STEP/U3D exporters are left out. `measure.sh` compiles with `-k 0` (failures are GUI, simulator, OCC or package-manager files the links never need), archives what compiled, and links each probe with `-dead_strip` and a linker map. `closure.py` replays archive resolution. Its `--why` option prints the chain that loads a file. `--edges` lists every reference from model code into editor code. `--cut` simulates a patch before writing it.
- **Schematic probe.** `probe.cpp` sets up KiCad the way the eeschema QA tests do (`PGM_BASE` subclass, `wxAppConsole`, `InitPgm( headless )`, settings registration; shared in `headless.hpp`) with a window-less `Kiface()`. It calls `EESCHEMA_HELPERS::LoadSchematic`, the loader `kicad-cli` uses. That loader rebuilds the hierarchy, repairs instances, loads variants, numbers pages, cleans up connectivity and recalculates `CONNECTION_GRAPH`. The probe then writes a netlist with `NETLIST_EXPORTER_KICAD` and saves the top-level sheet through `SCH_IO`. `compare.sh` checks the netlist against `kicad-cli sch export netlist` (whitespace normalised, header metadata dropped), and the ERC violation count of the original against the probe-saved file:

  | Project | Netlist vs `kicad-cli` | ERC (original vs probe-saved) |
  | --- | --- | --- |
  | KiCad's `demos/complex_hierarchy` (3 sheet instances, 52 nets) | identical | 40 = 40 |
  | CorvCAD's generated Demo (2 sheets, 5 nets) | identical | 5 = 5 |

  Load plus connectivity takes about 210–260 ms (Release, x86_64).
- **In-process ERC (2026-10-21).** The schematic probe now runs `ERC_TESTER` the way `kicad-cli`'s `JobSchErc` does: the project's ERC settings, the symbol library adapter, a drawing-sheet proxy, and errors and warnings. It writes `ERC_REPORT`'s JSON as `probe-erc.json`. `compare.sh` compares the violation lists (sheet, type, severity, item UUIDs and positions) with `kicad-cli sch erc --format json`.
  - **Footprint check skipped.** `kicad-cli` checks footprint links through the CvPcb module and the footprint library table, which are outside the schematic slice. The probe skips that one test, and `compare.sh` leaves those violations out and prints how many it dropped.
  - **Patch 0005.** 10.0.6 writes ERC JSON positions with the PCB unit scale, 100 times too small. KiCad 10.0.7 fixes that line, and patch 0005 backports the fix.
  - **Results.** The probe's lists are identical to `kicad-cli`'s, on desktop and in the `CorvCAD iPad Pro 11` simulator, for four schematics:
    - complex_hierarchy: 0 (plus 40 footprint-link warnings not compared);
    - corvcd-interop: 0;
    - gate 3's edited copies of those two: 6 and 8, from dangling wire, label and pin violations and undriven power pins.
  - **Cost.** ERC takes 1–10 ms. The headless closure grows from 322 to 335 translation units, still with no GUI unit: ERC itself, the symbol checker, and the drawing-sheet parser and draw items used for title-block variables. The iOS sweep compiles 338 of 338 units. The simulator probe is 32.8 MB.
- **Board probe.** `pcb_probe.cpp` calls `LoadBoard` and `SaveBoard` from `pcbnew_scripting_helpers.cpp`, the path `kicad-cli` uses. It writes the same JSON report as `integrations/kicad_native/bridge.py inspect`: every footprint with its position, rotation and pad nets, plus tracks, vias and the zone count. `compare_pcb.sh` checks three things:
  - the probe's report against KiCad's `pcbnew` module on the original board;
  - KiCad's report on the probe-saved board against the original;
  - `kicad-cli pcb drc` violation counts on both.

  All three hold on every board tested:

  | Board | Footprints | Tracks | Nets | Model and save | DRC (original vs probe-saved) | Load |
  | --- | ---: | ---: | ---: | --- | --- | ---: |
  | `complex_hierarchy` | 68 | 364 | 53 | identical | 0 = 0 | 90 ms |
  | `ecc83-pp` | 15 | 59 | 14 | identical | 2 = 2 | 19 ms |
  | `interf_u` | 25 | 815 | 174 | identical | 3 = 3 | 129 ms |
  | `jetson-agx-thor-baseboard` (81 MB) | 1,125 | 25,386 | 1,388 | identical | 578 = 578 | 6.7 s |
  | CorvCAD Demo | 3 | 0 | 1 | identical | 0 = 0 | 4 ms |

  The board closure was 1,085 translation units here. See "Decoupling the board slice" below for the 415-unit headless one.
- **In-process DRC (2026-10-21).** The board probe now runs `DRC_ENGINE` the way `kicad-cli`'s `JobExportDrc` does: the engine and project rules from `LoadBoard`, the footprint library adapter, a drawing-sheet proxy, and errors and warnings. It writes `DRC_REPORT`'s JSON as `probe-drc.json`. `kicad-cli` collects markers through `BOARD_COMMIT`, which needs a `TOOL_MANAGER`; the probe adds them to the board after the run instead. Schematic parity and zone refill are off in both, as `kicad-cli pcb drc` defaults. `compare_pcb.sh` compares the violations and unconnected items (type, severity, item UUIDs and positions) with `kicad-cli pcb drc --format json`.
  - **Static registration.** Each DRC test provider registers itself through a static object that nothing references, so a static-archive link drops it: only 2 of 28 providers were linked at first, and DRC silently found fewer violations (199 of 578 on the Jetson board). KiCad links every pcbnew object into its kiface. `measure.sh` now passes the provider objects to the linker directly. Any library build of the board slice for the app must do the same, for example with `-force_load` or an explicit reference to each provider.
  - **Results.** The lists are identical to `kicad-cli`'s on every board above: complex_hierarchy 0, ecc83-pp 2, interf_u 3, jetson-agx-thor-baseboard 578, and a CorvCAD-authored board 0. DRC takes 0.06–0.6 s on the small boards and 24 s on the Jetson board (Release, x86_64 iMac). The board closure grows to 1,112 translation units.
  - In the iOS simulator the DRC lists are identical too (see "Decoupling the board slice" below). It has not run on a physical iPad.
- **Decoupling the board slice (done on desktop, 2026-10-21).** `measure.sh` also links `corvcad_kicad_pcb_probe_headless`. It puts the board replacements (`headless/pcbnew/`, built with pcbnew's include roots) first, then the common replacements.
  - **The editor hook.** Nearly all of the 458 editor translation units came through `LoadBoard`. `pcbnew_scripting_helpers.cpp` keeps the running `PCB_EDIT_FRAME` in a static and calls it from a dozen scripting helpers. `headless/pcbnew/pcbnew_scripting_helpers.cpp` is that file without the frame: the static and every branch that used it are gone, and the rest is unchanged.
  - **Patch 0006.** It moves `PCB_TEXT::ShowSyntaxHelp()` to the text dialog, and `ZONE_SETTINGS::SetupLayersList()` with `LAYER_PROPERTIES_GRID_TABLE` to a new `zone_layer_properties_grid.cpp`.
  - **Without board generators** the closure is 413 translation units with no GUI unit, and links only wxBase, wx XML and wx core, like the schematic slice. Model, save round trip and in-process DRC lists match `kicad-cli` on complex_hierarchy, ecc83-pp, interf_u and the CorvCAD board.
  - **Board generators** (tuning patterns) register themselves through static objects, like the DRC providers. Once the editor no longer pulled `pcb_tuning_pattern.cpp` in, a board containing one failed to load ("Cannot create generated object of type 'tuning_pattern'"). `measure.sh` therefore links the generators as objects too. With it the Jetson board matches again (578 identical DRC items), but `pcb_tuning_pattern.cpp` brings the editor back (458 GUI units).
  - `closure.py --root` (it treats objects linked directly as roots) traced that to the pattern's interactive editing, which reaches `BOARD_COMMIT`, the push-and-shove router's `PNS_KICAD_IFACE` and `PCB_SELECTION_TOOL`. All of those calls sit in members the model never runs.
  - **Patch 0007** moves those members unchanged from `pcb_tuning_pattern.cpp` to a new `pcb_tuning_pattern_edit.cpp`: `TUNING_STATUS_VIEW_ITEM`, `CreateNew`, `EditStart` through `UpdateEditPoints`, `ViewDraw`, `ShowPropertiesDialog`, `GetPreviewItems` and `DRAWING_TOOL::PlaceTuningPattern`. Properties, outline and registration stay put. `EditStart` is the class's key function, so the vtable moves with it. `headless/pcbnew/pcb_tuning_pattern_edit.cpp` defines those virtuals as no-ops: a headless board only loads, saves and checks the pattern's stored copper. `measure.sh` links the generators' model objects but leaves `*_edit` objects in the archive. No `BOARD_COMMIT` replacement is needed.
  - **Result.** The headless board closure is 415 translation units (`closure-10.0.6-board.txt`, down from 1,112) with no GUI unit. It links only wxBase, wx XML and wx core. Model, save round trip and in-process DRC lists match `kicad-cli` on all five boards, including the Jetson board (578 items).
  - **iOS simulator compile.** `ios_sweep.sh` now also sweeps the headless board closure. The schematic, session and board closures together are 571 units, and all 571 compile for `x86_64-apple-ios17.0-simulator` against wx-iOS and the vcpkg dependencies. That took two more pieces:
    - **Patch 0008**, the board counterpart of 0002. Layer-name matching reads `wxPGChoices` by index instead of `wxPGChoiceEntry`. Two unused pybind11 includes go. `OGL_ATT_LIST` is guarded by `wxUSE_GLCANVAS`. The Specctra menu commands (`PCB_EDIT_FRAME::ExportSpecctraFile` and `::ImportSpecctraSession`) move to a new `specctra_frame.cpp`, so the DSN exporter and SES importer stay in the slice without the editor.
    - **iOS stand-ins** in `headless/ios/`: `pcb_base_frame.h`, `pcb_base_edit_frame.h`, `pcb_edit_frame.h` and `pcb_draw_panel_gal.h`, as for the schematic frames; `dialogs/dialog_color_picker.h`, which keeps only the custom colour types that the board adapter's settings use; and the frame-name defines in `eda_draw_frame.h`.
  - All 571 units also compile for `arm64-apple-ios17.0` (`IOS_TARGET=device`).
  - Patches 0001–0008 apply cleanly to a pristine 10.0.6 checkout, and the result matches the build tree.
  - **iOS link and simulator run.** `PROGRAM=pcb_probe ios_link.sh` links `corvcad_kicad_pcb_probe_ios` with 0 unresolved symbols: 28.7 MB for the simulator, and 26.8 MB arm64 for a device, which has not run. `ios_link.sh` now links each program from its own measured closure, because the sweep holds the union and objects keep their static initialisers. The board objects therefore pass to the linker directly and its DRC providers and generators register. The schematic probe and session still link and match as before. In the CorvCAD iPad Pro 11 simulator, `compare_pcb.sh` finds the model, the save round trip and the DRC lists identical to `kicad-cli` on all five boards. Timings and peak memory (`peakMB`, now printed by the board probe too), Release, x86_64 iMac:

    | Board | Load, desktop / simulator | DRC, desktop / simulator | Peak memory, desktop / simulator |
    |---|---|---|---|
    | complex_hierarchy | 89 / 97 ms | 0.64 / 0.52 s | 167 / 183 MB |
    | ecc83-pp | 18 / 23 ms | 0.48 / 0.35 s | 136 / 156 MB |
    | interf_u | 127 / 139 ms | 0.55 / 0.41 s | 162 / 178 MB |
    | jetson-agx-thor-baseboard | 6.5 / 6.7 s | 23.9 / 25.5 s | 1,040 / 1,050 MB |

    The Jetson board needs about 1 GB, which is close to the per-app memory limit on smaller iPads. A device run (gate 4) has to measure that limit before large boards are claimed.
- **Headless schematic slice.** The unpatched schematic probe linked 786 translation units, about 340 of them editor code (frames, tools, dialogs, widgets) that never runs. `closure.py --edges` traced them to about 40 model files reaching editor code. Two things remove all of them:
  - **`patches/0001-headless-schematic-slice.patch`**, applied by `fetch.py`. It keeps the desktop editor building, and the full probe still matches `kicad-cli`. The patch header lists its eight changes:
    - Items check the frame type instead of casting to `SCH_EDIT_FRAME`.
    - `SCHEMATIC_HOLDER` gains selection, schematic and connectivity hooks, used by `SCHEMATIC::CleanUp`, `LoadSchematic` and `ERC_TESTER`.
    - The loader drops its leaked frame-less `TOOL_MANAGER`.
    - Hypertext goes through a virtual frame call.
    - Editor-only functions move next to their dialogs.
    - Field validation rules leave the wx validator.
    - Legacy library loading reports through `PROGRESS_REPORTER` and wx logging.
    - The database library plugin gets its settings dialog from a factory.
  - **`headless/`**: CorvCAD replacements for five editor-bound files, linked ahead of the slice:
    - `ui_common.cpp`: text ellipsizing and the severity strings.
    - `confirm.cpp`: message boxes become wx log records.
    - `sch_commit.cpp`: applies staged changes without view, selection or undo.
    - `editor_services.cpp`: background-jobs and notifications bookkeeping, plus the two status-bar calls in `PGM_BASE`.

  The resulting `corvcad_kicad_probe_headless` links **322 translation units with no KiCad editor file**, and its netlist and ERC results are identical to the table above. Its re-saved files are byte-identical to the full probe's. (The count includes the wx and library layers below.)
- **Third-party libraries in the headless slice (measured 2026-10-18).** Python, libgit2, curl and unixODBC each entered through one edge: `PGM_BASE` constructs `SCRIPTING` and initialises curl, `SETTINGS_MANAGER` and `PROJECT` ask git for VCS text variables, and `SCH_IO_MGR::FindPlugin` builds the database and HTTP symbol-library plugins. Four more headless replacements cut them, and `patches/0004-unused-git-curl-includes.patch` drops unused `<git2.h>` and `<curl/curl.h>` includes:
  - `python_scripting.cpp`: `SCRIPTING` without an interpreter (headless `InitPgm` skips Python anyway).
  - `kicad_curl.cpp`: curl init is a no-op and HTTP requests fail, so an HTTP library loads no parts.
  - `text_eval_vcs.cpp`: VCS queries answer as upstream does outside a git repository.
  - `database_connection.cpp`: database libraries never connect, so they load no parts.

  On desktop the headless probe now links, besides wx, only zstd, FreeType, HarfBuzz, Protobuf and Abseil (Fontconfig until the Core Text replacement below). All are model dependencies: embedded files, fonts, and the IPC API types that items serialise to. `headless/ios/` has stand-ins for `python_scripting.h`, `kicad_curl/kicad_curl.h` and `git/project_git_utils.h`, so iOS builds need none of Python, curl or libgit2. wx 3.3 is accepted here, although KiCad 10 targets wx 3.2.

- **wxWidgets for iOS (measured 2026-10-18).** `integrations/kicad_core/wx_ios.sh` builds the pinned wxWidgets 3.3.4 for the iOS simulator (static, `--with-osx_iphone`). Its `osx/iphone/chkconf.h` turns off propgrid, AUI, GL canvas, menu bars, status bars, info bars, static boxes and bitmap buttons. `probe/ios_sweep.sh` compiles every file of the headless schematic slice for the simulator with the probe's own compile commands. It started at 130 of 310 failing; four things fix them:
  - **`patches/0002-wx-ios-schematic-slice.patch`**, applied after 0001, keeps the desktop editor building:
    - `property.h` falls back to `properties/pg_choices_fallback.h` (the `wxPGChoices` subset the property system uses, same semantics) when `wxUSE_PROPGRID` is 0.
    - AUI pane serialisation and pane defaults are guarded with `wxUSE_AUI`.
    - `gl_context_mgr.h` forward-declares the wx GL types.
    - `RC_TREE_MODEL` moves to `common/rc_tree_model.cpp`, and `STATUSBAR_REPORTER` joins the status bar.
    - `WarnUserIfOperatingSystemUnsupported` moves into `confirm.cpp`.
    - Git progress uses `PROGRESS_REPORTER` (its wx dialog was never set).
    - Model files drop editor includes (`sch_field.h` now forward-declares the Scintilla types).
    - Two `__WXOSX_IPHONE__` guards cover the editor-chooser file dialog and tooltips, which the iOS port declares but does not implement.
  - **`patches/0003-ios-kiplatform-environment.patch`**: the Apple KIPLATFORM environment file imports Foundation instead of Cocoa on iOS and skips the one `NSWindow` call. Its trash, cache-directory and CFNetwork proxy code use APIs that iOS also has.
  - **`headless/ios/`**: stand-ins for the editor frame, draw panel, status bar and navigate-tool headers. They are used only by iOS builds and are listed below. Model files need complete frame types for their message-panel methods, but the real frame headers are wxFrame/AUI code. Each stand-in keeps upstream's include guard, base classes, constants and non-GUI includes. No frame is ever created headless.
  - **`headless/gl_context_mgr.cpp`** and, on desktop only, **`headless/wx_property_choices.cpp`** (the six `wxPGChoices` functions and `wxAuiPaneInfo::IsValid`, copied from wxWidgets) let the desktop headless probe link without the propgrid, AUI and GL libraries.

  **Result:**
  - All 310 files compile for iOS, and all 304 after the library trim above.
  - The iOS objects reference no wx symbol that the iOS wx libraries lack.
  - On desktop, the headless probe links only `wx_baseu`, `wx_baseu_xml` and `wx_core`. The core use is image, colour and bitmap value types, plus a few menu, key-event and validator helpers, all present in the iOS port.
  - Netlist and ERC parity with `kicad-cli` is unchanged.

- **iOS simulator link and run (gate 4, measured 2026-10-18).**
  - `integrations/kicad_core/ios_deps.sh` builds the slice's third-party libraries for iOS with vcpkg. The release tag and port list are pinned in `upstream.json`: zstd, FreeType, HarfBuzz, Protobuf with Abseil, GLM and the Boost libraries the slice uses.
  - Its overlay triplets (`integrations/kicad_core/triplets/`) target the iOS 17 simulator, use the SDK's libiconv, and give host tools an explicit SDK sysroot. Without that sysroot, Intel Homebrew headers in `/usr/local/include` (a newer Abseil) break the host `protoc`. vcpkg's autotools ports also need Homebrew `autoconf autoconf-archive automake libtool`.
  - Given that prefix, `ios_sweep.sh` drops every desktop include path, regenerates the IPC API code with vcpkg's `protoc`, and compiles the 322-file closure plus the probe's `main` for the simulator.
  - `probe/ios_link.sh` links them into `corvcad_kicad_probe_ios`: a 32.5 MB x86_64 simulator executable with 0 unresolved symbols. It also writes a `.sim` wrapper that runs the executable in a booted simulator through `simctl spawn`, so `compare.sh` drives it unchanged.
  - **Result in the `CorvCAD iPad Pro 11` simulator:**
    - complex_hierarchy has 3 sheets and 52 nets; its netlist is identical to `kicad-cli` and ERC is 40/40. Demo has 5/5.
    - The re-saved sheets are byte-identical to the desktop headless probe's, and the netlist differs only in its date line.
    - Warm load takes about 230 ms, the same as desktop; the first cold run took 1.35 s.
    - Peak resident memory (`peakMB`, from `getrusage`, measured 2026-10-19) is 162 MB for complex_hierarchy and 156 MB for Demo in the simulator. The desktop headless probe peaks at 143 MB and 137 MB. That is a whole-process peak, including wx, font and library initialisation. Simulator memory is not iPad memory.
  - **Fonts (2026-10-19).** Default KiCad text uses the stroke font compiled into the code, so no font files are bundled. Fontconfig was reached in two places: the schematic parser lists the installed fonts on every load (for the editor's font picker), and text that names an outline (TrueType) font is looked up by name.
    - `headless/fontconfig.cpp` replaces KiCad's `common/font/fontconfig.cpp` with the same `FONTCONFIG` interface over Core Text. On Apple platforms it resolves names to the system font files: the face index inside a `.ttc` comes from FreeType, by PostScript name. Unknown names get Core Text's fallback (Helvetica), reported as a substitution. `headless/ios/font/fontconfig.h` drops the `<fontconfig/fontconfig.h>` include, so neither Fontconfig nor Expat is built or linked for iOS. `upstream.json` no longer lists the port.
    - The font list now holds family names only, not per-language names or per-face files. Only the desktop font picker reads it.
    - Measured: setting `CORVCAD_PROBE_FONTS` makes the probe resolve names through KiCad's own `FONT::GetFont`. On desktop and in the simulator, Helvetica, Helvetica Bold (face 1 of `Helvetica.ttc`), Times New Roman and Menlo load as outline fonts from the system font folders. The simulator no longer prints fontconfig's warning.
    - complex_hierarchy load time fell from about 237 ms to 132 ms on desktop and from about 230 ms to 83 ms in the simulator, with the netlist and ERC unchanged. The per-load font scan was most of the old time.
    - Whether the app sandbox on a physical iPad can read `/System/Library/Fonts` is untested.
  - **Fresh reproduction (2026-10-19).** Following the documented steps from an empty `build/fresh` reproduced every result above: 30 failed sources, closures of 803, 1,085 and 322 files, 323 of 323 iOS units, 0 unresolved symbols, and the same complex_hierarchy netlist and ERC (40/40) in the simulator and on desktop. That tree was configured without a build type, so it built unoptimised: the executable was 73 MB and loading took about twice as long. The probe's CMakeLists now defaults to Release.
  - **arm64 device build (2026-10-19).** `wx_ios.sh <dir> device`, `ios_deps.sh <dir> arm64-ios` (an overlay triplet with the same settings for `iphoneos`) and `IOS_TARGET=device` for `ios_sweep.sh` and `ios_link.sh` compile all 323 units for `arm64-apple-ios17.0`, with no source changes. They link `corvcad_kicad_probe_ios-device`: a 29.8 MB arm64 iOS executable (platform iOS, minimum 17.0) with 0 unresolved symbols. It has not run: that needs a signed app on a physical iPad.

  Not covered yet:
  - A physical-iPad run, including the system font lookup above.
  - Packaging the slice as a library for the app (gate 3).
  - Boost and GLM are header-only here, and the sweep still compiles generated sources from the desktop build tree.
ERC and DRC now run inside the probes (above). Still missing for gate 2's intent: a save comparison on a larger corpus. The probes re-save KiCad 9 files in the 10.0 format, as `kicad-cli` does.

Reproduce on the Mac:
1. `python3 integrations/kicad_core/fetch.py --output build/kicad-core --source-only`, which checks out 10.0.6 and applies the patches.
2. `cmake -G Ninja -S integrations/kicad_core/probe -B build/kicad-probe -DKICAD_SOURCE=build/kicad-core/kicad`. The scripts below drive Ninja directly, so the generator matters.
3. `bash integrations/kicad_core/probe/measure.sh build/kicad-probe`. A cold build takes about 1.5–2 hours on the iMac with 5 jobs. It prints each probe's closure and the headless GUI count.
4. `bash integrations/kicad_core/probe/compare.sh build/kicad-probe/corvcad_kicad_probe_headless <in.kicad_sch> <work-dir>` and `bash integrations/kicad_core/probe/compare_pcb.sh build/kicad-probe/corvcad_kicad_pcb_probe <in.kicad_pcb> <work-dir>`.
5. For iOS: `bash integrations/kicad_core/wx_ios.sh build/wx-ios` (about 20 minutes), then `bash integrations/kicad_core/probe/ios_sweep.sh build/kicad-probe build/wx-ios/build-sim`. It prints the failure count grouped by first error; per-file logs are in `build/kicad-probe/ios_sweep/`.
6. For the iOS simulator run: `bash integrations/kicad_core/ios_deps.sh build/ios-deps` (about 30 minutes cold), then `bash integrations/kicad_core/probe/ios_sweep.sh build/kicad-probe build/wx-ios/build-sim 6 build/ios-deps/installed/x64-ios-simulator`. Then `bash integrations/kicad_core/probe/ios_link.sh build/kicad-probe build/wx-ios/build-sim build/ios-deps/installed/x64-ios-simulator`, and `compare.sh` with `build/kicad-probe/corvcad_kicad_probe_ios.sim` as the probe.
7. For an arm64 device link: `bash integrations/kicad_core/wx_ios.sh build/wx-ios device` and `bash integrations/kicad_core/ios_deps.sh build/ios-deps arm64-ios`. Then run step 6's sweep and link with `IOS_TARGET=device`, `build/wx-ios/build-device` and `build/ios-deps/installed/arm64-ios`.
Local verification on 2026-10-18: a KiCad 10.0.6 top-level CMake configure with a C++20-capable compiler stopped at `Could NOT find GLM (missing: GLM_INCLUDE_DIR GLM_VERSION)`. This is the first missing dependency on this Windows host, not proof that GLM is the only one. A full checkout under the long Windows task path also encountered `Filename too long` in a demo footprint; the sparse `common/`, `eeschema/`, `include/` checkout remains usable. The portable CorvCAD engine and new session built and passed five tests. A source-preserving Value edit on the supplied interoperability project reopened in KiCad 10.0.6: both original and edited copies reported 21 ERC violations, and KiCad exported the edited netlist with the new value. That checks one compatible edit, **not** KiCad internals running inside CorvCAD.

The normal KiCad top-level build also requests Zlib, Zstd, CURL, Cairo, Pixman, libgit2, Boost, FreeType, HarfBuzz, Fontconfig, ngspice, OCC, Protobuf, SWIG, Python, multiple wx GUI components and OpenGL. The headless port must measure which of these its selected translation units truly need; inheriting the full editor target would defeat the intended iPad library boundary.

## Gate 3 desktop and simulator proof (measured 2026-10-21)

`integrations/kicad_core/session/kicad_session.cpp` implements the existing `cc_sch_session_*` C ABI (`engine/include/corvcad/schematic_session_c_api.h`) over KiCad 10.0.6's own `SCHEMATIC`. It reports the backend `kicad-10.0.6-native` with `TRANSACTIONS | HIERARCHY | NATIVE_KICAD | ERC | NETLIST` (ERC and netlist since 2026-10-21, API version 3).
- **Loading and saving.** It loads through kicad-cli's loader, as the probe does, and saves through KiCad's `SCH_IO` writer. `save` checks file times for external changes and writes only edited sheets. `save_copy` follows the compatible backend's rules: the target must be a new directory outside the project, and the copy is staged and then renamed.
- **Ids.** Snapshots use the compatible backend's JSON schema. Ids are the KiCad instance path plus the item UUID.
- **Edits.** Move, set property (not Reference), rename, delete (not sheets), add wire, add label, move label and rename label are KiCad item edits. Rename sets the reference of one sheet instance, so it also works on shared sheets, which the compatible backend refuses.
- **Placement and routing (2026-10-21).** `add_two_pin` places CorvCAD's two-pin part with the compatible backend's rules: refused on a shared sheet, at least 10.16 mm from any symbol, with the first `R<n>` that no sheet instance uses. The part is the sheet's cached `CorvCAD:TwoPin`, or KiCad's parse of the same definition the compatible backend embeds (`engine/include/corvcad/starter_parts.hpp`), so a project needs no CorvCAD library.

  `route` and `add_routed_wire` use the engine's route planner (`engine/src/schematic_route.cpp`, now shared by both backends). The native backend fills its obstacles from KiCad's model: every pin of every symbol and each symbol's body-and-pin-tips box. The compatible backend now does the same from each sheet's embedded `lib_symbols` (placed unit, rotation and mirror), so both plan the same routes around imported parts.
- **Undo.** Each transaction keeps a journal. A modified item swaps data with its clone (`SwapItemData`, as `SCH_COMMIT::Revert` does), and an added or removed item moves between its screen and the journal.
- **Connectivity, netlist and ERC.** After every commit, undo, redo and aborted transaction the session recalculates KiCad's connection graph through `SCHEMATIC::RecalculateConnections`, the editor's path after a commit. It runs without cleanup, because KiCad's cleanup merges wires and adds junctions outside the journal. It recalculates fully, not incrementally. API version 3 adds `cc_sch_session_netlist` (`NETLIST_EXPORTER_KICAD`) and `cc_sch_session_erc_json` (`ERC_TESTER` and `ERC_REPORT`, run as the schematic probe does, without the footprint-link check). ERC markers are cleared before and after each run. The compatible backend returns `CC_SCH_UNSUPPORTED` for both.
- **Not yet covered:**
  - Multi-file `save` has no rollback between renames.
  - It is an executable, not yet a library in the app, and it has not run on a physical iPad.

`session/abi_test.c` drives a backend through the C ABI alone. It is linked once against the engine (`build/abi_test_compat`) and once against the native session (`corvcad_kicad_session`, 0 unresolved; 320 translation units, 335 with ERC). It aborts a delete, commits a move, a Value change, a wire and a label, then undoes, redoes and saves a copy. `session/check.sh` compares the results.

Results:
- On complex_hierarchy and on a CorvCAD-authored project, abort and undo restore the first snapshot exactly in both backends, and redo restores the edited one.
- The unedited snapshots of the two backends are identical, except on complex_hierarchy's shared sheet. There the native backend reports each instance's reference (`#U0301`) and the compatible backend the file's single Reference text (`#U0201`).
- Every saved copy loads in `kicad-cli`. The native and compatible copies have the same ERC counts (46 and 8 before placement and routing were added to the test, 51 and 11 now) and net counts (53 and 5, now 55 and 7). The originals had 40 violations and 52 nets, and 0 violations and 3 nets.
- The extra violations are the test's own edits: the moved symbol's pins leave their wires, and the added wire and label are dangling.
- **Placement and routing.** In the committed transaction `abi_test.c` also places a two-pin part at the first free grid spot and routes a wire from its pin 2. Both backends place it at the same spot and plan the same route. Apart from UUIDs, their edited snapshots are identical on the CorvCAD-authored project; on complex_hierarchy they differ only in the shared sheet's references, as above. The in-process netlists and ERC lists below include these edits.
- **In-process netlist and ERC.** `abi_test.c` also saves the session's netlist and ERC report after the undo and after the redo. `check.sh` compares them with `kicad-cli` on the original project and on the native saved copy. The netlists are identical (ignoring header metadata), and so are the ERC violation lists: 0 after undo, 10 and 11 after redo. The edited copies have more nets than the originals (55 and 7 against 52 and 3), so the match shows the recalculation, not a stale graph. Before placement and routing were added, the same edits gave 6 and 8 violations.
- **Simulator.**
  - `ios_sweep.sh` now also compiles the session. `PROGRAM=session ios_link.sh` links `corvcad_kicad_session_ios`, an x86_64 simulator executable with 0 unresolved symbols.
  - With ERC, netlist, placement and routing it is 32.9 MB. The sweep compiles 339 of 339 units, including the engine's route planner, and the session's closure is 335.
  - Run through its `.sim` wrapper in `CorvCAD iPad Pro 11`, `check.sh` gives the same results as on desktop on both projects, including the netlist and ERC comparisons.

Reproduce: after step 3 of "Gate 2 desktop proof", build the engine (`cmake -S engine -B build/engine` and build), then `clang -Iengine/include -o build/abi_test_compat integrations/kicad_core/session/abi_test.c build/engine/libcorvcad_engine.a -lc++`. Then run `bash integrations/kicad_core/session/check.sh build/kicad-probe/corvcad_kicad_session build/abi_test_compat <project.kicad_pro> <work-dir>`. For the simulator, run step 6's sweep, then `PROGRAM=session` with the same `ios_link.sh` arguments, and pass `build/kicad-probe/corvcad_kicad_session_ios.sim` to `check.sh`.

## Gate 5: board code in the app (started 2026-10-24; desktop measured, iOS pending)

The first gate 5 operations are KiCad's board DRC and zone fill, packaged with the schematic session in the `NATIVE_KICAD` edition.

**Desktop result (CircleCI `m4pro.medium`, macOS arm64, 2026-10-24).**
- **Build.** The full candidate build plus every measured link (`measure.sh`) took 8 min 13 s with 6 jobs; on the Intel iMac it took 1.5–2 hours. The schematic (336), board (415) and session (336) closures came out identical to the iMac's. The native program (session plus board checks) links with 0 unresolved symbols. Its closure is 569 translation units: the session and board closures plus `pcbnew/zone_filler.cpp` and `common/kidialog.cpp`. The zone filler only opens a `KIDIALOG` (its refill prompts, with `aCheck` or a parent window), and `kicad_board.cpp` asks for neither. `KIDIALOG` derives from `wxRichMessageDialog`, which wxWidgets' iOS port lacks, so iOS builds use a stand-in `headless/ios/kidialog.h`: upstream's class over a window-less base whose `ShowModal` logs the message and answers Cancel. Linked as objects, as the app force-loads them (`native_link.sh`), it has no duplicate symbols: 27.1 MB.
- **Comparison with kicad-cli 10.0.6 on eight KiCad demo boards.**

  | Board | DRC, as loaded | DRC after refill | Filled areas | DRC / refill+DRC / fill |
  |---|---|---|---|---|
  | ecc83-pp | 2 = 2 | 2 = 2 | same (1 zone layer) | 0.5 / 0.8 / 0.3 s |
  | interf_u | 3 = 3 | 2 = 2 | same (1) | 0.5 / 1.0 / 0.5 s |
  | complex_hierarchy | 0 = 0 | 0 = 0 | differ (teardrops) | 0.6 / 1.3 / 0.8 s |
  | video | 129 = 129 | 129 = 129 | same (2) | 1.3 / 3.4 / 2.6 s |
  | StickHub | 0 = 0 | 0 = 0 | same (5) | 0.5 / 0.9 / 0.4 s |
  | CM5_MINIMA_3 | 171 = 171 | 171 = 171 | same (18) | 1.6 / 2.9 / 1.3 s |
  | RoyalBlue54L-Feather | 238 = 238, plus library checks (below) | 238 = 238, plus library checks | differ (teardrops) | 1.0 / 1.6 / 1.0 s |
  | jetson-agx-thor-baseboard | 578 = 578 | 222 = 222 | same (100) | 14.8 / 28.3 / 14.0 s |

  "=" means identical lists (type, severity, item UUIDs, positions). Times include loading each board.
- **Footprint-library checks.** On RoyalBlue54L-Feather, kicad-cli on Windows (with KiCad's footprint libraries installed) reports 43 `lib_footprint_mismatch` and 2 `lib_footprint_issues` warnings; the Mac, without libraries, reports 67 footprint-library items instead of 45 (305 items against 283). That is the environment, not the port. `board_check.py` counts these checks apart, and the other 238 items are identical as loaded and after the refill (job #10). The iPad has no footprint libraries either.
- **Teardrops (resolved 2026-10-24).** kicad-cli regenerates teardrops before refilling, through `BOARD_COMMIT`, editor code that patch 0007 keeps out of the slice. `headless/pcbnew/board_commit.cpp` now applies staged board changes without view, selection or undo, and `kicad_board.cpp` regenerates teardrops before every refill. On Codemagic (commit `ca144a1`), the filled areas match kicad-cli on all 8 boards, the two teardrop boards included. Teardrops are compared as a sorted set per layer, because a refill recreates them with new UUIDs. DRC stays identical.
- **Update PCB from Schematic (patch 0009).** `BOARD_NETLIST_UPDATER` runs without the editor. Its editor-frame calls moved unchanged to `board_netlist_updater_frame.cpp`, which `headless/pcbnew/` replaces, and footprints come from a loader. The native closure is 578 units with no editor code. `cc_board_native_update_from_netlist` drives it with the session's netlist. A footprint whose library ID changed was reported, not exchanged, until 2026-11-01; since then the headless exchange (below) replaces it. On Codemagic (commit `d57a584`), the check deleted U1 from ecc83 and ran the update with kicad-cli's netlist. U1 came back from the project library, and the pad nets of all 15 footprints were identical to the original board. Footprints whose schematic library is a global one this host lacks (`Resistor_THT`) are reported as not found.
- **Libraries load on demand (2026-10-24).** ERC loads the symbol libraries that its symbols reference, DRC the footprint libraries its footprints reference, and the updater each footprint's library. kicad-cli's `AsyncLoad` of every table library is no longer done. The DRC lists and fills stayed identical on all 8 boards. The iPad memory effect is still to be measured with `gate4.py`.
- **Ratsnest (board connectivity).** `cc_board_native_ratsnest` returns each net's unrouted edges after `RecalculateRatsnest`. The edge counts equal kicad-cli's unconnected items on all 8 boards, which are all fully routed. The pipeline next checks ecc83 with its tracks removed, where kicad-cli reports 14.
- **Library symbols.** Session API 5 places any `Library:Name` symbol. `fetch.py`'s `index.json` lets the app search all 22,860 official symbols without loading a library. The pipeline next checks a placement on ecc83 against KiCad's netlist.
- **Board session (2026-10-25).** The last board loaded stays open between native board calls (`openBoard` in `kicad_board.cpp`). It is reloaded when its file changes, except when the file now holds exactly the text the last edit returned. `board_test session` makes 6 calls with 2 loads on ecc83 and video, a refill after DRC on the same open board included (CircleCI job #5). The open board is never destroyed: freed during static destruction it outlived KiCad's globals and trapped at exit on the video board.
- **Interactive router (2026-10-25).** KiCad's push-and-shove router (`pcbnew/router`) runs headless on the open board.
  - **Patch 0010** moves `PNS_KICAD_IFACE` (the editor's interface: view, preview items, tools, selection, editor commit) and the view-only debug decorator out of `pns_kicad_iface.cpp` into a new `pns_kicad_iface_view.cpp`, unchanged, and drops the editor headers only they used. The native closure grows from 578 to 611 units: 33 router units, none of the view or tool ones.
  - **`kicad_board.cpp`** drives it as `ROUTER_TOOL` does (`TOOL_BASE::Reset`, `prepareInteractive`, `pickSingleItem`, `snapToItem`, `FixRoute`, `CommitRouting`, the layer switch after a via). `HEADLESS_ROUTER_IFACE` derives from `PNS_KICAD_IFACE_BASE`. It commits with `PNS_KICAD_IFACE`'s code, without groups and the selection tool, through the headless `BOARD_COMMIT`, and returns the preview as text.
  - **C ABI:** `cc_board_native_route_start`, `_move` (the preview), `_fix`, `_toggle_via`, `_finish` and `_cancel`, plus `cc_board_native_drag_start`. The modes are walk around, shove and highlight collisions.
  - **With the project's rules (Codemagic, commit `f110285`).** On ecc83 13 of 14 edges are routed; the 14th cannot reach its pad under the project's clearances and is left unrouted. A tap ends a route only when the route has reached the item; `FixRoute` would otherwise commit a dangling stub. A drag of a routed track and a length tuning (meanders added, 27.37 mm of a 100 mm target) leave connectivity unchanged, and KiCad's DRC after a refill has no new violation. StickHub's USB pair (`/U1D+`, `/U1D-`) is routed as a differential pair to the far pads, with no new violation and 2 unconnected items fewer.
  - **Route kinds:** a track, a differential pair (both nets, the pair's width and gap from its rules), and length tuning of an existing track as Tune Length, Tune Differential Pair Length and Tune Skew. Tuning takes a target and reports the tuned length and status in the preview. Dragging moves a track or via, its neighbours following. Tuning commits plain meandered tracks, not KiCad's editable tuning-pattern object, whose editor half patch 0007 keeps out.
  - **Check** (`board_check.py route`, CircleCI job #14, commit `315f0b1`): ecc83 with its tracks and vias removed. All 14 ratsnest edges are routed as a finger would route them: tap the pad, move, tap the other pad. One goes from B.Cu through a via to F.Cu. Unconnected items go from 14 to 0. KiCad's DRC after a zone refill has the same 2 violations as before routing. As loaded, it adds 2 clearance items between the new via and track and the GND zone's stale fill. Zones are fills the editor refills after edits, not router obstacles.
- **Fabrication, plot, Board Setup and schematic tools (2026-10-26; Codemagic native-check `6adf1794ab5e2ae28344a036`, commit `dc7b7a2`).**
  - **Fabrication outputs.** `cc_board_native_fab_outputs` calls KiCad's own writers as kicad-cli's job handlers do. Gerbers come from `StartPlotBoard` and `PlotBoardLayers` with the board's plot settings, plus `GERBER_JOBFILE_WRITER`. The drill files come from `EXCELLON_WRITER` (mm, decimal, absolute origin, PTH and NPTH in separate files), and the placement CSV from `PLACE_FILE_EXPORTER`.
  - **Fabrication check.** `board_check.py fab` compares the files with kicad-cli 10.0.6's (`session/fab_reference.json`, made on Windows), leaving out date and version lines. On ecc83, interf_u, video, complex_hierarchy, StickHub and CorvCAD's RoundTrip, all 111 files are identical.
  - **Plot.** `cc_board_native_plot` runs `PCB_PLOTTER` with the board's plot settings: one PDF with a page per layer, or one SVG per layer. It is checked for structure only (page and layer counts, SVGs that parse), because kicad-cli's pdf and svg defaults are not the board's plot settings.
  - **Native closure.** It grows from 611 to 630 units: the plotters, the drill, job and placement writers, `pcb_plotter.cpp` and `pcbplot.cpp`. It gains no GUI unit (`kidialog.cpp` was already in it).
  - **Board Setup.** `cc_board_native_board_setup` and `cc_board_native_apply_board_setup` cover the copper layer count, eight design rules, the netclasses and the pattern assignments. Apply returns the board text and the `.kicad_pro` text.
  - **Board Setup check.** `board_check.py setup` sets 4 layers, a rule, a netclass and an assignment on ecc83, reads them back in a fresh process from the written files, and runs DRC on the result.
  - **Stale project settings.** `SETTINGS_MANAGER::LoadProject` returns a project it already holds, settings included. A board reload therefore re-reads the open project's file first, so an undone Board Setup reaches DRC and the router.
  - **Schematic session API 6** (`CC_SCH_KICAD_TOOLS`). It adds annotation (the Annotate dialog's defaults, optionally renumbering all), buses and wire-to-bus entries, global and hierarchical labels, and new hierarchical sheets whose file is written on save. It also adds sheet pins imported from hierarchical labels and a BOM.
  - **BOM.** It follows `FIELDS_EDITOR_GRID_DATA_MODEL`'s grouping and `Export`. That class itself is the editor's grid table (`WX_GRID`), so it is not linked.
  - **Schematic check.** `board_check.py schematic` runs every tool on copies of ecc83, complex_hierarchy and video, then reopens each project from its files. The new sheet, its pin, the bus and the entry read back, and ERC runs. Renumbering keeps the distinct reference count (24, 117, 395) with none left unannotated. Each BOM is identical to `kicad-cli sch export bom --group-by Value,Footprint` (`session/bom_reference.json`).
- **Schematic plot (2026-10-30; iMac desktop probe).** Session API 7 `cc_sch_session_plot` (`CC_SCH_PLOT`) runs `SCH_PLOTTER` with `kicad-cli`'s defaults (`EESCHEMA_JOBS_HANDLER::JobExportPlot`): all sheets, the project's drawing sheet, the default colour theme, PDF with links and metadata.
  - **Closure.** The native closure grows from 630 to 632 units (`sch_plotter.cpp`, `sch_render_settings.cpp`); the board plot had already brought in the plotters. No GUI unit.
  - **Version string.** The probe writes `kicad_build_version.h` with KiCad's release version (`10.0.6`), as a source tarball build does. `git describe --dirty` on the patched checkout gave `10.0.6-dirty`, which the title block printed.
  - **Check.** `board_check.py schematic` plots ecc83, complex_hierarchy and video before editing them. PDF page counts and the drawn elements of every SVG sheet are identical to `kicad-cli` 10.0.6's (`session/bom_reference.json`, made on Windows); on ecc83 the SVG elements were also compared one by one.
- **No-connect flags, symbol mirror and notes (2026-10-31; iMac desktop probe, kicad-cli on Windows, simulator).** Session API 8 (`CC_SCH_KICAD_TOOLS`) adds KiCad's no-connect flag (`SCH_NO_CONNECT`), the editor's symbol mirror (`SetOrientation(SYM_MIRROR_X/Y)` with field autoplacement cleared, as `SCH_EDIT_TOOL::Mirror`), a text note (`SCH_TEXT`) and a note line (`SCH_LINE` on `LAYER_NOTES`). `board_check.py schematic` places a flag on a pin KiCad's ERC reports unconnected on the root sheet, mirrors a root symbol, adds the notes, saves and reopens. `notes-parity` then compares `kicad-cli` 10.0.6's netlist and ERC of each edited copy with the session's on ecc83, complex_hierarchy, video and the round-trip fixture. The netlists are identical, and the ERC violation types are identical except ERC's library checks, which depend on the libraries installed where ERC runs. Each project's `pin_not_connected` count dropped by one in both. No new closure units; the gate 1 hashes of the changed CorvCAD files were refreshed.
- **Library footprints placed and swapped (2026-11-01; iMac desktop probe, kicad-cli on Windows, simulator).** `cc_board_native_edit` gains `place` (a project or official library footprint through the project's footprint table, as `PCB_BASE_FRAME::loadFootprint`, with the reference given) and `swap` (KiCad's Change Footprint). Swap runs `headless/pcbnew/exchange_footprint.cpp`, a copy of `PCB_EDIT_FRAME::ExchangeFootprint` with its similarity matching for pads, drawings, zones, fields and groups. Its frame calls are replaced: the board is passed in, `PlaceFootprint` is reduced to the position, and the flip direction is KiCad's default. It runs with the header's default arguments, as Update PCB passes them. The native closure grows from 632 to 633 units, and the gate 1 inventory was recaptured. `board_check.py edit` places `R_0805_2012Metric` and changes a two-pad resistor to `R_1206_3216Metric` on ecc83, video and complex_hierarchy; the pad nets are kept, and `kicad-cli`'s DRC of the results is identical (unconnected items by count). Update PCB from Schematic uses it too (2026-11-01): patch 0009 no longer stops at a null frame, and the headless `BOARD_NETLIST_UPDATER_FRAME::ExchangeFootprint` runs the copy. `board_check.py swap-update` gives ecc83's R1 `Resistor_SMD:R_1206_3216Metric` in kicad-cli's netlist: R1 is exchanged, the pad nets of all 15 footprints stay identical, and the footprints whose netlist library differs from the board's (six on ecc83, R1 included) change with it, as KiCad's Update PCB does with Replace footprints on. `kicad-cli` reopens the result with identical DRC.
- **Stack-up and zone properties (2026-11-01; iMac desktop probe, kicad-cli on Windows, simulator).** Board Setup's text gains `dielectric` lines (index, core or prepreg, thickness, material, Er) from the board's stack-up, or KiCad's default for the layer count; applying them sets those items and the board thickness from the stack-up (`BuildBoardThicknessFromStackup`), and only a real change makes the stack-up the board's own (`m_HasStackup`). Dielectrics sent with a layer-count change are skipped, since the layer change rebuilds them. `cc_board_native_edit`'s `zone` request sets a copper zone's net, priority, clearance, minimum width, pad connection, thermal gap and spoke width, and fill mode; the app refills in the same undo step. Teardrop zones are refused, as KiCad makes them from its teardrop settings, and the reader marks them. `board_check.py setup` sets a dielectric to 0.2 mm FR408 Er 3.7 and reads it back after a reload, and `kicad-cli` reopens that board with identical DRC. `edit` now also edits a zone on ecc83, video and complex_hierarchy, and `edit-parity` finds `kicad-cli`'s DRC identical (unconnected items by count).
- **Board edits: flip, track width and layer, dimensions (2026-11-01; iMac desktop probe, kicad-cli on Windows, simulator).** `cc_board_native_edit` takes one tab-separated request on the open board and returns KiCad's saved text: `flip` (`FOOTPRINT::Flip`, left-right about the footprint, KiCad's default), `track` (a track or arc's width and copper layer) and `dimension` (`PCB_DIM_ALIGNED` with the board's dimension and layer defaults, as `DRAWING_TOOL::DrawDimension`). After an edit the open board is closed, so the next call loads the written file as `kicad-cli` does. The board reader draws dimensions as their extension lines, crossbar and text, and moves and deletes them. `board_check.py edit` applies all three to ecc83, video and complex_hierarchy and checks the saved files; `edit-parity` has `kicad-cli` 10.0.6 open each result, and its DRC is identical to the session's. Unconnected items are compared by count there (99 each on video): a flipped footprint leaves tracks dangling, and which of several equal ratsnest pairs KiCad names follows the C++ library's sort order, which differs between macOS and Windows.
- **Sheets, power and bus helpers (2026-11-01; iMac desktop probe, kicad-cli on Windows, simulator).** Session API 9 deletes a sheet through `delete_item` (the hierarchy is refreshed after each commit, undo and redo; the file stays, as in KiCad), resizes a sheet as the point editor does (`SCH_SHEET::Resize`, refused below `GetMinWidth`/`GetMinHeight`) and adds bus-to-bus entries (`SCH_BUS_BUS_ENTRY`; KiCad saves them as bus segments). The app adds Resize and Delete to the sheet hold menu, a wire/bus kind to the Entry tool and a power-rail menu (KiCad's `power` library) to the Part tool. `board_check.py schematic` resizes the probe's sheet, adds the entry, deletes the sheet, undoes and redoes it and reopens; `notes-parity` finds the netlists of ecc83, complex_hierarchy, video and the round-trip fixture identical to `kicad-cli`'s afterwards, and their ERC types identical (the no-connect left on the deleted sheet's pin is `no_connect_dangling` in both), ERC's library checks set apart.
- **Importers (2026-11-01; iMac desktop probe, kicad-cli on Windows, simulator).** `cc_board_native_import` runs the format's `PCB_IO` plugin with the project, lists nets and reconciles the importer's footprints into `<project>-import-fps` (`FOOTPRINT_IMPORT_RECONCILER`, now in the closure) as `PCB_EDIT_FRAME` imports, without its interactive repair actions (kicad-cli leaves them out too). Items an importer leaves on no layer (KiCad 10.0.6's Eagle import does, and kicad-cli then cannot reopen its own output) are moved to User.Comments, KiCad's Rescue answer. Session API 10 `cc_sch_import` follows `SCH_EDIT_FRAME::importFile` (`SCH_FOOTPRINT_FIELD_RECONCILER`, empty drawing sheet, global cleanup, every sheet saved). Multi-design projects (EasyEDA Pro) take the first board-and-schematic pair; there is no chooser yet. `board_check.py import` and `import-parity` check boards against `kicad-cli pcb import` (Altium HiFive1, Eagle AHT20, EasyEDA smart watch, PADS ClaySight: identical footprints, pad nets and counts) and run schematic-parity DRC on the imported pairs. CorvCAD's own board reader now takes the largest closed Edge.Cuts loop as the outline, so boards with slots or cutouts open.
- **Project switch (2026-10-31; iMac desktop probe and simulator rehearsal).** KiCad resolves every library (project tables, `${KIPRJMOD}`) through its one active project, and making another project active destroys the previous `PROJECT`. The adapters used to activate only the first project in a process, so every later project saw the first one's library tables: in the simulator probe rehearsal, complex_hierarchy's ERC gained 73 library findings when ecc83 had been open first. Now a project becomes active whenever no schematic session is open (the cached board is closed and the loaded projects unloaded first), and the app releases the previous project's session before opening the next. While another project's session is still open, a model loads beside it as before. `board_check.py projects` runs two projects in one process in both orders and fails when a project's library findings depend on the one before; it failed on the old behaviour (113 against 40).
- **PCB Draw tool (2026-10-26; Codemagic native-check `6adf53fbb44f189f146b99ac`).**
  - **Portable writers.** Zones, outline replacement and silkscreen and fab lines and text are written by the portable engine, like manual tracks and vias. KiCad's zone filler (`cc_board_native_fill_zones`) fills a new zone in the same undo step in this edition.
  - **Check.** `engine/tests/fixtures/drawn/Drawn.kicad_pcb` is what the drawing calls write on a starter board; an engine test fails if the two differ apart from UUIDs. In `compare` and `fab`, its DRC (before and after refill), its two zone fills and its 13 fab files are identical to `kicad-cli` 10.0.6's.
  - **Native closure.** It is unchanged: the recaptured inventory equals the committed one, in which only the `engine.hpp` hash was refreshed.
- **Project libraries (2026-10-27; Codemagic native-check `6adfb5b9c277c2cd2d988672`).**
  - **Portable library files.** The portable engine (`library.hpp`) lists, imports, creates and edits project libraries in both editions. Its edits patch a symbol's or footprint's KiCad text.
  - **Reload.** `cc_board_native_reload_libraries` calls `LIBRARY_MANAGER::ReloadTables(PROJECT)`, as KiCad's library table dialog does after a save. It rebuilds the open project's tables and drops the cached project libraries.
  - **Check.** `board_test library` takes the engine test's generated CorvLib, in a project whose tables lack it. Placing `CorvLib:Chip` fails. After the tables arrive and the reload runs, Chip and Conn place, KiCad's netlist carries their CorvLib footprints, and Update PCB adds both footprints with all their pads (8 and 4). DRC then runs on the result.
  - **Native closure.** It is unchanged: the recaptured inventory equals the committed one.
  - **Not run on an iPad yet.**

The design:
- **C ABI.** `engine/include/corvcad/board_native_c_api.h`: `cc_board_native_drc` (DRC as `kicad-cli pcb drc --format json`, optionally after `--refill-zones`) and `cc_board_native_fill_zones` (the refilled board's text; nothing is written). Each call loads the file through kicad-cli's loader and keeps nothing. The compatible edition's `engine/src/board_native_c_api.cpp` reports no backend, and the engine test checks that.
- **Implementation.** `integrations/kicad_core/session/kicad_board.cpp` runs DRC as the board probe does and fills with `ZONE_FILLER` as `ZONE_FILLER_TOOL::FillAllZones` does headless, except teardrops: that tool regenerates them first through a `TOOL_MANAGER`, so boards with teardrops may fill differently. It shares the program object, settings and `Kiface()` with `kicad_session.cpp`, which every native build links. Board code reads `Kiface().KifaceSettings()` in three places; with the headless module object those see no settings, as in the board probe.
- **Closure.** The zone filler is not in the measured board closure. `measure.sh` therefore links a fourth program, the session plus the board checks over both slices (`native.closure.txt`, audit profile `native`). `ios_sweep.sh` sweeps it and `ios_package.sh` packages it. `probe/native_link.sh` links that closure as objects, as the app force-loads it, so duplicate symbols between the schematic and board slices would show on desktop first.
- **Check.** `session/board_check.py reference` records kicad-cli 10.0.6's DRC lists without and with a refill, and per zone and layer the filled area and vertex count of the board it saves (`--save-board`), on eight KiCad demo boards (`session/board_reference.json`, made on Windows). `compare` runs `session/board_test.c` against them. CircleCI's manual `native-check` job runs the full build, `native_link.sh`, the comparison and a recapture of the inventory's sources.
- **Device.** The diagnostics build's `board` command also runs native DRC without and with a refill, and `tools/corvprobe/gate4.py` compares those lists with `kicad-cli`.
- **Not in the UI.** The app does not offer DRC or zone fill until they match `kicad-cli` on the corpus and on a physical iPad.

## Plan for the open blockers (proposal, 2026-10-18)

These are the intended resolutions for the obstacles above. Each becomes a recorded result only when its proof builds and is compared against KiCad. The measured closure, the settings setup and the first patch series are now done (see "Gate 2 desktop proof"); the rest are still proposals.

- **Build host.** Do not build KiCad on Windows: the GLM failure and path-length errors are host problems, not port findings. Do the desktop proof on Linux (distribution packages provide GLM, Boost, wxBase, zstd and friends) and the iPadOS build on macOS with Xcode. A macOS 26 / Xcode 26.2 machine is now available for builds and simulator tests; Codemagic (Apple silicon, `xcode: latest`) remains the release builder and supplies the iOS 27 SDK.
- **Measured translation units instead of `eeschema_kiface_objects`.** Configure KiCad 10.0.6 with scripting, SPICE, i18n and OCC off, build its object libraries, and link a small proof executable that loads a schematic, recalculates `CONNECTION_GRAPH`, exports a netlist, runs ERC into a report sink and saves. Link with dead-stripping and a linker map. The objects that contribute live symbols are the real closure. Turn that list into CorvCAD's own C++20 static library target and record it in `upstream.json` and `THIRD_PARTY.md`. GUI symbols that stay referenced but never execute get explicit stubs, listed as patches.
- **Settings singleton (`Pgm()`).** Copy the setup that `kicad-cli` already uses to load schematics and run ERC without a window: a headless program object with a settings manager rooted in a directory the caller supplies (the app sandbox on iPad).
- **Editor code in model files.** A patch series under `integrations/kicad_core/patches/`, applied by `fetch.py`, plus CorvCAD replacements in `integrations/kicad_core/headless/` for editor-bound files. **Done for the schematic and board slices:** neither links a KiCad editor file (board: patches 0006 and 0007 plus `headless/pcbnew/`). Still to do: split the series per change for upstream. Header-only uses such as `SYMBOL_ORIENTATION_T` from `sch_edit_frame.h` cost nothing at link time and can wait.
- **wxWidgets.** Link wxBase only (strings, files, streams), which builds for iOS. Do not adopt the lower-tier wx iOS GUI port. Measured result: wxBase alone is not enough, because model code uses wx core value types (`wxImage`, `wxColour`, `wxBitmap`) and a few helpers. The wx iOS port provides all of these. Patch 0002 and the `headless/ios/` stand-ins remove the rest, so the schematic slice compiles against wx's iOS toolkit (see "wxWidgets for iOS" above).
- **Language level.** The CorvCAD engine and app now build as C++20, matching KiCad, so no language bridge is needed between the two.
- **iOS dependencies.** KiCad ships a `vcpkg.json`. Build the needed ports with vcpkg's `arm64-ios` and `arm64-ios-simulator` triplets on macOS, cached in CI, and pin the vcpkg baseline.
- **Symbol libraries.** Bundle the full official version-matched 223-library pack for offline use, as required by the approved schematic plan. Resolve project/imported libraries through the library table as well. A stripped starter-only edition does not satisfy this requirement; size and offline search performance belong to the packaging and device gates.
- **Backend switch.** A native backend implements the same `cc_sch_session_*` interface and reports `CC_SCH_NATIVE_KICAD`, `CC_SCH_ERC` and `CC_SCH_NETLIST` only once each works. The app already reads the backend name and capability bits.
- **Licensing.** The sideloaded edition links GPL KiCad code and ships complete corresponding source. A separate App Store edition is a post-v1 decision and would keep the compatibility backend.

## Packaging the native session (approved 2026-10-21; simulator and arm64 app builds)

The native `cc_sch_session` backend works as desktop and simulator executables (gate 3). The user approved this design on 2026-10-21, and it is partly built, with these differences from the proposal:
- `probe/ios_package.sh` writes per-SDK static libraries (`libcorvcad_kicad.a`, force-loaded, plus `libcorvcad_kicad_deps.a`) rather than an xcframework.
- The route planner stays in the app target.
- `headless/simulator_stub.cpp` replaces `spice_simulator.cpp`. A debug dylib keeps KIBIS's reference to it, which `-dead_strip` removes in the probes.

`xcodebuild ... NATIVE_KICAD=YES` swaps `schematic_session_c_api.cpp` for the native session and bundles the retained notices. The inspector and a Licences screen identify the edition. Measured 2026-10-21:
- the iOS 26.2 simulator UI tests pass with the native backend (screenshots, and place part plus undo);
- the unsigned arm64 Release app links, at 31.3 MB executable against 2.8 MB for the compatible edition.

`integrations/kicad_core/release_native.sh` makes a release on the Mac build host:
1. It checks the KiCad tree against the gate 1 inventory, packages it, and builds the unsigned arm64 IPA.
2. It writes the corresponding-source set beside the IPA: CorvCAD at the release commit, pristine KiCad 10.0.6 (whose patches are in the CorvCAD archive), the pinned wx tarball, and vcpkg at its baseline with the dependency sources it downloaded. It adds a README with the build order and `SHA256SUMS`.

The 2026-10-21 run produced a 9.5 MB IPA and about 410 MB of source archives. Publishing both together is a manual step, and nothing has been published yet. After a session edit, `ONLY=kicad_session probe/ios_sweep.sh ...` recompiles just that unit before packaging.

**Distribution decision (the project owner, 2026-10-21).** The project is not ready to publish, and no build of either edition is distributed to anyone.
- The private repository holds an exact copy of the work, and nothing else is published.
- `release_native.sh` and its source set exist so that a release can be complete when there is one. Running it publishes nothing.
- Publishing waits for the owner's explicit go that the app is ready for release.
- KiCad's patched source gets its own repository once the KiCad port is entirely finished. Until then, the pinned upstream commit plus `patches/` and `headless/` in this repository define it.

GPL source obligations apply when a binary is distributed, so they begin with the first release.

**First physical-iPad run (2026-10-22, reported by the owner).**
- Setup: the `NATIVE_KICAD` IPA from `89a1134` (built with the iOS 26.2 SDK, unsigned, sideloaded) on an iPad Air 11-inch (M2) running iPadOS 27.0.1.
- Result: it launched, and the owner's informal use behaved as expected.
- Not yet measured: memory, size, load time, output parity, and the `/System/Library/Fonts` lookup. Gate 4 stays open until they are.

**Gate 4 measurements on a physical iPad (2026-10-24).**
- **Setup.** iPad Air 11-inch (M2), `iPad14,8`, 8 GB, iPadOS 27.0.1. The unsigned `NATIVE_KICAD` diagnostics IPA from `835a772` was built on CircleCI (Xcode 26.4.1). It has the schematic session and the gate 5 board checks, and was sideloaded through LiveContainer. The CorvProbe tweak (`tools/corvprobe`) measured in-process, and `tools/corvprobe/gate4.py` compared against `kicad-cli` 10.0.6 on Windows. Executable 43.3 MB, bundle 43.9 MB; idle footprint 38 MB, with 5.3 GB available to the app.
- **Parity on four KiCad demo projects.**

  | Project | Netlist | ERC (by type) | Board DRC, as loaded / after refill |
  |---|---|---|---|
  | ecc83-pp | identical, 13 nets | identical apart from library checks | identical, 2 / 2 |
  | complex_hierarchy | identical, 52 nets | identical apart from library checks | identical, 0 / 0 |
  | video | identical, 588 nets | identical apart from library checks | identical, 0 / 0 |
  | jetson-agx-thor-baseboard | identical, 1,355 nets | identical apart from library checks (1,871 = 1,871) | identical, 379 / 23 |

  "Identical" for a netlist means equal pin groups and net names. For DRC it means equal lists by type, severity, item UUIDs and positions.
- **Library checks.** These depend on which libraries are installed and are counted apart:
  - ERC: on the iPad, `lib_symbol_issues` (26, 73, 395 and 0) appear because the official symbol libraries are not bundled yet (see the symbol bundle below). On the desktop, `footprint_link_issues` appear (3 and 1,141), a check the session skips.
  - DRC: the footprint-library checks. With them included, the Jetson board is 578 / 222 on both.
- **Timings on the iPad.**

  | Project | Open | Netlist | ERC | DRC | Refill and DRC | Process peak, fresh launch |
  |---|---:|---:|---:|---:|---:|---:|
  | ecc83-pp | 26–76 ms | 1 ms | 3 ms | 0.3 s | 0.3 s | 158 MB |
  | complex_hierarchy | 40 ms | 5 ms | 5 ms | 0.5 s | 0.8 s | 191 MB |
  | video | 182 ms | 19 ms | 49 ms | 1.2 s | 3.1 s | 492 MB |
  | jetson-agx-thor-baseboard | 0.9 s | 94 ms | 0.8 s | 17.5 s | 33.8 s | 2.04 GB |

  The peaks are the kernel's process peak footprint after each project in a fresh launch, so each includes the projects before it. The Jetson board's DRC raised it from about 0.9 GB to 2.04 GB, well inside what this 8 GB iPad gives the app. A 4 GB iPad may not have that much; this has not been measured.
- **Fonts.** The app sandbox can list and read `/System/Library/Fonts/Core` (75 entries) and `CoreUI`, which the Core Text font lookup uses.
- **With the bundled libraries (2026-10-24, IPA `1f0c90d`).**
  - The app bundles all 223 official symbol libraries and 155 footprint libraries: 413 MB installed, 52 MB IPA, and no 3D models.
  - ERC runs the footprint-link check through `kicad_board.cpp`'s stand-in for CvPcb.
  - `gate4.py` also uploads each project's own `.kicad_sym` and `.kicad_mod` files.
  - Result: on all four projects the netlist, ERC and board DRC (as loaded and after refill) are identical to `kicad-cli` with nothing set apart. That is 0, 0, 3 and 3,012 ERC items, and 2, 0, 129 and 578 DRC items (222 after refill), footprint-library checks included.
- **Library memory.** KiCad's library manager loads every library in a table, not only the ones a project uses.
  - On a fresh launch, the first ERC loads all symbol libraries (about 2 s; the process grew to about 1.0 GB). The first DRC loads all footprint libraries (about 3 s; about 2.2 GB).
  - Both stay loaded. The Jetson board's DRC then peaked at 4.7 GB.
  - This fits the 8 GB iPad Air but is unlikely to fit 4 GB iPads.
  - **Fixed (IPA `2efa8a6`, 2026-10-25):** ERC and DRC now load only the libraries a project references (`LoadOne` per nickname).
    - The process footprint after each project in one launch: ecc83 165 MB, complex_hierarchy 239 MB, video 522 MB.
    - The Jetson board peaked at 2.07 GB during DRC, down from 4.7 GB.
    - Netlist, ERC and DRC (as loaded and after refill) stayed identical to `kicad-cli` on all four projects.
- **Not covered:**
  - Zone fill on boards with teardrops.
  - iPads with less memory.
  - Library search (the part-library card).

**Combined edition probe pass on the physical iPad (2026-10-31).**
- **Setup.** The same iPad Air 11-inch (M2), iPadOS 27.0.1. The unsigned combined (KiCad + FreeCAD) diagnostics IPA from `f9b86a6`, built on the iMac (Xcode 26.2, iOS 26.2 SDK), sideloaded through LiveContainer with CorvProbe; `gate4.py` against `kicad-cli` 10.0.6 on Windows. Bundle 549 MB, idle footprint 56 MB, 5.3 GB available to the app.
- **Parity.** All four projects ran in one launch, in the order ecc83, complex_hierarchy, video (Jetson after a relaunch: the iPad slept during the first attempt and iPadOS ended the app). Netlist, ERC and board DRC (as loaded and after refill) are identical to `kicad-cli` with nothing set apart: 0, 0, 3 and 3,012 ERC items; 2, 0, 129 and 379 DRC items. On the three projects with desktop references, the BOM, the schematic plot (PDF pages and per-sheet SVG elements) and all fabrication files are identical too (10, 23 and 25 files). The Jetson board's 21 fabrication files and its plots were written but have no reference to compare with.
- **Project switch.** In the combined simulator rehearsal before this run, a project opened after another one saw the first one's library tables (ERC library findings, footprint-library DRC checks). It came with the 2026-10-29 project-lifetime change and was fixed before this IPA (see the project switch entry above); this run opened the projects one after another with no such finding.
- **Timings and memory on the iPad.**

  | Project | Open | ERC | Schematic plot, PDF / SVG | DRC | Refill and DRC | Fab outputs | Board plot, PDF / SVG | Footprint, highest |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|
  | ecc83-pp | 83 ms | 0.2 s | 68 / 5 ms | 0.3 s | 0.4 s | 45 ms | 29 / 12 ms | 187 MB |
  | complex_hierarchy | 38 ms | 0.3 s | 91 / 27 ms | 0.5 s | 0.7 s | 0.1 s | 0.3 / 0.1 s | 258 MB |
  | video | 186 ms | 0.8 s | 0.2 / 0.1 s | 1.3 s | 2.7 s | 0.7 s | 0.8 / 0.1 s | 557 MB |
  | jetson-agx-thor-baseboard | 0.9 s | 1.0 s | 0.9 / 1.2 s | 17.6 s | 30.1 s | 5.3 s | 3.9 / 1.2 s | 1.04 GB (peak 1.8 GB) |

  The first three ran in one launch, so each footprint includes the projects before it. The Jetson row is a fresh launch; the kernel's peak during its DRC was 1.8 GB.
- **Not covered:** the touch UI on the device (this pass drives the engines through the diagnostics hook), FreeCAD on the device (F5), and iPads with less memory.

The gate 1 inventory was recaptured on the Mac after packaging, junctions and rotation (1,585 inputs). It adds `ios_package.sh` and `simulator_stub.cpp`. The desktop CMake build globs `headless/`, so the stub also joins the schematic and session closures (336 units). Dependencies and notices are unchanged.

The original proposal follows.

- **One implementation per binary, chosen at build time.** `kicad_session.cpp` and the engine's `schematic_session_c_api.cpp` define the same `cc_sch_*` symbols. The sideloaded edition links the native one, and the App Store edition (post-v1) keeps the compatible one. The Swift side doesn't change: it already reads `cc_sch_session_backend()` and the capability bits at runtime. A runtime fallback between both would need renamed symbols and a dispatcher. Leave it out until a project the native backend can't open turns up.
- **Artifact.** `CorvCADKiCad.xcframework`, a static library with an `ios-arm64` slice and an `ios-arm64_x86_64-simulator` slice. It holds the measured schematic closure (`closure-10.0.6-headless.txt`), `kicad_session.cpp`, the shared route planner and the vcpkg dependencies that `ios_link.sh` already links. The board slice is added once it is decoupled. It is built by a script beside `ios_link.sh` (`ios_xcframework.sh`) from the same pinned inputs, cached in CI, and never committed.
- **App wiring.** `app/project.yml` gets a `NATIVE_KICAD` build setting. When it is set, the target drops `schematic_session_c_api.cpp`, links the xcframework, and adds the wx and system frameworks that `ios_link.sh` uses. Codemagic gets a second workflow for the sideloaded IPA. Static registration doesn't matter for the schematic session, but the board slice will need `-force_load` or explicit references for DRC providers and generators (see "Static registration" above).
- **Data.** Symbol libraries resolve through the project's own tables first. The two-pin starter part is compiled in (`starter_parts.hpp`). The full official 223-library bundle and the settings directory live in the app container, which `Pgm()`'s headless settings manager is pointed at. Fonts need nothing bundled (see gate 4).
- **Licence (per the plan above).** The sideloaded IPA is a combined work under GPL-3.0-or-later. CorvCAD's MIT files keep their notices. Each release publishes corresponding source: this repository at the release tag, the pinned KiCad 10.0.6 commit with `patches/`, the wxWidgets tarball and the vcpkg baseline from `upstream.json`, and the build scripts. The app shows the GPL text and a source link in its about screen. `THIRD_PARTY.md` already lists the dependency licences.
- **Checks before it ships.** The xcframework links into the app for the simulator and arm64. UI tests pass with the native backend, and the backend name shows in the workspace. Gate 3's `check.sh` scenarios pass through the app's own C calls. Size and memory are measured on the physical iPad (gate 4).

## Which upstream path to extract

KiCad's [source mirror](https://github.com/KiCad/kicad-source-mirror) has `common/`, `pcbnew/`, and `eeschema/` modules, plus a [predominantly GPL-3-or-later combined distribution](https://github.com/KiCad/kicad-source-mirror/blob/master/LICENSE.README). The upstream [IPC documentation](https://dev-docs.kicad.org/en/apis-and-binding/ipc-api/for-addon-developers/) states that KiCad 10 IPC requires a running GUI, while KiCad 11 adds headless `kicad-cli` IPC and schematic plugins. It also states that a standalone supported document-editing library is not planned. The headless KiCad 11 source path is the best starting point for an extraction experiment, but the IPC API is still expanding; do not assume it exposes all edits or DRC/ERC as library calls.

An upstream sparse checkout was inspected at commit [`d9ec718c68b909e29e2566dc1942d2a4e0732055`](https://gitlab.com/kicad/code/kicad/-/commit/d9ec718c68b909e29e2566dc1942d2a4e0732055) (the source location and candidate files are pinned in `integrations/kicad_core/upstream.json`). It contains `pcbnew/api/headless_pcb_context.*` and `eeschema/api/headless_sch_context.*`. The PCB context calls `BOARD_LOADER::SaveBoard`, uses `BOARD_NETLIST_UPDATER`, synchronizes nets, and accesses DRC tools. The schematic context calls `SCH_API_SAVE` and `EESCHEMA_HELPERS::LoadSchematic`. This is a better extraction seam than the editor frames, but is **not** a self-contained library: both contexts use `wxString`, `PROJECT`, `TOOL_MANAGER`, `KIWAY`, and global KiCad settings/services. The schematic file IO implementation even includes `sch_edit_frame.h` for an orientation type; PCB file IO includes `wx/msgdlg.h`. A wxBase-only build therefore requires source-level decoupling or small upstream patches; merely turning off GUI targets will not be enough.

Build a desktop **headless** proof before cross-compiling. Avoid starting with the PCB or schematic editor frames, canvases, dialogs, and interactive tools: those pull in wxWidgets GUI services that CorvCAD's SwiftUI front end does not need.

## wxWidgets choice

The current [wxWidgets platform documentation](https://github.com/wxWidgets/wxWidgets/blob/master/docs/doxygen/mainpages/introduction.h) lists iOS (minimum iOS 13) as a supported but lower-tier port. In 3.3.4 the CMake build has no iOS toolkit, but `configure --with-osx_iphone` builds it (measured, see "wxWidgets for iOS" above). The old iOS-9-era experiment is therefore not the only evidence. That build does **not** prove KiCad's full GUI or headless dependency graph builds and behaves correctly on iPadOS.

Prefer a headless KiCad slice using wxBase where possible, with narrow patches or platform shims for the GUI types found in the source audit. Measure each dependency before broadening the wx iOS port. CorvCAD's app UI should remain SwiftUI/Metal. Porting KiCad's desktop UI to wx iOS would duplicate the touch interface and expand the maintenance burden without advancing electrical operations.

## Extraction gates

1. Pin a KiCad upstream commit and map license and dependencies for selected `common/`, `pcbnew/`, and `eeschema/` translation units. Record every copied source file and upstream change in `THIRD_PARTY.md`; a combined binary using GPL KiCad code needs appropriate GPL distribution terms.
2. On desktop, build a small executable or library that uses upstream code to load and save one board and one schematic without constructing editor windows. Compare its results with `kicad-cli` and the installed KiCad 10 reference adapter.
3. Expose a narrow C ABI: open project, enumerate IDs, apply a transaction, undo/redo, save copy, and validate. Keep reference IDs stable across schematic and board. Test source and binary compatibility at the ABI boundary.
4. Cross-compile the **same** headless target for iPadOS arm64, bundle required data and libraries, run on simulator and physical iPad, then measure memory, load time, and output parity. No downloaded executable code or JIT is needed for native ahead-of-time compilation.
5. Add one KiCad operation at a time: schematic connectivity/netlist, board connectivity, zone fill, ERC/DRC, schematic-to-PCB sync, routing. For each, test a real project corpus and round-trip in official KiCad before calling it supported.

If the source extraction is blocked by pervasive GUI dependencies, continue the portable format-compatible core and use KiCad's official desktop/headless service for validation where available. This is a decision gate, not a claim that the on-device port exists.

# SPICE simulation with ngspice

Status:
- **2026-11-01:** the owner approved ngspice 46, statically linked in the `NATIVE_KICAD` edition,
  with XSPICE code models and OSDI left out at first. S1 started the same day.
- **Later on 2026-11-01:** the owner approved S1b, adding XSPICE with statically linked code
  models.

S1, S1b, S2 (KiCad's simulator in the native session) and S3 (the app) are done; results below.
S4, running it on a real iPad, is what remains.

## What it adds

KiCad's circuit simulator in the `NATIVE_KICAD` edition: operating point, DC sweep, AC, transient
and noise analyses on a project's schematic, with KiCad's own SPICE netlist export and model
handling.

Results UI (owner's direction, 2026-11-01): not only plots. Once the dependency works, the
simulation should also show the board being electrified, as an animation that is part of the 3D
viewer. See S3.

## What KiCad provides, and what replaces the GUI

KiCad's simulator splits into headless parts and the `SIMULATOR_FRAME` GUI.

- **Headless and reusable:** `eeschema/sim/`: the SPICE netlist exporter
  (`NETLIST_EXPORTER_SPICE`), the model layer (`SIM_MODEL*`, `SIM_LIB_MGR`, `SPICE_GENERATOR`) and
  the `NGSPICE` wrapper. Many of these units are already compiled into the native closure for
  the schematic. The port currently answers `SIMULATOR::CreateInstance` with nothing
  (`integrations/kicad_core/headless/simulator_stub.cpp`).
- **Not used:** the simulator frame, its plot panels and its dialogs.
- **Changed for iOS:**
  - `NGSPICE::init_dll` finds `libngspice` at run time through `wxDynamicLibrary`. iOS apps
    can't load a library they did not ship and sign, and loading code at run time is
    restricted. ngspice is linked statically instead.
  - A CorvCAD headless file will hand `NGSPICE` the linked functions (`ngSpice_Init`,
    `ngSpice_Command`, `ngGet_Vec_Info`, and so on), then call `corvcad_ngspice_codemodels()`.
    That call takes the place of the `codemodel` commands KiCad's wrapper sends for the `.cm`
    plug-ins it finds.
  - KiCad's wrapper above it stays unchanged.

## The dependency

- **Version:** ngspice **46**, the version KiCad 10.0.6's `vcpkg.json` pins. The tarball is
  pinned by SHA-256 in `integrations/kicad_core/upstream.json`; its SHA-1 and MD5 match
  SourceForge's listing. The newest release is ngspice-47 (2026-08-11).
- **Build:** `integrations/kicad_core/ngspice_ios.sh <work> mac|sim|device` configures ngspice's
  shared-library build: `--with-ngshared`, with XSPICE, and without OSDI, OpenMP, readline, FFTW
  or X11. It packs one static `libngspice.a`. It builds for macOS (with the plain `ngspice`
  executable and its code-model plug-ins as the reference), the x86_64 iOS simulator and arm64
  iPadOS. The Mac build goes first, because the iOS builds run its code-model generator
  (`cmpp`). HICUM2 is C++, so linking needs `-lc++`.
- **XSPICE code models without plug-ins:** desktop ngspice loads each code-model library (`.cm`)
  with `dlopen` and passes its device tables to `add_device`/`add_udn`. The static library holds
  instead:
  - the models' own objects;
  - `dlmain.c`'s helpers (`fopen_with_path`, `cm_message_printf`, `cm_is_inertial`), compiled
    from the pinned file. Its forwarders into the core are left out, since the core defines
    those functions itself.
  - `ngspice/static_codemodels.c`, which passes all 80 models of the seven libraries to the same
    two calls.
- **iOS adaptations** (`integrations/kicad_core/ngspice/ios_prefix.h`, included at make time
  only; configure's function checks need the plain headers):
  - iOS has no `system()`. ngspice uses it only for desktop helpers (its `shell` command,
    gnuplot), which now fail as if no shell were available.
  - An iOS process has no passwd entry, and `ngSpice_Init` dereferences `getpwuid(getuid())` to
    find `~/.spiceinit`. That crashed every simulator run. Without an entry, the home directory
    is now `$HOME`, the app's container.
- **Left out:** OSDI/Verilog-A, ADMS, and the Verilog co-simulation plug-ins.
- **Run time:**
  - ngspice keeps global state, so one simulation runs at a time.
  - Its fatal-error path calls the `controlled_exit` callback instead of ending the process; the
    probe checks this.
  - Vectors stay in memory with the shared API (the executable streams them to a raw file).
  - ngspice refuses a run whose estimated result exceeds the "available" memory. On Apple
    platforms it counts only free pages (`vm_stat.free_count`). That figure swings with the
    system's caches: on the iMac, Class-D (14 million values) ran in one check and was refused
    in the next. `ngspice/apple_memory.c` replaces that figure in every build of the library: on
    iOS with the app's real limit, `os_proc_available_memory()`, and elsewhere (macOS, and the
    simulator, which has no such limit) with free, inactive and purgeable pages. The S1 probes
    still set ngspice's `no_mem_check`, since they test results.
  - Code models open data files relative to the netlist's directory.

## Licences

ngspice 46's own `COPYING` (not Debian's older packaging map, which the first draft used) gives:

| Part | Licence | In the library |
|---|---|---|
| Most of ngspice | BSD-3-Clause | yes |
| `src/maths/KLU` | LGPL-2.0 | yes |
| `src/frontend/numparam` | LGPL-2.0-or-later | yes |
| `src/maths/sparse` | MIT (unnamed) | yes |
| Bison-generated parsers | Bison skeleton GPL-3.0-or-later WITH Bison-exception-2.2 | yes |
| `src/xspice` (core and code models) | public domain (Georgia Tech) | yes |
| `src/xspice/icm/table` | GPL-2.0-or-later | yes |
| `src/xspice/icm/dlmain.c` (its header) | BSD-3-Clause | its helpers only |
| `src/xspice/cmpp` | public domain; a build-time generator | no |
| `src/osdi` | MPL-2.0 | no |
| `src/xspice/verilog` co-simulation plug-ins | GPL-2.0-or-later | no |

The getopt file that Debian's map lists with an advertising clause was relicensed to BSD terms
in ngspice in 2018, and it compiles to nothing on Apple platforms (`getopt_long` exists).

`integrations/kicad_core/audit/ngspice.json` (written by `ngspice/inventory.py`) follows each of
the device library's objects to its source and the headers it read, through the compiler's
dependency records. It records 2,006 files, including the code-model sources and the generator,
each with its hash, role and licence. All of it fits the KiCad edition's GPL-3.0-or-later
combined work. Before an IPA with ngspice is shared, two things remain: the notices have to join
the app's legal resources, and the gate 1 capture has to see ngspice in the link maps (S2).

## S1 result (2026-11-01, Intel iMac, iOS 26.2 simulator)

**How the check works** (`integrations/kicad_core/ngspice/check.sh`):
- **Netlists:** each of KiCad 10.0.6's 18 simulation demos is exported with
  `kicad-cli sch export netlist --format spice`. If the schematic has no analysis, the check adds
  the first workbook tab's, as KiCad's simulator does for that tab.
- **Setup:** it sets KiCad's default compatibility mode (`ngbehavior=ltpsa`), turns off
  ngspice's free-memory check in the probes (see "Run time"), and copies the demo's data files
  beside the netlist.
- **Runs:** each demo runs through the ngspice executable (with its `.cm` plug-ins) and through
  `ngspice/probe.c` linked with the static library. The probe makes the shared-API calls KiCad's
  wrapper makes; it runs on macOS and in the simulator.
- **Comparison:** `ngspice/compare.py` compares every vector:
  - within ngspice's RELTOL of the vector's largest value, with VNTOL/ABSTOL as the floor;
  - on the reference's time points when a transient took another time-step path;
  - noise sources are left out, since ngspice seeds them from the process ID.

**Results:**
- **16 demos run, and all agree with the executable on macOS, identically.** They include the
  five that use XSPICE code-model devices (analog multiplier, Class-D, IBIS, SMPS, up-down
  counter) and the three vendor op-amp models that use `poly`.
- **In the simulator, 14 of them agree.**
  - Most agree to within 2.3e-12 of each vector's largest value; power_supply to 9.3e-6.
  - buck_conv takes a slightly different time-step path. Its RMS difference is 0.1%; switching
    edges land a step apart.
- **Class-D and smps-com** are long switching runs (450,000 and 1.07 million time steps).
  - In the simulator they drift from the reference by 6.5% and 20% RMS.
  - Control: the executable drifts from itself by 5.7% and 17.4% when only RELTOL is nudged by
    0.1%.
  - So the difference is these circuits' own sensitivity to rounding, not a fault in the build.
    The check counts such runs apart, never as agreement, and fails if a run exceeds 1.5 times
    its control.
- **hv_converter** runs in the executable, but its result (1.7 million points × 56 vectors, a
  2.3 GB ASCII raw file) is too large for this check to compare, so the check skips it and says
  so.
- **q17:** `kicad-cli` itself can't read the demo's model library.
- **Device:** the arm64 probe links (5.2 MB). Running it needs a signed app on an iPad (S4).

## S2 result (2026-11-05, Intel iMac)

**What S2 adds:**
- **Patch 0011:** with `CORVCAD_NGSPICE_STATIC`, KiCad's `NGSPICE::init_dll` takes ngspice's
  functions from the linked library and registers the code models, in place of loading
  `libngspice` and sourcing `spinit`.
- **Build:** the probe build takes `-DNGSPICE_BUILD=<ngspice_ios.sh work directory>`. Then
  `ngspice.cpp` and KiCad's own `spice_simulator.cpp` replace the stub. The native closure gains
  those two and `spice_circuit_model.cpp`, and loses the stub (637 units in all; the gate 1
  inventory is refreshed). The programs and the iOS package link `libngspice.a`.
- **Session API 11:**
  - `cc_sch_session_simulate` runs one analysis as KiCad's Simulator window does: the project's
    ngspice settings, KiCad's netlist of the current state, ngspice's background thread. It reads
    a workbook tab's option lines as KiCad's workbook loader does.
  - `cc_sch_session_spice_netlist` returns that netlist.
- **Symbol libraries:** `fetch.py` packs `Simulation_SPICE.sp` beside them, as KiCad installs
  it. Schematics name it `${KICAD7_SYMBOL_DIR}/Simulation_SPICE.sp`, which KiCad resolves through
  `KICAD10_SYMBOL_DIR`.

**Two findings on the way:**
- **Waiting for a run:** after `bg_run`, ngspice reports "not running" until its thread has
  started, so the session waits for KiCad's running report and then for idle.
- **Workbook options:** a tab's options are part of the run. KiCad's defaults for a new tab add
  `.probe alli`, which ngspice refuses in circuits with digital nodes (Class-D, smps-com). The
  demos' own tabs don't ask for it.

**Check:** `ngspice/check.sh` with `SESSION=<corvcad_kicad_native>` runs each demo through the
session on a copy of its project, with its workbook's first tab. The reference is the ngspice
executable on the session's own netlist, with the project's compatibility mode.
- **Desktop:** all 16 runnable demos agree.
  - 15 are identical, to 5e-16 of each vector's largest value.
  - v_i_sources agrees within 6e-4, because of its random noise sources.
  - The longest runs take 20 s (Class-D) and 31 s (smps-com).
- **iOS simulator** (iPad Pro 13, iOS 26.2): the native program, linked for the simulator from
  the sweep (`ios_link.sh PROGRAM=native`, 53.8 MB), against the same desktop references.
  - 14 demos agree: 10 within 5e-13, mult_vca810, generic_opamp_bip and mainsheet within
    2.3e-12, v_i_sources within 6.3e-4.
  - buck_conv takes the same other time-step path as the S1 simulator probe (0.1% RMS after
    interpolation).
  - Class-D and smps-com are within their own sensitivity (6.5% and 20% RMS against 5.7% and
    17.4%), also as in S1.
  - power_supply agrees within 3.3e-4 once one node is left out. That node is the transformer
    secondary's reference, tied to ground only through 1 GΩ, so ngspice resolves it only to
    about 1 mV. It carried 0.2 mV of rounding noise in one build and 0 V in the other; R3's
    current is left out with it.
- **App:** the simulator package links ngspice (639 objects), and the KiCad edition app passes
  all 9 NativeKiCadTests. The default edition builds and skips them.
- **Against kicad-cli's netlist:**
  - identical or within 0.06% wherever the netlists match;
  - ibis and power_supply take another time-step path, because their tabs add current probes
    that kicad-cli's export lacks.

## Phases

1. **S1, ngspice for iOS:** done, with XSPICE (S1b).
2. **S2, KiCad's simulator layer, headless:** done (above).
3. **S3, app:** done (below).
4. **S4, device validation** on the iPad, with timings and memory.

## S3: the app (2026-11-08, `NATIVE_KICAD` built with `NGSPICE_BUILD`)

**Simulation section** in the schematic inspector:
- The analysis is the project's first workbook tab (or the schematic's directive), or a transient,
  AC, DC sweep, operating point or custom command. "Add to schematic" places it as a text
  directive.
- A run takes KiCad's state on its own 16 MB thread behind a modal with Stop
  (`cc_sch_simulate_stop`). An edit clears the result.

**Plot** over the canvas (Swift Charts): one chart per unit, up to five traces, a drag cursor, gain
or phase for AC, a table for an operating point. Tapping a net or a part with the Select tool
adds it; net names come from KiCad's netlist through KiCad's SPICE naming. Results are kept at
about 4,000 points by stride, so fast edges can fall between them.

**Electrify** in the 3D view:
- **Voltages:** each board net's tracks, pads, vias and filled zones glow with the simulator's
  voltage over a time slider (cyan positive, red negative, square root of the share of the peak).
  The board finds a net by its name; a net not in the simulation stays dark. Electrify hides the
  lid. AC results have no single instant, so they aren't shown.
- **Current:** SPICE gives device pin currents, not track currents. `net_flow` (engine) solves
  each net's tracks as resistors between its pins; vias, pads and filled zones join without
  resistance. Dots move along tracks with the current's direction; their speed is relative.
- **Where current isn't shown:** nets touching a subcircuit (X) device, because ngspice names its
  pins by subcircuit pin rather than by pad; nets whose pin currents don't sum to zero; nets too
  large to solve (500 joined nodes).

**Check:** Codemagic `native-sim`, arm64 iPad simulator, KiCad edition with ngspice. `testSimulationOnBoard`
runs KiCad's rectifier demo with a generated board and finds a plot and 4 of 4 nets simulated, with
current on 4. On those nets Kirchhoff's check passes, which also confirms the sign convention. The
other 9 NativeKiCadTests pass, including the Eagle import, once the workflow seeded KiCad's QA files
(build 6af377026d03b6e6823ca478). Stopping
a run through the session keeps its points so far, and the next run in that session matches a fresh
one (10,008 points to 10 ms). The engine's `net_flow` cases (a line, parallel widths, a T, a zone, a
via, disconnected pins) are in `test_engine`.

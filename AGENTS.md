# CorvCAD contributor and agent guide

## Project and goal

CorvCAD is a free, open-source CAD workspace focused first on the PCB-to-enclosure workflow. The intended iPad experience combines KiCad-compatible electronics work with parametric mechanical design, a touch-native preview, and interoperable exports. The portable engine should also support future iPhone, macOS, Windows, Linux, and Android front ends. Technical drawings and slicing are later capabilities.

The app can create a local KiCad 10 starter project with a blank schematic and rectangular board outline, then reopen it and retain enclosure controls in an app-specific sidecar. Its touch tools add a limited two-pin part, wires, labels, board footprint, mounting hole, tracks and vias. They can also add copper zones (unfilled until KiCad's zone filler runs) and silkscreen/fab lines and text, and replace the board outline. Project libraries can be imported, created and removed, and their symbols and footprints edited on a canvas with source-preserving text edits. The editor uses snap feedback and hold menus for selected direct edits. These operations are a narrow format-compatible subset, not a complete PCB or schematic editor.

The portable engine also has a transaction-based schematic session and C ABI for hierarchical project-local files, undo/redo and source-preserving saves. It is explicitly a compatible fallback, not KiCad internals. KiCad 10.0.6 source and all official symbol libraries are pinned as external build inputs, ~~but no KiCad source is linked into the app yet~~ (**UPDATE 2026-11-14:** the default edition links none; the `NATIVE_KICAD` edition links KiCad's own code, and the `NATIVE_FREECAD` edition FreeCAD's). Keep this distinction visible in code, UI and documentation.

This is an incremental project. Describe implemented behavior precisely; do not present intended integrations as shipped features. The current status and build instructions live in `README.md`, `docs/ARCHITECTURE.md`, `docs/KICAD_INTEGRATION.md`, `docs/KICAD_CORE_PORT.md`, `docs/OPEN_CASCADE_IOS.md`, and `docs/MILESTONES.md`.

## Architecture rules

- Keep `engine/` portable C++ with a small C ABI for platform bindings. Put iPad interaction and rendering in `app/`; do not make the engine depend on SwiftUI, SceneKit, Metal, or Apple-only APIs.
- Keep the mechanical model, electronics model, import/export, and external-tool integrations behind explicit boundaries. Prefer replaceable adapters over coupling the main engine to one app or command-line tool.
- Treat KiCad files as user data. Preserve unsupported fields when editing, do not silently discard a project, and test round trips with KiCad itself. The current parser and footprint move are limited; they are not KiCad's editing engine.
- Treat OCCT B-rep as the intended source of truth for exact mechanical geometry. A mesh is for preview or mesh export. Do not convert an exact shape to a mesh and then claim a STEP export is exact.
- Keep current desktop OCCT STEP support optional. Do not enable iPad STEP in UI or docs until an iPadOS OCCT build, exact tray and lid geometry, export, and device validation actually work. (**UPDATE 2026-11-14:** met in the `NATIVE_FREECAD` edition by the 2026-10-31 STEP gate; elsewhere the rule stands.)
- Do not add FreeCAD, KiCad core, or a slicer as a required base dependency without a focused integration design, reproducible build, license review, and tests.

## Working rules

- Inspect the working tree before edits. Preserve existing staged and unstaged user work; avoid unrelated rewrites and do not stage or commit changes unless the task calls for it.
- Make the smallest coherent change that advances the requested workflow. Keep UI claims, README, architecture, milestones, and third-party notices consistent with code.
- Add a focused regression fixture or test when changing parsers, geometric operations, file exports, or data-preserving edits. Run the relevant checks and report what did and did not run. A passing desktop build does not establish an iPad device result.
- For electronics compatibility, validate representative board and schematic outputs with installed KiCad tools where available. For geometry, check valid solids, tolerances, separate tray/lid exports, and mesh/export consistency where applicable.
- Pin external source versions or release checksums for reproducible builds. Record licenses and notices before shipping third-party binaries. Never commit API tokens, signing identities, provisioning profiles, or other secrets.
- Gate 1's measured KiCad source, patch and dependency inventory lives in `integrations/kicad_core/audit/`. Run `audit.py verify --kicad <pinned-patched-checkout>` when changing native integration inputs. A changed closure, source file, patch, build script or dependency pin requires a reviewed inventory refresh; see `docs/history/KICAD_GATE1_HANDOFF.md`. Preserve per-file GPL, MIT and other notices. The inventory does not replace the complete corresponding-source archive required for a native binary release.
- Prefer official upstream documentation and working source examples for architectural claims. Mark proposals, untested paths, and inference as such.

## Current verification paths

- Portable engine: `cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release`, `cmake --build build/engine`, `ctest --test-dir build/engine --output-on-failure`.
- KiCad desktop adapter and validation: see `docs/KICAD_INTEGRATION.md`.
- iPad app and unsigned IPA workflows: see `README.md`, `app/project.yml`, and `.github/workflows/`. An unsigned IPA needs an appropriate installation/signing route. CorvCAD's current native code does not require JIT.

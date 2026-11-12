# Mechanical document session

`session_runtime.Session` implements the Python side of the optional F4 mechanical
C ABI. It requires the reviewed native FreeCAD modules. It imports the unchanged
F1 `model.py`, `inputs.py` and `checks.py`; the tray and lid remain parametric
PartDesign bodies with exact OCCT solids.

The constructor accepts `(document_path, input_json, initial_settings_dict)`.
Existing FCStd spreadsheet parameters take precedence over initial settings.
Opening checks project-local board identity and canonical mechanical input
(bounds, thickness and mounting holes including references), required feature graph,
parameter aliases, constrained sketches, solid validity, bounds, volumes and
through bores. Existing F1 documents can acquire session metadata after the same
checks. Opening does not save a file. Electronics-only edits may change the
whole-board SHA256 while preserving the mechanical contract; these revisions
reuse the existing enclosure and parameters. Changed mechanical input or an
incompatible/partial model produces an explicit error rather than regeneration.
The source snapshot/SHA stored in FCStd is the revision last validated by that
session. It updates on explicit save after reopening with a new matching input.
A session retained across later electronics edits keeps its prior validated
snapshot until reopen. Legacy F1 files lack retained board identity/geometry and
therefore require their original SHA for first adoption and explicit F4 save.

Methods match `engine/include/corvcad/mechanical_session_c_api.h`:

- `get_settings()`, `set_parameter(name, value)`, `recompute()`
- `can_undo()`, `can_redo()`, `undo()`, `redo()`
- `triangles(part)` returns nine finite coordinates per triangle; `part` is
  `0` for the tray or `1` for the lid.
- `save()`, `export_step(part, path)`, `export_stl(part, path)`, `close()`

Each successful parameter edit recomputes inside a FreeCAD transaction and adds
one settings snapshot to this session's undo history. Input or recompute failure
leaves settings, geometry and undo/redo stacks unchanged. History is session-local
and is not persisted across reopen. Multiple documents have independent sessions;
a second session cannot open the same path concurrently.

Preview and STL use tessellation of the same current exact solid at 0.05 mm.
Both mesh paths check closed manifold topology, winding and volume agreement.
STEP exports the exact body and reloads it to verify one valid closed solid,
bounds and volume. The parameter spreadsheet persists in FCStd rather than an
app sidecar. Unknown document objects and properties are retained through saves.

Writes use a temporary file in the output directory. New outputs are published
with an atomic no-replace hard link; existing owned outputs are hash-checked
immediately before atomic replacement. Explicit saves reject changes to the
opened document's bytes. Export paths may replace only outputs written by that
same live session whose bytes still match the recorded hash. A reopened session
treats pre-existing exports as unknown; choose a new output path. These checks do
not constitute a filesystem transaction against an uncooperative concurrent
writer in the final check-to-replace interval. No operation overwrites a file
merely because its name matches an expected export name.

Run the real-geometry regressions with a FreeCAD-capable Python interpreter:

```text
<FreeCAD-python> integrations/freecad_core/session/test_session_runtime.py
```

The suite exercises both unchanged F1 fixtures, all eight parameters, exact
STEP/STL/preview agreement, undo/redo, injected recompute/write failures,
provenance and partial-model rejection, external-edit protection, custom property
preservation, legacy F1 adoption and independent documents. There is no fake
geometry backend. A passing Windows run establishes desktop session behavior;
native C ABI and iOS simulator/device integration require their separate checks.

# FreeCAD F4 implementation and native validation

Codex claimed board card `cqs3tfefiib8wpkcmqtq6bri51h` after F3 acceptance.
Branch `codex/freecad-f4` starts from accepted F3 and merges committed `main`
through `919d3ec`. Claude's uncommitted library work remains in its own checkout.
**F4 accepted 2026-10-28 for the optional FreeCAD edition.** Both native C ABI
fixtures, portrait/landscape app UI tests, default-edition undo/redo and arm64
device compilation/link pass. Source remains in PR #3, stacked on F3.
Physical iPad execution is F5; combined KiCad/FreeCAD integration is separate.

## Session boundary

[`mechanical_session_c_api.h`](../../engine/include/corvcad/mechanical_session_c_api.h)
exposes initialization, open/close, parameter edits, recompute, undo/redo,
triangle buffers, explicit FCStd save, and separate tray/lid STEP/STL exports.
It contains no Python, Qt, FreeCAD or Apple types. The default engine links an
unavailable adapter; it cannot silently substitute a mesh for an exact export.
The optional native adapter initializes the reviewed F3 runtime once, serializes
calls with a mutex and Python GIL, retains process-scoped state, and catches
native/Python exceptions before returning through C.

The [Python session](../../integrations/freecad_core/session/README.md) uses the
unchanged F1 parametric model. It validates solids, dimensions, bores and feature
structure; parameter failures roll back without changing history. Existing
FCStd spreadsheet values override initial UI settings. Save preserves unrelated
document objects/properties and rejects externally modified files. Export uses
the same exact bodies as preview, with STEP reimport and STL manifold checks.

Board identity and canonical mechanical input are retained in FCStd. A changed
electronics-only source hash does not invalidate matching mechanical geometry.
Changed bounds, thickness or mounting holes require explicit resolution rather
than silently replacing the saved model. This remains the rectangular F1 scope:
plain bores, separate plain lid, no connector cutouts or fastening features.

The compatible board C ABI now exposes actual `general/thickness` metadata;
missing metadata is explicit, rather than an assumed 1.6 mm. The measured KiCad
closure inputs and pins are unchanged; its inventory verification passes.

## App edition

`NATIVE_FREECAD=YES` selects the optional adapter and SDK-specific runtime.
The app uses exact-solid tray/lid tessellation for SceneKit, FCStd-backed
settings, native parameter undo/redo, explicit save and separate STL exports.
The default edition retains its current mesh and sidecar behavior.

The native edition prompts before discarding unsaved enclosure work. Renaming a
project with a native enclosure is blocked because it changes stored board
identity. Layout edits that invalidate the current enclosure produce an error;
they do not replace the exact model with fallback geometry. Licences come from
the packaged upstream/dependency notices.

User-facing STEP remains hidden until F5 physical iPad validation. Exact STEP
is exercised internally through the native C ABI. Enabling both `NATIVE_KICAD`
and `NATIVE_FREECAD` currently fails explicitly: common package ABIs and archive
coalescing must be verified before a combined edition can link each shared
dependency once.

## Reproducible native path

F4 adds a deferred CMake hook to the accepted F3 build. It does not change the
hashed upstream patch profiles. The native probe shares F3's actual link closure.
Packaging force-loads only the nine FreeCAD archives and session adapter, and
deduplicates the remaining static archives into a separate dependency library.
Per-SDK package manifests retain archive/resource hashes; the runtime excludes
the command-line executable. Resources are verified before and after validation.

Use the existing manual Codemagic `freecad-ios` or `freecad-ios-device` workflow
with `FREECAD_STAGE=session`, branch `codex/freecad-f4`. This preserves each
workflow's accepted SDK input cache. The pipeline runs portable/source/package
checks, builds the native probe, repeats the F3 baseline, stages the F4 package,
and runs both C ABI fixtures plus app UI tests on an owned iPad simulator.
The device lane compiles/links the app without device execution. No IPA or native
package is published as an artifact.

Equivalent commands after the F3 input builds:

```sh
python3 integrations/freecad_core/session/build.py --sdk iphonesimulator
# Stage the baseline runtime as documented in ios/README.md first.
python3 integrations/freecad_core/session/package.py --sdk iphonesimulator --output build/freecad-ios/package/sim
python3 integrations/freecad_core/session/run.py --sdk iphonesimulator --package build/freecad-ios/package --output build/freecad-ios/f4-evidence
```

The probe covers real in-process C ABI edits, rollback, undo/redo, persistence,
buffer contracts, independent sessions and export guards. `NativeFreeCADTests`
uses a seeded project and conflicting legacy sidecar to check native preview,
parameter undo/redo, save/reopen byte preservation, rejected edits and STL export.

## Verification and remaining work

- Thirteen real session regressions pass on pinned Windows FreeCAD 1.1.4.
  This establishes the Python backend only, not native C ABI or app acceptance.
  The [retained receipt](../../integrations/freecad_core/session/evidence/windows-session-initial.json)
  records exact versions, source hashes and the full local log identity.
- Eight configuration/package regressions pass locally and on the iMac, including the real
  deferred hook, upstream resource-copy targets, force-load scope, archive
  deduplication, architecture mismatch rejection, immutable resources and
  distinct native/default project seeding without overwrite.
- The iOS source/runtime suite passes locally: 107 tests pass and two
  Clang-dependent checks skip, including the new Intel target/cache guards.
- Workflow/app configuration and shell syntax checks pass; KiCad's unchanged
  source/dependency/notice inventory verifies.
- Native adapter compilation/link and both C ABI fixtures pass on the Intel
  iPad simulator at `fba4dec`, including the bundled directory-root regression.
  The real native app passes both portrait/landscape flows, and the default
  edition passes its undo/redo regression. The owned simulator is deleted and
  runtime resources remain unchanged. The [accepted simulator receipt](../../integrations/freecad_core/session/evidence/imac-simulator-accepted.json)
  records tested source hashes, both fixture exports and three successful UI
  tests at `d9d47c0`.
- Arm64 device native probe and app compile/link pass. The [device receipt](../../integrations/freecad_core/session/evidence/imac-device-accepted.json)
  records a 91,427,224-byte probe, the 94,100,168-byte app debug code image,
  SDK/architecture metadata and all 1,712 bundled resource hashes checked
  against the package. No physical device execution is implied.

The first native run `6adfb5bb49af9ae3e9fbcff3` at `7a8d998` passed all source
and package regressions, all six portable engine tests on macOS, dependencies,
CPython and OCCT. Configuration then failed because CMake forbids creating
subdirectories during deferred execution. The hook now defers an `include`,
with explicit source paths relative to its list file. A compiler-free real
CMake regression checks late target visibility, source paths and repeat
configuration. Native adapter compilation and app execution did not run in
that failed attempt. Its [receipt](../../integrations/freecad_core/session/evidence/codemagic-session-configure-initial.json)
retains the precise failure and verified scope.

Retry `6ae04fa3cc6c87aa9f261b3d` at `2ea6eca` configured successfully and compiled
and linked both the command executable and native C ABI probe. Staging then
failed on missing `Mod/Material/Init.py`: upstream resource-copy targets are in
the default build graph but are not dependencies of the two selected executable
targets. The F4 builder now builds the complete reviewed headless graph, like
F3. A real CMake regression confirms that an independent `ALL` resource target
is built and its initializer matches the source before staging. The
[staging receipt](../../integrations/freecad_core/session/evidence/codemagic-session-staging-initial.json)
separates successful adapter compilation from untested runtime/app behavior.

Run `6ae08de59c9b17b232c52691` at `83b09a9` finished on 2026-10-27 at
12:22 UTC. Compilation/link, resource staging and both F3 baseline proofs
passed. The packaged C ABI probe failed during initialization with
`No module named 'PartDesign'`; app build/tests were not reached. The
[bootstrap receipt](../../integrations/freecad_core/session/evidence/codemagic-session-bootstrap-initial.json)
retains that boundary. FreeCAD caches application directories before processing
`--set-config`, so the CLI under `runtime/bin` worked while the embedded probe
searched relative to its executable. The adapter now refreshes the public
directory value and reruns FreeCAD's registered bootstrap against the bundle.
The native probe checks those roots and the actual PartDesign helper origin.
The later Intel native fixture runs at `fba4dec` confirm this correction.

## Intel iMac continuation

Codemagic minutes were exhausted after that run. The user authorized the
existing SSH host `corvmac`: Intel x86_64, macOS 26.3.1, Xcode 26.2 and iOS
26.2 simulator. Runner card `cszjyegzaqt84dnrbfissb19pqw` is claimed by Codex.
Use isolated `~/corvcad-freecad-f4` and `build/freecad-ios-intel`; preserve the
other checkouts and KiCad caches. All six portable engine tests pass there.
The [frontend receipt](../../integrations/freecad_core/session/evidence/imac-frontend-contract-initial.json)
records the stub-linked compilation check; it establishes no native execution.
All 109 source/runtime tests and seven package tests also pass on this Mac with
Python 3.14. An owned iPad simulator executed an x86_64 process and was removed;
the [runner receipt](../../integrations/freecad_core/session/evidence/imac-runner-initial.json)
records those preparation checks separately from native FreeCAD acceptance.

The build tools now accept explicit `--architecture x86_64` for the simulator
only, select native `x64-osx` host generators, and reject mismatched caches,
inventories and packages. The default remains arm64; device builds require it.
Run `deps.sh build/freecad-ios-intel/deps x64-ios-simulator`, then pass that
root and architecture to the Python, OCCT, FreeCAD, staging, packaging and
validation commands. The dependency inventory also accepts the new triplet.
The pinned registry's older Eigen override requires full vcpkg Git history.
Use two build jobs on this 8 GB machine. The accepted Intel results are separate
evidence from the arm64 F3 receipts and physical F5 gate.
Host build scripts require Python 3.12 or newer; this Mac's unqualified
`python3` is 3.10, so use `/usr/local/opt/python@3.14/bin/python3.14` explicitly.
The dispatched build's state is `build/f4-imac/native-run-state.json`, with
per-stage logs alongside it. Its owned bootstrap and continuation helpers are
ignored local runner files; check the state and process before restarting to
avoid dispatching duplicate builds. The sequence covers dependency inventory,
Python/OCCT, native build, F3 baseline, C ABI fixtures and app UI tests; it stops
at the first failure.

The first Intel dependency build completed successfully (137 installation
steps). Its target inventory verifies 123 package records, 50 static archives
and 105 notices. The host CPython build then discovered Homebrew OpenSSL 4,
whose API is incompatible with pinned CPython 3.13.7. The host generator now
disables optional OpenSSL modules through configure cache flags and checks
generator extensions before accepting a partial build. This affects host
tooling only. Both new regressions pass on the iMac; native Python/FreeCAD and
app results were pending at that checkpoint.

The resumed Intel sequence built Python and OCCT, passed the Python simulator
probe, compiled/linked FreeCAD, staged resources and repeated both F3 geometry
proofs. Both real C ABI fixtures then passed all operation and export guards;
the native app compiled and ran. The [session/app receipt](../../integrations/freecad_core/session/evidence/imac-session-app-initial.json)
retains this successful native scope and the first UI failure separately.
The slider's final binding write could overwrite the restored setting after
its editing callback. Commit/validation now runs on the next main-queue turn.
The native UI test passes with that correction. The default-edition regression
then exposed a missing Demo fixture in the runner; each edition now explicitly
receives its requested project. Native flows now launch a fresh project in each
orientation and drag the thumb directly, avoiding rotation-transition frames
and localized accessibility-value assumptions. Both native tests and the
default undo test pass. The completed run's state is `native-run-state.json`,
output `build/freecad-ios-intel/f4-evidence-ui-retry4`, under the same remote
workspace. Export hashes, immutable resources and simulator cleanup were
independently verified before retaining the acceptance receipt.

The arm64 device sequence also completed on this Intel host, reusing the
reviewed sources and native host tools with separate SDK-specific target
inputs. It used one compile job and a free-space guard. Inspect
`build/f4-imac/device-run-state.json` and its per-stage logs before restarting.
Device compilation is not physical iPad execution; F5 remains separate.

F4 covers the optional single-engine FreeCAD edition. Combined KiCad/FreeCAD
integration is tracked on card `ckz3cdm77gbf57ky6cfjk3we9zr`: verify the union
dependency features/ABIs and link each common archive once before lifting the
combined-build guard. Physical iPad execution and public iPad STEP availability
remain F5; release packaging is separate from these development checks.

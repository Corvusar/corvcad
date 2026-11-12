# FreeCAD F5 preparation packet

Preparation card `c43qnn68gifrrzguu9x51kdwmxw` is owned by **Codex**. Actual F5
remains **Planned**: no iPad is available. This packet does not enable public
STEP export and cannot automatically accept F5. F4's simulator and arm64
compile/link results remain separate evidence in [the F4 handoff](FREECAD_F4_HANDOFF.md).

## Device path and build contract

Reuse the existing [CorvProbe physical-iPad transport](../../tools/corvprobe/README.md):
the LiveContainer tweak measures the app process, samples physical footprint
every 100 ms, records the kernel's process-lifetime peak and available memory,
and transfers files below Documents over its authenticated LAN interface.
The new `mechanical-info` and `mechanical` commands run through the app's
`CORVCAD_DIAGNOSTICS` notification hook. The diagnostic uses the same native
session C ABI and app preview preparation as F4, in separate fresh directories;
the user's visible project is untouched. Diagnostic STEP calls have no public
app action and are excluded when `CORVCAD_DIAGNOSTICS` is absent.

The build owner must supply the reviewed arm64 device package. The app build
settings are `NATIVE_FREECAD=YES`, the explicit `FREECAD_PACKAGE` path,
`SWIFT_ACTIVE_COMPILATION_CONDITIONS='$(inherited) NATIVE_FREECAD CORVCAD_DIAGNOSTICS'`,
and `CODE_SIGNING_ALLOWED=NO` for the unsigned development build. Preserve the
build log, source revision/dirty state, package manifest, executable metadata,
IPA hash and all retained notices. Package the built app in `Payload/CorvCAD.app`
using the existing unsigned-IPA route; this packet does not build, publish,
sign or install it. Use the owner's established signing/LiveContainer route.
Keep the probe tweak out of release packaging. The combined native edition
remains subject to its separate dependency/link validation.

The current diagnostic does not change package/dependency scripts or the app
edition guards. The build owner controls runner dispatch and any diagnostic IPA.
`CORVCAD_DIAGNOSTICS` works in Release; `DEBUG` is required only for the simulator
launch hook below, not for the physical-device notification commands.
An isolated simulator rehearsal must explicitly request `--expect-hardware simulator`;
it remains simulator evidence. `tools/capture` records simulator framebuffers and
does not establish physical-iPad behavior or performance.

## Prepare immutable inputs

From the repository root, using Python 3.12 or newer:

```sh
python3 integrations/freecad_core/device/packet.py prepare build/freecad-f5/packet-001
python3 integrations/freecad_core/device/packet.py verify build/freecad-f5/packet-001
```

The packet retains two reviewed rectangular F1 cases: the small `demo` board
with one mounting bore, and the complete `roundtrip` KiCad project with two
mounting bores. It records the original board bytes, actual SHA256 (including
line endings), accepted mechanical input, tool/app source hashes, Git revision
and dirty state. These are prototype enclosures with plain through bores and a
separate plain lid; there are no fastening features or connector cutouts.
Preparation refuses an existing output directory. Do not alter a prepared packet.

## Network-free simulator rehearsal

The Mac build owner may rehearse the hidden command without loading CorvProbe.
Build the native simulator app with `DEBUG NATIVE_FREECAD CORVCAD_DIAGNOSTICS`,
install it in an **owned** simulator and copy one prepared case to its app data
container's `Documents/Gate5/<packet-run-id>/<case>/`. Launch a fresh process with:

```sh
SIMCTL_CHILD_CORVCAD_MECHANICAL_REQUEST="Gate5/<packet-run-id>/demo/request.json" \
  xcrun simctl launch "$OWNED_SIMULATOR" org.corvcad.CorvCAD
```

The install-time hook exists only when all of `DEBUG`, `CORVCAD_DIAGNOSTICS` and
`targetEnvironment(simulator)` apply. It posts the same in-process notification
as CorvProbe and writes a new `simulator-reply.json` beside `request.json`.
Poll that file, require `passed: true`, `hardware_class: simulator`,
`rehearsal_only: true`, `physical_footprint_measured: false` and
`f5_accepted: false`, then verify the actual `outputs/device-report.json`,
FCStd/STEP/STL hashes and operation flags. Repeat with the other case in a fresh
process. Existing outputs/replies are not overwritten. Retain the simulator ID,
build/source/package identities and cleanup evidence.

This path has no fabricated `/metrics` or footprint data and does not run the
physical collector. Its files are useful for checking diagnostic wiring and
desktop reimport implementation, with a separately labelled simulator receipt.
Actual F5 remains Planned. No public UI action or production app behavior changes.

The owned-simulator runner automates this sequence, verifies actual output/input
and bundled helper hashes, and deletes only the simulator it created:

```sh
python3 integrations/freecad_core/device/rehearse.py \
  --app build/native-workspace/freecad-diagnostics/Build/Products/Debug-iphonesimulator/CorvCAD.app \
  --packet build/freecad-f5/packet-001 --output build/freecad-f5/rehearsal-001 \
  --package-manifest build/freecad-ios-intel/package/sim/package-manifest.json
```

`--package-manifest` is optional; when supplied, every actual app runtime resource
must match it. Additional `--receipt <path>` arguments retain build/package
evidence. The runner selects an installed compatible iPad, installs the supplied
app, seeds immutable inputs, launches a fresh process for each case and waits at
most 300 seconds for each real reply. It retains screenshots, raw replies,
actual exports, runtime/code identities and cleanup results, including partial
failure evidence. No native package, default simulator or user project is rebuilt
or deleted. The app must be rebuilt after diagnostic source changes. F4 packages
retain individual runtime resources rather than the staging `runtime-manifest.json`;
diagnostics therefore identify the actual bundled helper files, and hash the
staging manifest only if it is present.

## Collect each case on the iPad

Install the diagnostics app/tweak as described by CorvProbe. Keep the app in the
foreground and remain on its home screen. Store its token in a local file outside
the repository; the collector takes a token-file path and does not store the token.
Use the trusted LAN and turn off the development tweak when finished.

For a cold measurement, terminate CorvCAD completely and relaunch it before
**each** case. Do not open an enclosure first. Record the iPad model/OS, installation
route, power/thermal state and whether the app process was freshly launched.
The diagnostic records whether the app wrapper had already initialized FreeCAD;
`--fresh-launch-confirmed` rejects a pre-initialized runtime. This is a runtime
initialization/open measurement, not boot-to-first-frame timing or a claim that
OS disk caches are cold. Later parameter edits are warm-runtime recomputes.

```sh
python3 integrations/freecad_core/device/collect.py \
  --packet build/freecad-f5/packet-001 --case demo \
  --host 192.168.1.10 --token-file /private/path/corvprobe-token.txt \
  --fresh-launch-confirmed --output build/freecad-f5/demo-001
# Fully terminate and relaunch CorvCAD before collecting the second case.
python3 integrations/freecad_core/device/collect.py \
  --packet build/freecad-f5/packet-001 --case roundtrip \
  --host 192.168.1.10 --token-file /private/path/corvprobe-token.txt \
  --fresh-launch-confirmed --output build/freecad-f5/roundtrip-001
```

The collector verifies uploaded bytes by reading them back. The app parses the
board through its C ABI, validates the four-edge rectangle and actual thickness,
initializes/opens and prepares the preview, measures all eight parameter changes
and restores, checks rejected edits and undo/redo, saves/reopens FCStd without
changing bytes, and exports separate exact STEP and STL files. The collector
compares the actual mechanical input with its fixture contract and checks every
download against device-reported size/SHA256. Requests and outputs live under
`Documents/Gate5/<unique-run>/<case>/`; existing inputs/outputs are refused.
Prepare a new packet for another run instead of erasing prior evidence.

Retained outputs include:

- `enclosure.FCStd`, `tray.step`, `lid.step`, `tray.stl`, `lid.stl`, actual
  `input.json` and the original KiCad inputs;
- device operation/timing report, notification reply, process identity before/after,
  runtime-manifest hash, device/bundle/executable size and power/thermal metrics;
- whole-command memory samples, before/after physical footprint, sampled peak during
  the command, kernel process-lifetime peak, logs, and the visible app-window screenshot;
- a hashed collection receipt that still says `f5_accepted: false`, with independent
  desktop reimport, slicer and manual UI review pending.

The kernel peak is cumulative for that process. The sampled command peak can
miss spikes shorter than 100 ms. It is not a per-parameter peak, and resident
bytes are not physical footprint. Size is the installed diagnostic app, including
the effect of its packaging/installation route. The app's full code image and
runtime package sizes remain independently recorded by the build owner.
On failure, preserve the failed receipt, partial files and best-effort logs and
memory evidence. Never relabel a failed or simulator run as physical acceptance.

## Independent desktop and slicer review

Use the pinned stock FreeCAD 1.1.4 command executable:

```sh
python3 integrations/freecad_core/device/desktop_check.py \
  --freecad-command /path/to/FreeCADCmd --case build/freecad-f5/demo-001 \
  --output build/freecad-f5/demo-001-desktop.json
```

Repeat for `roundtrip`. The runner requires both a successful command and its
explicit success JSON, since FreeCADCmd may return zero after a script exception.
It verifies collected hashes, opens the actual FCStd and retained spreadsheet
settings, recomputes, reimports each STEP as one valid closed exact solid, checks
analytic bounds/volume and real through-bores with material around them, and
checks STL bounds, manifold topology, winding, self-intersection and volume.
Collected input/export bytes must remain unchanged.

Then open the **same hashed** tray/lid STL files in a desktop slicer. Record slicer
name/version, units/profile, displayed dimensions, object count, screenshots and
any repair warnings. Retain the slicer project or log if available; do not replace
the source STL with a repaired/resaved copy. Also manually open the FCStd in desktop
FreeCAD to inspect the feature tree and edit a parameter in a separate copy.

On the physical iPad, exercise the actual enclosure UI in portrait and landscape:
preview/orbit/zoom, change/reject a parameter, undo/redo, save/relaunch/reopen and
share both STL files. The hidden diagnostic does not substitute for these touch
checks; its screenshot records the current app window, not an automated UI flow.
Record any foreground/background lifecycle or memory-pressure failures explicitly.

Only after the owner reviews the actual physical runs, desktop/slicer artifacts
and UI checks may F5 be accepted and public iPad STEP availability be considered.
No script here changes board status or exposes that action.

## Preparation checks

On 2026-10-28, source `6534bb7` passed the real native diagnostics rehearsal
for both fixtures on the Intel iMac's iPad simulator. All eight parameter
changes/restores, rejected-edit rollback, undo/redo, FCStd byte-preserving
reopen and separate STEP/STL exports passed. All 1,712 bundled runtime files
remained unchanged, output hashes were checked, and the owned simulator was
shut down and deleted successfully. The [retained preparation receipt](../../integrations/freecad_core/device/evidence/imac-preparation-simulator.json)
records the actual results. Physical footprint, device touch checks, desktop
reimport and slicer review were not run; F5 remains Planned.

```sh
python3 -m unittest discover -s integrations/freecad_core/device -p 'test_*.py' -v
```

These tests use clearly synthetic protocol fixtures, verify hash/overwrite/path
guards and failed-run retention, reject simulator-as-physical and warm-as-cold
classifications, and check that transport errors do not expose tokens. They do
not execute FreeCAD, a slicer or a device. The 19 helper tests are separate from
the real simulator rehearsal above. Physical checks cannot run until an iPad
is available.

# iPad demo capture on the iMac

Status: headless iMac take recorded and checked on 2026-10-27. The
Intel iMac (`corvmac`) builds, drives and records the app locally. GitHub-hosted
runners are no longer the capture path for this demo.
This is the iPad app in Simulator, not a macOS app or a physical-iPad recording.

The story is one continuous project: **launch CorvCAD â†’ home screen â†’ create a
project â†’ build a real circuit's schematic from scratch â†’ move parts and connect
wires â†’ update its PCB â†’ arrange and route the board â†’ show the finished PCB**.
Keep the final shot in PCB mode.

## What the existing recording does

[`demo-video.yml`](../../.github/workflows/demo-video.yml) is the older, manual CI
experiment. [`DemoVideoTests.swift`](../../app/UITests/DemoVideoTests.swift) opens
preloaded `ecc83-pp` and `ecc83-unrouted` projects, places an LM358, routes a few
connections, shows DRC and ends in Enclosure. It skips unavailable steps and the
workflow tolerates test failure. It also exports two opposite rotations without
establishing capture cadence. Those artifacts do not satisfy this plan.

Retain those files as references for building and interacting with the native
edition. A replacement scripted take must create the project through the UI,
assert every required step and stop on failure. XCTest may drive the local iMac
session if needed; that is separate from using a hosted CI runner for capture.

## Host and capture preflight

The host is Intel `x86_64`, macOS 26.3.1, Xcode 26.2 and the iOS 26.2
Simulator runtime. Its console session is locked and reports zero active desktop
displays. A logged-in WindowServer does not establish screen-recording access.
The tested path reads the Simulator framebuffer over SSH; it needs neither an
unlocked desktop nor macOS desktop recording permission.

1. Build the **`NATIVE_KICAD=YES`** simulator edition with the pinned symbol and
   footprint libraries. Follow [the native build instructions](../KICAD_CORE_PORT.md)
   and `app/project.yml`. The compatibility edition cannot demonstrate native
   library placement, schematic-to-PCB update and the native board router.
2. Use the [capture tools](../../tools/capture/README.md) to build the pinned recorder
   and create a dedicated custom **1x iPad simulator**. Its portrait framebuffer
   is 834 × 1210 pixels; the landscape app layout remains 1210 × 834 logical
   points. This is a reduced-resolution capture, not Retina or physical hardware.
   Preserve existing simulators and projects.
3. Build the app and UI tests for this device, prepare landscape, and rehearse
   the circuit. `FromScratchDemoTests` is opt-in and fails on missing steps. Its
   final take creates and edits the project through visible UI; it never imports
   a circuit or opens a project through launch shortcuts. Rehearsal-only helpers
   are separate tests.
4. Check a decoded landscape frame and continuous motion before recording.
   Keep the iPad aspect ratio, readable text and the whole canvas. The raw
   framebuffer remains portrait even while the app is landscape; the tested
   delivery transform is one counterclockwise quarter-turn (`transpose=2`).
5. Keep capture logs, UI result bundles, screenshots and the authored project.
   Stop recording on the completion marker before XCTest teardown closes the app.

## Circuit for the take

Recorded circuit: **LED Switch**, a 5 V, low-current LED indicator
controlled by a 3.3 V or 5 V logic input through an NPN transistor. It is small
enough to draw and fully route on camera while demonstrating more than isolated
symbols. The full UI rehearsal authored and routed it successfully. Independent desktop
KiCad checks verified the six intended nets and all thirteen component pins;
this is file/connectivity validation, not a physical bench test.

| Reference | Symbol / value | Assigned footprint |
| --- | --- | --- |
| J1 | `Connector_Generic:Conn_01x02`, power input (+5 V, GND) | `Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical` |
| J2 | `Connector_Generic:Conn_01x02`, control input (CTRL, GND) | Same two-pin header |
| R1 | `Device:R`, 1 kÎ© base resistor | `Resistor_SMD:R_0805_2012Metric` |
| R2 | `Device:R`, 330 Î© LED series resistor | `Resistor_SMD:R_0805_2012Metric` |
| D1 | `Device:LED`, red LED | `LED_SMD:LED_0805_2012Metric` |
| Q1 | `Transistor_BJT:BC547`, NPN switch | `Package_TO_SOT_THT:TO-92_Inline` |

Wire +5 V from J1 through R2 to D1's anode; connect D1's cathode to Q1's
collector. Connect CTRL from J2 through R1 to Q1's base. Join Q1's emitter and
both connector grounds. Use clear +5V, CTRL and GND labels. The six intended
nets are supply, R2â€“LED anode, LED cathodeâ€“collector, control, R1â€“base and ground.
Verify polarity and terminal-to-pad mapping against the pinned libraries and
component data before claiming a buildable design. Add any ERC power flags
required by the chosen symbols through the app UI.

Use a 40 Ã— 30 mm rectangular starter board, subject to rehearsal of placement
and clearance. The exact board size and shot timing can change to keep the
result readable; the completed circuit and workflow are required.

## Shot sequence and visible evidence

| Beat | On-screen action | Required result |
| --- | --- | --- |
| Launch | Start recording with the app closed; launch CorvCAD from the iPad home screen. | CorvCAD home screen is visible for a short readable hold. No launch environment shortcut opens a project. |
| Create | Tap **New project**, enter **LED Switch** and board dimensions, then **Create project**. | A new blank schematic and board belong to this project. No imported circuit or hidden file injection. |
| Place | Open the part picker, find and place all six parts, edit values and select real footprints. | Each part has the intended reference, value and footprint. Show enough picker interaction to establish ordinary use. |
| Arrange | Reposition at least two symbols, rotate as useful, pan/zoom to frame the circuit. | Visible before/after arrangement with usable snap feedback. |
| Wire | Connect every circuit net with the Wire tool, including the common ground branch; add labels. | A complete, legible circuit with actual pin connections, not wires merely touching symbol artwork. |
| Check schematic | Inspect connectivity, annotate if needed, run native ERC and save/export the netlist. | All six components and the intended connectivity verified; resolve unexplained findings. |
| Transfer | Switch this same project to **PCB** and invoke **Update from schematic**. | Six matching footprints with correct pad nets and a visible ratsnest. No second project. |
| Layout | Arrange the footprints inside Edge.Cuts, with accessible power/control headers and a readable LED position. | Deliberate board placement; show at least one footprint move and rotation. |
| Route | Use the native Track tool to connect all nets. Add B.Cu/vias only where useful. | All required connections routed; no skipped or unrelated connections. |
| Check PCB | Run native DRC, fix issues and rerun. If using a copper zone, refill before the final check. | Zero unrouted connections and no unexplained DRC violations. Preserve actual reports. |
| Reveal | Close the inspector, fit the completed board, briefly zoom into routing and return to the overview. | Final clean PCB view, held for 5â€“8 seconds, with outline, footprints and copper readable. End here. |

Current component movement uses the hold menu's **Move**, then a tap at the new
location. A hold-and-slide selects a menu action; it is not a free symbol drag.
Show the supported interaction accurately. If literal direct dragging is needed
for the take, implement and verify it first. Moving symbols does not establish
rubber-band wire reconnection: arrange them before wiring, or visibly repair and
recheck affected connections afterward.

Pace this as a person using the app. Allow readable pauses and actual tool
feedback. Keep an uncut real-time master, including the launch and project
creation. Any shorter edit must retain the requested sequence and disclose cuts
or speed changes. Do not end with an enclosure or imply automatic board sync.

## Recording and true 60 fps

Apple's built-in `simctl io recordVideo` worked headlessly, but its local help
has no frame-rate option and it produced variable-rate footage. A full Retina
take with the explicit 60 Hz recorder averaged about 57.6 fps, so that take was
retained only as a review copy. Lowering only the encoder output size did not
remove the source rendering load.

The selected path uses Meta's MIT-licensed `sim-video` at pinned commit
`36ba98251ba558bca2cc706da503b9ba5095c21d`, with the checked-in Intel/timestamp
[patch](../../tools/capture/idb-intel.patch), on the custom 1x framebuffer. The
timestamp patch uses measured acquisition intervals, keeping GPU conversion
latency out of presentation times while preserving actual capture gaps. It does
not replace elapsed time with a frame counter. The recorder stays outside the app
and release package. See [setup and recording commands](../../tools/capture/README.md).

The 1x transition preflight sustained 60 Hz after recorder warm-up: conversion
to a 60 fps delivery grid dropped 15 surplus samples and duplicated **zero**.
Requesting 60 fps or seeing 60 fps metadata alone does not establish compliance.
Check the complete take as well as this short preflight. Static holds naturally
repeat images; inspect actual movement for a slower source repeated twice.

After verifying orientation and source cadence, export with:

```sh
bash tools/capture/export_video.sh \
  build/demo-imac/take/master.mp4 \
  build/demo-imac/delivery/corvcad-demo-60fps.mp4
```

The helper defaults to removing five seconds of recorder warm-up. The final
take uses `TRIM_START_SECONDS=23.2` to omit test installation and the custom
Simulator's garbled Springboard wallpaper; it starts as CorvCAD opens. There are
no internal cuts. The helper resets the timestamp origin, rotates once and
selects actual source samples onto an exact 60 fps grid. Its
`fps=60` filter is accepted only when FFmpeg reports **zero duplicated frames**;
there is no optical interpolation or speed change. A nonzero duplicate count
returns failure and leaves a review artifact. Keep the original acquisition
log, timestamp analysis and raw master to distinguish sample selection from
upconverting slower footage. Inspect moving frames and any source-overlap
("torn") warnings as well; zero encoder drops does not by itself prove image
quality.

## Handoff and completion

Keep the raw master, correctly oriented 60 fps delivery MP4, landscape PNGs of
the home screen, completed schematic and final PCB, capture logs and frame-time
analysis under `build/demo-imac/`. Preserve the newly authored project folder,
its netlist and ERC/DRC reports, plus the host/build/capture settings used.
Reopen the exported schematic and board with installed KiCad tools, check the
six-component netlist and run ERC/DRC there as an independent file check.

Completion requires the entire visible story, a consistent saved project, a
fully connected PCB, and measured landscape/60 fps evidence. A simulator video
does not establish physical-iPad performance, electrical bench validation or a
released native edition. No such result is claimed by this recording.

## Implementation and verification notes

The end-to-end rehearsal exposed a native schematic routing defect: the LED's
light-arrow artwork enlarged its obstacle box past the cathode tip, preventing
a wire from leaving that pin. The fix opens only a narrow outward escape from
the symbol body and retains the pin as an obstacle. Regression coverage checks
all four rotations, blocked inward routes and other wires crossing the pin.

The private iMac build linked the native desktop proof (634 objects) and built
the Intel simulator package/app. All seven portable engine tests passed; the
verification copy's KiCad text fixtures needed LF normalization after transfer
from Windows. All eleven audit tests passed on both Windows and macOS. The
refreshed source inventory verifies 1718 source/recipe records, ten patches,
166 installed dependency records and 137 notices. Source sets and all four
closure profiles are unchanged; prior dependency/link-map/notice evidence was
retained, as recorded in `audit/verification.json`.

Desktop `kicad-cli` 10.0.7 independently checked the UI-authored project against
the pinned libraries: zero ERC violations, zero DRC violations, zero unconnected
items and zero schematic parity issues. The app's native engine remains pinned
to KiCad 10.0.6. The complete UI take asserts seven successful routes and ends
on the PCB.

## Recorded result (2026-10-27)

The full `testAuthorCircuit` take passed in 389.586 seconds with no failures.
The master is 398.35 seconds at 834 × 1210 portrait pixels. After warm-up,
23,759 actual samples averaged 60.4017/s; median and 99th-percentile gaps were
16.667 ms, maximum 20 ms, with no gaps above 25 ms. The recorder reported no
encoder drops, write failures or encode errors, and two steady-state push
overruns. It also flagged 2,181 source-surface seed changes during reads; those
are overlap warnings, not proven visible tears. Representative decoded app
frames and the final PCB were inspected; do not describe this as a mathematical
proof of every frame being tear-free.

The clean delivery, `corvcad-ipad-demo-60fps.mp4`, is **1210 × 834, 60/1 fps,
22,509 frames, 375.15 seconds (6:15)**. FFmpeg selected 22,509 of 22,659 input
samples after the 23.2-second lead-in trim: 150 dropped, **zero duplicated**.
The final shot is the routed PCB. No internal editing, interpolation or speed
change was applied. The raw master is retained separately.

A separate 85-second continuous-pinch test passed against the saved project.
Decoded canvas differences showed movement updating at varying rates, including
a 60-change/s interval and intervals near 30/s. This establishes 60 Hz capture
with real moving samples, not a guarantee that every gesture produces sixty
distinct app redraws each second. Preserve `motion-proof.json` with the capture
evidence rather than equating the MP4's frame-rate label with UI performance.

The generated delivery directory contains the video, project archive, native UI
logs, independent ERC/DRC/netlist/connectivity reports, source cadence metrics,
probe JSON and representative screenshots. It is intentionally under ignored
`build/demo-imac/`; the reproducible tools and this record belong in source.

# Headless iMac demo capture

The storyboard, circuit and acceptance checks are in
[the capture plan](../../docs/history/IPAD_DEMO_CAPTURE.md). The UI driver is
[`FromScratchDemoTests`](../../app/UITests/FromScratchDemoTests.swift), enabled
only with `TEST_RUNNER_CAPTURE_DEMO=1`. Its full take uses the app UI to create
the project and stops on a failed step. A completion marker stops recording on
the PCB before XCTest teardown closes the app.

`setup_idb.sh` builds Meta's MIT-licensed
[`sim-video` recorder at 36ba982](https://github.com/facebook/idb/tree/36ba98251ba558bca2cc706da503b9ba5095c21d/Tools/VideoRecorder).
Its [MIT notice](https://github.com/facebook/idb/blob/36ba98251ba558bca2cc706da503b9ba5095c21d/LICENSE)
stays with the tool checkout. The local patch selects Intel builds and link
stubs, and timestamps frames from measured acquisition intervals rather than
the time GPU conversion finishes. Actual capture gaps remain gaps. Keep this
tool outside the app and release packaging.

On the Intel iMac with Xcode 26.2, XcodeGen, Git, Python 3 and FFmpeg:

```sh
bash tools/capture/setup_idb.sh
SIM_UUID=$(python3 tools/capture/create_simulator.py)
export SIM_UUID
xcrun simctl boot "$SIM_UUID"
xcrun simctl bootstatus "$SIM_UUID" -b
```

The helper copies the installed iPad profile into the current user's Simulator
profiles and creates a new device. Its **custom 1x framebuffer** is 834 × 1210;
the app retains the iPad's logical size and landscape layout. Identify it as a
reduced-resolution simulator capture, without claiming Retina or device
performance. No Mac desktop, unlocked GUI session or desktop recording grant
is needed. Existing devices and Apple profiles are preserved.

Build the native edition and tests with the current KiCad package and packed
libraries, following [the native build](../../docs/KICAD_CORE_PORT.md), then
prepare landscape without opening a project:

```sh
xcodebuild build-for-testing -project app/CorvCAD.xcodeproj -scheme CorvCAD \
  -destination "id=$SIM_UUID" -derivedDataPath build/DerivedData \
  NATIVE_KICAD=YES ENABLE_DEBUG_DYLIB=NO ENABLE_TESTABILITY=NO CODE_SIGNING_ALLOWED=NO
TEST_RUNNER_CAPTURE_DEMO=1 xcodebuild test-without-building \
  -project app/CorvCAD.xcodeproj -scheme CorvCAD -destination "id=$SIM_UUID" \
  -derivedDataPath build/DerivedData -parallel-testing-enabled NO \
  -only-testing:CorvCADUITests/FromScratchDemoTests/testPrepareLandscape
bash tools/capture/record_demo.sh build/demo-imac/take
```

Set `KICAD_PACKAGE`, `KICAD_SYMBOLS` and `KICAD_FOOTPRINTS` as build settings if
using non-default package paths. `CAPTURE_TOOL` and `DERIVED_DATA` override the
recorder and build paths; `DEMO_PROJECT` sets the name typed through the UI.
Use a fresh dedicated simulator, or a new project name, for another take.
Output directories must be new. The scripts never erase projects or overwrite
prior takes.

The master uses the simulator's portrait framebuffer even while the app is in
landscape. Verify a decoded frame, then use the single correct rotation for
delivery. Retain the master, capture statistics, UI result bundle and saved
project. An explicit `--fps 60` request still needs source-timestamp and motion
checks; see the acceptance criteria in the plan.

After checking the first usable app frame, export and independently check the
saved project (copy it out of the simulator's app data container first):

```sh
TRIM_START_SECONDS=23.2 bash tools/capture/export_video.sh \
  build/demo-imac/take/master.mp4 build/demo-imac/delivery/demo.mp4
python3 tools/capture/validate_project.py '/path/to/LED Switch' \
  build/demo-imac/validation --symbols "$KICAD_SYMBOLS" --footprints "$KICAD_FOOTPRINTS"
```

`TRIM_START_SECONDS` defaults to 5 for recorder warm-up. The recorded take used
23.2 to also omit test installation and the custom simulator's garbled Springboard
wallpaper, starting as the app opens. Choose the trim from decoded footage for
each take. The export refuses existing outputs and fails acceptance if FFmpeg
duplicates any frames. It retains its encoding log and probe JSON. Review actual
motion and source-overlap warnings separately; the check cannot prove that the
app redraws on every captured frame.

For a continuous-motion probe against an existing project, run only
`testMotionPreflight` with `TEST_RUNNER_CAPTURE_DEMO=1` and
`TEST_RUNNER_DEMO_PROJECT='LED Switch'`, while recording separately. This helper
opens the saved project through a launch shortcut; it is never part of the
from-scratch take.

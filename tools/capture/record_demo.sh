#!/bin/bash
# Run on the iMac after building the NATIVE_KICAD app and UI tests for this simulator.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SIM=${SIM_UUID:?Set SIM_UUID to the dedicated booted capture simulator}
TOOL=${CAPTURE_TOOL:-"$ROOT/build/capture-idb/Build/Products/Release/sim-video"}
DERIVED=${DERIVED_DATA:-"$ROOT/build/DerivedData"}
PROJECT=${DEMO_PROJECT:-"LED Switch"}
OUT=${1:?Pass a new output directory}
[ -x "$TOOL" ] || { echo "Capture tool missing: $TOOL" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"
OUT=$(cd "$OUT" && pwd)
cd "$ROOT"
RECORDER=
TESTPID=
stop_recording() {
  if [ -n "$RECORDER" ]; then
    kill -INT "$RECORDER" 2>/dev/null || true
    wait "$RECORDER" || true
    RECORDER=
  fi
}
cleanup() {
  stop_recording
  if [ -n "$TESTPID" ] && kill -0 "$TESTPID" 2>/dev/null; then
    kill -TERM "$TESTPID" 2>/dev/null || true
    wait "$TESTPID" || true
  fi
}
trap cleanup EXIT
"$TOOL" record --set "$HOME/Library/Developer/CoreSimulator/Devices" --udid "$SIM" \
  --fps 60 --scale "${CAPTURE_SCALE:-1}" --avg-bitrate 8000000 --duration 1800 \
  "$OUT/master.mp4" > "$OUT/capture.log" 2>&1 &
RECORDER=$!
sleep 5
kill -0 "$RECORDER" 2>/dev/null || { echo 'Recorder stopped during warm-up; inspect capture.log.' >&2; exit 1; }
TEST_RUNNER_CAPTURE_DEMO=1 TEST_RUNNER_DEMO_PROJECT="$PROJECT" TEST_RUNNER_SNAPSHOT_DIR="$OUT" \
  xcodebuild test-without-building -project app/CorvCAD.xcodeproj -scheme CorvCAD \
  -destination "id=$SIM" -derivedDataPath "$DERIVED" -parallel-testing-enabled NO \
  -resultBundlePath "$OUT/demo.xcresult" \
  -only-testing:CorvCADUITests/FromScratchDemoTests/testAuthorCircuit \
  > "$OUT/ui-test.log" 2>&1 &
TESTPID=$!
while kill -0 "$TESTPID" 2>/dev/null; do
  [ ! -f "$OUT/take-complete.txt" ] || break
  kill -0 "$RECORDER" 2>/dev/null || { echo 'Recorder exited during the take.' >&2; exit 1; }
  sleep 0.2
done
stop_recording
wait "$TESTPID"
TESTPID=
[ -f "$OUT/take-complete.txt" ] || { echo 'Required circuit/PCB steps did not finish; keep this as a failed take.' >&2; exit 1; }
trap - EXIT
printf 'Complete UI take saved to %s; validate its source cadence before delivery.\n' "$OUT"

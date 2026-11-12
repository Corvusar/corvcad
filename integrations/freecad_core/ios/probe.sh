#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$1" && pwd)
SDK=${2:?iphoneos or iphonesimulator}
ARCHITECTURE=${4:-arm64}
TARGET=$(python3 "$HERE/target_config.py" compiler-target "$SDK" "$ARCHITECTURE")
STAGE="$ROOT/$SDK/stage"
EXTRA_LIBS=(-lz -lbz2)
if [ -n "${3:-}" ]; then
  DEPS=$(cd "$3" && pwd)
  EXTRA_LIBS=("$DEPS/lib/libffi.a" "$DEPS/lib/libz.a" "$DEPS/lib/libbz2.a")
fi
xcrun --sdk "$SDK" clang -target "$TARGET" -isysroot "$(xcrun --sdk "$SDK" --show-sdk-path)" \
  -O2 -I"$STAGE/include/python3.13" "$HERE/python_probe.c" "$STAGE/lib/libpython3.13.a" \
  "${EXTRA_LIBS[@]}" -lsqlite3 -liconv -framework Foundation -framework UIKit -o "$ROOT/$SDK/python-probe"
xcrun vtool -show-build "$ROOT/$SDK/python-probe" > "$ROOT/$SDK/macho-build.txt"
xcrun size "$ROOT/$SDK/python-probe" > "$ROOT/$SDK/binary-size.txt"
otool -L "$ROOT/$SDK/python-probe" > "$ROOT/$SDK/dynamic-links.txt"
python3 "$HERE/binary_report.py" "$ROOT/$SDK/python-probe" "$SDK" "$ARCHITECTURE"
if [ "$SDK" = iphonesimulator ]; then
  DEVICE=$(python3 "$HERE/simulator.py" "$ROOT/$SDK/simulator.json")
  trap 'xcrun simctl shutdown "$DEVICE" >/dev/null 2>&1 || true; xcrun simctl delete "$DEVICE" >/dev/null 2>&1 || true' EXIT
  xcrun simctl boot "$DEVICE"
  xcrun simctl bootstatus "$DEVICE" -b
  xcrun simctl spawn "$DEVICE" "$ROOT/$SDK/python-probe" "$STAGE/lib/python3.13" "$ROOT/$SDK/python-probe.json"
  python3 -c 'import json,sys; assert json.load(open(sys.argv[1]))["passed"] is True' "$ROOT/$SDK/python-probe.json"
fi

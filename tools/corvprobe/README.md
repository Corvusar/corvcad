# CorvProbe: gate 4 measurements on a physical iPad

A development-only LiveContainer tweak (`CorvProbe.m`) plus a desktop client (`gate4.py`). It is never part of a CorvCAD release: it opens a LAN port. It follows the guest-tweak route in the LiveContainer feasibility audit. Stock LiveContainer loads signed dylibs from its Tweaks folder into guest apps; the tweak stays inert in any app other than `org.corvcad.CorvCAD`.

## What it measures

- **Memory:** process footprint sampled every 100 ms, the kernel's peak footprint, and memory still available to the app.
- **App:** bundle and executable size, device model and iPadOS version.
- **Fonts:** whether the app can list and read `/System/Library/Fonts`.
- **Files:** upload, list and download under the app's Documents.
- **Logs (`/log`):** the app's own os_log entries (OSLogStore, current process) and the last 2,000 lines of its stdout and stderr. KiCad's and wxWidgets' messages go there. The tweak redirects both through a pipe at load and still passes them on to the original stderr.
- **Screen (`/snapshot`):** a PNG of the app's key window, for checking the UI remotely.
- **Engine runs (`/run`):** the app opens a project's schematic with its own session backend (KiCad's model in the `NATIVE_KICAD` edition) and returns KiCad's netlist, ERC report and BOM with timings, and plots it to PDF and SVG (`CC_SCH_PLOT`). It also parses a board and, with KiCad's board code, runs DRC and writes the fabrication outputs, the board plot, Board Setup and the ratsnest (`fab`, `board-plot`, `board-setup`, `ratsnest`). Written files go to `Documents/Diagnostics/` and every reply carries the app's memory footprint. This needs an IPA built with the `CORVCAD_DIAGNOSTICS` Swift condition; pass `'SWIFT_ACTIVE_COMPILATION_CONDITIONS=$(inherited) CORVCAD_DIAGNOSTICS'` to `xcodebuild`.

## Build

The **CorvProbe tweak** GitHub workflow (manual) builds an ad-hoc signed arm64 `CorvProbe.dylib` and pushes it to the `ci/corvprobe` branch. Locally on a Mac:

```sh
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -dynamiclib -fobjc-arc \
  -framework Foundation -framework UIKit -framework OSLog -install_name @rpath/CorvProbe.dylib -o CorvProbe.dylib tools/corvprobe/CorvProbe.m
codesign -f -s - CorvProbe.dylib
```

## Install on the iPad (LiveContainer)

1. Install the diagnostics IPA (`CorvCAD-KiCad-<rev>-diagnostics-unsigned.ipa`) in LiveContainer as usual. It replaces the earlier CorvCAD; keep its data.
2. In LiveContainer's **Tweaks** tab, import `CorvProbe.dylib` and let LiveContainer sign it with its certificate. Either put it in the global tweak folder or create a folder for it.
3. In CorvCAD's app settings in LiveContainer, select that tweak folder, if you did not use the global one.
4. Launch CorvCAD. On first launch the tweak writes `corvprobe.token` in CorvCAD's Documents, visible in Files → CorvCAD. Read the 8-character token from it.
5. The first incoming connection may trigger iPadOS's local-network prompt; allow it.

## Run

From the desktop on the same network:

```sh
py -3 tools/corvprobe/gate4.py <ipad-ip> <token> build/gate4.json
```

It uploads KiCad's demos ecc83, complex_hierarchy, video and the Jetson AGX Thor baseboard to `Documents/Gate4/`. It compares the device netlist (pin groups and names) and ERC with `kicad-cli` here, and the BOM, schematic plot and fabrication outputs with the desktop references (`integrations/kicad_core/session/bom_reference.json`, `fab_reference.json`), and writes memory, timings, size and font results to the JSON. Keep CorvCAD in the foreground while it runs; iPadOS suspends background apps.

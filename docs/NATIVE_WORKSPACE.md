# Combined native workspace integration

The combined KiCad/FreeCAD edition is in development. The default edition and
the separately accepted native editions remain the working configurations.
`NATIVE_KICAD=YES NATIVE_FREECAD=YES` builds a combined app; its x86_64
simulator UI tests pass and the unsigned arm64 app links (below). ~~Device execution and physical iPad results are open.~~ **UPDATE 2026-11-14:** a combined diagnostics IPA ran on a physical iPad Air (M2) on 2026-10-31 (KiCad parity through the diagnostics hook and the FreeCAD STEP gate); the touch UI on a device and a release build are open. F4 acceptance does not
establish combined-engine acceptance or physical iPad validation.

## Measured starting point

The two accepted dependency recipes pin vcpkg commit
`9e593bb18ea69cc5095e012465dcd675a822ed0d`. A read-only inventory of the Intel
iMac caches found 63 overlapping target packages with matching versions and
features, but different ABI hashes for every overlapping package. Their
triplet files differ, including explicit target architecture and GNU configure
host settings. Matching versions alone are insufficient evidence for reuse.

There are also dependencies outside those manifests:

- KiCad's measured closure includes vendored fmt 12.1.0; FreeCAD uses fmt
  12.2.0. Both use the `fmt::v12` namespace. All consumers must use one reviewed
  header/library version; removing a duplicate archive alone does not fix this.
- wxWidgets' bundled zlib and PNG symbols have `wx_zlib_` and `wx_png_` prefixes.
  Its bundled Expat exports ordinary `XML_*` symbols and collides with the
  shared target Expat. A new wx build should use the common target Expat through
  upstream's `--with-expat=sys`, and verify the resulting link inputs.
- The current KiCad target inventory contains no Eigen package. Retain the
  reviewed FreeCAD Eigen 3.4.0 override for the union, then rebuild consumers
  against the same headers. This is not evidence that all future KiCad slices
  are compatible with that version.

The raw archive scan also found two unprefixed internal globals in CPython's
bundled Expat, and wx's bundled PCRE2 10.49 differs from the union PCRE2 10.47.
The combined recipe therefore uses `build_python.py --system-expat --deps
<union-prefix>` and `wx_build.py --system-regex` in new output roots. These
upstream configuration options select the common Expat and PCRE2 providers;
the original standalone defaults remain available. Python's build claim records
its provider and archive/header hashes. The F4 session hook carries shared Expat
through both command probes and the package's original raw link inputs.

Do not link every installed archive indiscriminately. Full protobuf and
`utf8_validity` replace their alternate lite/range archives; unneeded HarfBuzz
sub-libraries and Boost.Container introduce additional overlapping globals.
Any exclusion must be checked against actual remaining consumers' undefined
symbols, and all selected raw archives must still pass the strong-symbol scan.
Archive ordering is not a collision fix.

The first complete combined preflight found two more exported implementation
globals, `nsvg__parseXML` and `nsvg__colors`, in both KiCad's modified NanoSVG
and wx's inline NanoSVG. Their public APIs differ, so these forks cannot be
substituted for one another. The wx recipe prefixes those two private names
through compiler definitions; public wx/NanoSVG declarations and the pinned
source tree remain unchanged. The new wx receipt records this adaptation.
Rebuild wx, recheck the actual KiCad-used configuration headers and rerun the
whole selected archive preflight before linking.

The source evidence is the pinned manifests, KiCad's native source inventory,
vcpkg's installed status/ABI records, and actual archive symbol tables. These
observations describe the inspected caches, not an accepted combined build.

The first engine rebuild passed wx and both standalone FreeCAD C ABI simulator
fixtures against this shared prefix. KiCad's first pass found one include-contract
change: fmt 12.2's `core.h` no longer exposes `fmt::format` by default, whereas
KiCad's bundled 12.1 header did. The union KiCad recipe now selects fmt's public
`FMT_DEPRECATED_HEAVY_CORE=1` compatibility option. Its source headers and linked
implementation still both come from the common fmt 12.2 package. Recompile and
retain the new dependency evidence before accepting that native core.

The plotting translation unit also uses a narrow GPL stand-in for the desktop
plot-dialog header, which refers to wx controls unavailable on iOS. This reuses
the reviewed existing iOS adaptation, applies only to `pcbnew/pcbplot.cpp`, and
retains the native plotting functions. The supplemental header hash/licence
is recorded separately from the unchanged standalone KiCad inventory.

## Build boundaries

`integrations/native_workspace/` owns the union recipe and its evidence. It
must not overwrite the standalone FreeCAD or KiCad inventories. Use a fresh
build directory and separate vcpkg installed roots for each SDK: a manifest
install can otherwise remove the other SDK's packages from a shared root.

The intended link consists of the KiCad core, the FreeCAD session and nine
registered FreeCAD core archives, one common vcpkg dependency closure, the
reviewed wx inputs, CPython and OCCT 7.8.1. Force-load only the engine registration
archives. Do not combine the old `libcorvcad_kicad_deps.a` and
`libcorvcad_freecad_deps.a` archives. Retain all source licences and dependency
notices; the combined native distribution is GPL-3.0-or-later.

The KiCad union build must first verify the existing audited baseline. Compile
the measured native source closure with the common fmt headers, omit the old
vendored fmt implementation, and record actual compiler dependency files.
The resulting supplemental source/header inventory, commands, hashes and
licences require review before accepting the new closure. A passing baseline
audit does not automatically approve changed target headers.

Rebuild FreeCAD and CPython against the common prefix, including CPython's
recorded libffi, zlib and bzip2 inputs. Reuse an existing OCCT build only after
its source, target, configuration and archive checks pass. Never infer ABI
compatibility from a directory name or package version.

The initial Intel simulator dependency invocation is:

```sh
python3 integrations/native_workspace/deps.py \
  --root build/native-workspace/deps \
  --sdk iphonesimulator --architecture x86_64 --jobs 2
```

`--plan` verifies the pinned inputs without creating directories. An optional
`--reference /path/to/existing/vcpkg` borrows Git objects during cloning and
then dissociates; it never copies another installed dependency prefix. The
completed SDK directory contains `native-dependencies.json`, the installed
prefix and standalone inventory/Mach-O receipts. Only proceed when that
receipt reports that all target archives passed verification.

`wx_build.py` consumes that receipt, the pinned wx source tree and original
checksummed tarball. It configures a new output directory with system Expat,
records the actual compiler dependencies and verifies target archives.
`kicad_build.py` consumes the receipt, that new wx build and a freshly generated
desktop probe build's Ninja commands. It selects the committed measured native
profile, replaces fmt, compiles fresh target objects with dependency files and
emits a supplemental `kicad-build.json`. Both commands require an absent output
directory and preserve existing builds. Their `--help` lists explicit paths;
neither accepts objects or merged dependencies from an earlier iOS sweep.

After an interruption, `kicad_build.py --resume` may reuse its own matching
output directory. Each completed unit has an atomic checkpoint binding the
exact command, source, object hash and all actual compiler dependency hashes.
Missing or stale checkpoints cause recompilation; older uncheckpointed attempts
are never adopted. A different recipe/toolchain/input claim requires a new root.

For the FreeCAD side, use a fresh per-SDK root whose `deps` entry points to
this completed SDK directory. Clone the pinned source into `source-ios/FreeCAD`
and replay `prepare_ios.py prepare`. Reuse independent copies of a verified
same-target OCCT stage/receipt and same-version host CPython build, then rebuild
target CPython with `build_python.py --deps <union-target-prefix>`. The existing
`session/build.py`, `ios/stage_runtime.py` and `session/package.py` can then use
the new root. `cross.inputs` checks that CPython's libffi/zlib/bzip2 hashes match
the union inventory. Do not copy the old target interpreter, FreeCAD objects or
CMake cache. Run the standalone session probes before attempting composition.

Build one SDK first and measure remaining disk space. The initial iMac has
limited spare disk and 8 GB RAM; a second cold dependency build can exhaust
that margin. Existing accepted caches and other agents' checkouts are retained.

The first simulator bootstrap failed while linking the host `protobuf:x64-osx`
tools: Apple Clang found Homebrew's newer Abseil headers before the imported
vcpkg include directory, despite selecting the pinned vcpkg libraries. The
host-only overlays now set `CMAKE_NO_SYSTEM_FROM_IMPORTED=ON`, making those
imported headers ordinary `-I` inputs ahead of Clang's implicit Homebrew path.
Both host recipe hashes are pinned and recorded; the iOS triplets are unchanged.
Preserve the failed root and use `--root build/native-workspace/deps-host-isolated`
for this retry. A helper test checks the actual CMake-generated include ordering;
the complete host/target build still has to pass before issuing its receipt.

On the iMac, add `--clean-buildtrees-after-build` to the dependency invocation.
This removes source/configuration/object subdirectories after each successful
port, retaining top-level port logs and ABI records. Installed files, downloads
and failed-port trees remain. It is a recorded storage policy, so a stopped
owned build can resume with it without changing its compiled-input claim.
The first host-isolated attempt was interrupted deliberately to enable this
policy before the larger ports; its log/exit are retained separately.

## Archive preflight

`combine.py` checks raw static archives for SDK, architecture, iOS minimum and
duplicate strong external symbols. It rejects the two old bundled dependency
archives and conflicting builds with the same archive name. Identical inputs
are selected once. For example, on the Mac:

```sh
python3 integrations/native_workspace/combine.py \
  --sdk iphonesimulator --architecture x86_64 \
  --output build/native-workspace/archive-preflight.json \
  /path/to/union/lib/libexpat.a /path/to/new-wx/lib/libwx_baseu_xml.a
```

This is a preflight, not a package builder or acceptance receipt. Weak C++
definitions still require matching source/header ABIs. A successful scan
does not establish that two engines can initialize and operate together.

`select_archives.py` measures the fixed variant exclusions against every
retained archive's strong/weak exports and undefined symbols. A new consumer
of an omitted unique export fails selection. It never strips objects or adds
a collision allowlist. `verify_inputs.py` rechecks the original raw archives,
actual compiler inputs, shared prefix and runtime package. If wx was rebuilt
with a different internal regex provider after KiCad compilation, every wx
build header actually included by KiCad must still match byte-for-byte,
including its configuration header; otherwise KiCad must be recompiled.

After the fresh shared-Expat Python/FreeCAD and shared-PCRE2 wx builds pass,
the development mixed link can be prepared on the Mac:

```sh
python3 integrations/native_workspace/compose.py \
  --dependency-receipt "$DEPS/native-dependencies.json" \
  --wx-receipt "$WX/wx-build.json" \
  --kicad-receipt "$KICAD/kicad-build.json" \
  --freecad-root "$FREECAD" --freecad-package "$FREECAD/package/sim" \
  --sdk iphonesimulator --architecture x86_64 --output "$NEW_LINK"
python3 integrations/native_workspace/probe/run.py \
  --binary "$NEW_LINK/mixed-probe" \
  --runtime "$FREECAD/package/sim/FreeCADRuntime" \
  --architecture x86_64 --output "$NEW_RUN"
```

Both outputs must be new directories. The compositor retains selection,
preflight, actual compiler dependency closures, commands, logs, link map and
binary hashes, including failed attempts. It force-loads engine registration
archives and links selected dependencies once without merging them. A passing
`link-recipe.json` establishes only the probe link. The owned-simulator runner
must then require both fresh processes to exit successfully and preserve all
original project/runtime bytes after exit. See
[`probe/README.md`](../integrations/native_workspace/probe/README.md).

Native KiCad models borrow their `PROJECT` from its settings manager. The
adapters permit initial project activation only until the first schematic or
board model has loaded; later project loads retain the manager's existing
projects for the process lifetime. This includes a standalone schematic that
uses the initial dummy project. KiCad's ordinary active-project switch deletes
the previous project, so using it to reopen a board or schematic copy would
invalidate another live model. The mixed probe checks the original schematic
after those copy operations. A fresh native rebuild and both x86_64 simulator
orders passed at `1d29565`; this establishes no physical iPad result.
It does not establish support for simultaneous schematic models sharing one
`PROJECT`, whose schematic settings are also shared. The current regression
opens distinct project copies.

The retained [combined simulator evidence](../integrations/native_workspace/evidence/imac-mixed-simulator.json)
records the 631-unit KiCad rebuild, 4,544 actual source/header inputs, zero
conflicting strong globals and a real combined link with only system dynamic
dependencies. Both fresh processes passed interleaved edits, history,
saved-copy reopen and exact STEP/STL exports. All original project bytes and
1,722 runtime files remained unchanged; simulator shutdown and deletion passed.
Inputs, archives and outputs were independently rehashed after completion.
The empty fixture project
file does not establish preservation of unsupported fields in edited files.

## Combined app (x86_64 simulator, 2026-10-29)

The app's build guard is lifted. With `NATIVE_KICAD=YES NATIVE_FREECAD=YES`,
`app/project.yml` links the response file `compose.py` wrote
(`NATIVE_WORKSPACE_LINK`, its `mixed-link.rsp`) instead of either single-engine
package, and bundles `FreeCADRuntime` from the workspace FreeCAD package
(`FREECAD_PACKAGE`). The single-engine and default links are unchanged.

A fresh 631-unit KiCad core from the merged source (`kicad-sim-005`), the
recomposed link and both probe orders passed again. The Debug app links only
system libraries. In one simulator install, `NativeKiCadTests` (router, library
symbols and footprint chooser, DRC, ERC with findings marked, netlist export)
and both `NativeFreeCADTests` orientations passed. The default edition's
screenshot tests and `EnclosureUndoTests` passed against the same source. See
the [combined app evidence](../integrations/native_workspace/evidence/imac-combined-app-simulator.json).
The app tests open separate projects per engine; same-project interleaving is
covered by the C ABI probe only.

## Acceptance still required

Items 1 and 2 below are retained; the simulator part of 3 and 4 passed as above.
Still open: the standalone KiCad-only and FreeCAD-only native regressions on
this source and same-project editing through the app UI. The arm64 union
(dependencies, shared-PCRE2 wx, 631-unit KiCad core, shared-Expat CPython and FreeCAD)
and an unsigned Release arm64 combined app link passed on 2026-10-29 with only system
libraries and the FreeCAD runtime, KiCad libraries and notices bundled
([arm64 evidence](../integrations/native_workspace/evidence/imac-combined-app-arm64.json)).
~~That app has not run on a device.~~ **UPDATE 2026-11-14:** a combined diagnostics IPA (`f9b86a6`, built on the iMac) ran on a physical iPad Air (M2) on 2026-10-31; see the [combined probe pass](KICAD_CORE_PORT.md).

1. One reproducible union recipe, checked target package features/ABI hashes,
   actual source/header closure, archive hashes and complete notices.
2. A combined simulator link with no non-system dynamic dependencies and no
   conflicting strong globals. Exercise both engine initialization orders.
3. Schematic and PCB editing plus exact enclosure recompute, undo/redo,
   save/reopen and internal exports in the same project. Confirm that changing
   one engine's model does not discard the other model's unsupported data.
4. Default, KiCad-only and FreeCAD-only regressions, then an unsigned arm64
   combined app link with verified runtime resources. Label simulator results
   and device-target builds accurately.

Physical F5 remains separate. The device preparation work can proceed without
an iPad, but physical memory, cold start, interactive performance and desktop
reimport/slicer evidence must come from actual device exports. Public iPad
STEP controls stay gated until that validation succeeds.

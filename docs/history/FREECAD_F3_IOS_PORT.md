# FreeCAD F3 static iOS port

Started 2026-10-25 by Codex after the accepted F2 desktop input audit.
**F3 accepted 2026-10-26** at native commit `3bfde68`: static FreeCAD links for
arm64 simulator and device; both unchanged F1 enclosure proofs pass on an iPad
simulator with nine built-in modules, desktop geometry parity and measured
size/RSS/recompute. See the [simulator receipt](../../integrations/freecad_core/ios/evidence/codemagic-simulator-accepted.json)
and [device link receipt](../../integrations/freecad_core/ios/evidence/codemagic-device-accepted.json). The default
engine and app do not use this directory. App integration and physical iPad
execution remain F4/F5.

The [initial Python probe](../../integrations/freecad_core/ios/evidence/python-initial.json) passed on an iPad
simulator and linked for arm64 device in run
[40192622311](https://github.com/Corvusar/corvcad/actions/runs/40192622311).
The [mirror Python evidence](../../integrations/freecad_core/ios/evidence/python-mirror.json) also confirms the newer
Mach-O/file-size/RSS checks. Static FreeCAD compilation/model tests remain
separate. The [F3 handoff](FREECAD_F3_HANDOFF.md) records that
boundary and the working runner supplied by Claude.

## Run

Requires an Apple silicon Mac with full Xcode and an installed iOS simulator
runtime. Both targets use arm64 and the app's iOS 17 minimum:

```sh
python3 -m unittest discover -s integrations/freecad_core/ios -v
python3 integrations/freecad_core/ios/build_python.py --sdk iphonesimulator
bash integrations/freecad_core/ios/probe.sh build/freecad-ios/python iphonesimulator
python3 integrations/freecad_core/ios/build_python.py --sdk iphoneos
bash integrations/freecad_core/ios/probe.sh build/freecad-ios/python iphoneos

brew install autoconf autoconf-archive automake libtool pkgconf ninja
bash integrations/freecad_core/ios/deps.sh build/freecad-ios/deps arm64-ios-simulator
bash integrations/freecad_core/ios/deps.sh build/freecad-ios/deps arm64-ios
python3 integrations/freecad_core/ios/dependency_inventory.py build/freecad-ios/deps/installed arm64-ios build/freecad-ios/deps/inventory-arm64-ios.json
```

The manual **Build and test** workflow accepts `mechanical_ios=true` and
`mechanical_stage=all|python|dependencies|static-desktop|occt|ios`, calling `mechanical-ios.yml` on the
selected branch. Normal manual CI retains its previous default. No push/PR/tag
triggers are added. Evidence artifacts contain logs, package/ABI inventories and
licence notices; they do not contain an IPA or native runtime distribution.

## Interpreter boundary

CPython 3.13.7 comes from the checksum-verified official source archive in
`inputs.json`. The host interpreter comes from that same source; its extension
modules are built as well, because the bytecode compiler imports `_struct`.
Cached source edits or archive corruption fail rather than being overwritten.

[Upstream's iOS build](https://github.com/python/cpython/blob/v3.13.7/iOS/README.rst)
requires framework configuration. The experiment uses that configuration and
builds `libpython3.13.a` with `MODULE_BUILDTYPE=static`; nested stdlib convenience
archives are flattened into the staged archive. The probe links that archive
and registers a **synthetic** module with `PyImport_AppendInittab` before
initializing an isolated interpreter. It requires `sys.platform == 'ios'` and
built-in origins for essential stdlib extensions. It checks the linked binary's
dynamic dependencies and retains Mach-O/size reports. The device executable is
linked but not run; a device link is not a physical iPad test.

The runtime stage includes Python sources and unchecked-hash bytecode,
plus CPython's complete `LICENSE`. Signal installation, user site packages and
bytecode writes are disabled in the probe. This stage is not the final app
resource policy: optional stdlib modules, host-only resources, subprocess entry
points and the FreeCAD resource allowlist still need explicit review/tests.

## Sharing dependencies with KiCad

This follows the owner's F3 card comment: avoid duplicate/conflicting libraries
when the native engines eventually share one app. `vcpkg.json` uses the **same
registry commit as KiCad**, with matching deployment target and static linkage.
Qt is built from pinned vcpkg recipes with Core, Concurrent, Network and Xml;
Gui/Widgets/OpenGL and default extras are not selected. This replaces the
proposal's prebuilt Qt package for the experiment so the selected features and
archive inputs can be measured together. The baseline contains Qt 6.11.1;
compatibility with FreeCAD still needs a source-build result.

Eigen is explicitly overridden to 3.4.0#5: the shared registry's Eigen 5 default
does not satisfy FreeCAD's Eigen 3 finder. ICU is required by FreeCAD Base/App.
The target triplets supply a distinct Darwin/iOS Autoconf host for every package: the
baseline's identical arm64 Darwin build/host names otherwise cause configure to
try running an iOS executable on macOS. This is a cross-configuration change,
not an emulated test result. The first `darwin-ios` spelling was rejected by
upstream `config.sub`; the candidate uses a versioned Darwin host spelling
instead. The compiler's `-target` and SDK still determine iOS deployment/ABI.
The target dependency builds, inventories and strict archive checks passed for
both SDKs in run 40215845149, including same-version host-only Qt Linguist tools.
The [retained inventories](../../integrations/freecad_core/ios/evidence/dependencies-mirror.json) record 85 target
core/feature records and 43 archives per SDK, with 1,427 checked Mach-O build
versions per target. Feature records refer to their core package's version/ABI.
The earlier run 40214097907 predated archive-member validation, which then found
Qt's pre-project autodetection selecting arm64+x86_64 universal archives. The
triplets now pass `VCPKG_OSX_ARCHITECTURES=arm64` before that detection. Every
archive must pass `--verify-target` (architecture, SDK and minimum iOS version).
These builds do not establish an iOS FreeCAD link.
OCCT is not taken from this registry, whose OCCT 8 is outside the reviewed
FreeCAD 1.1.4 dependency range. CPython and OCCT are separate dependency work.

`dependency_inventory.py` retains actual per-feature versions/ABIs, archive
content hashes and copyright hashes. It rejects missing ABIs and target dynamic
or GUI libraries. **A shared version is insufficient**: reuse requires the same
SDK, architecture, deployment target, registry, features and ABI. F4's combined
app link must consume each common archive once. In particular, do not link two
independently assembled monolithic dependency archives and assume the linker's
symbol selection establishes compatibility. Nothing currently changes KiCad's
dependency package or claims that app-level sharing has been completed.

## Static FreeCAD candidate

`static-profile.json` locks the F2 base profile, the additional 13-file source
patch, each edited file before/after and `static_registry.cpp`. Run
`prepare_static.py prepare` in a fresh source checkout, or `verify` on an already
prepared checkout. Both reject changed/untracked inputs without resetting or
overwriting them. The pinned upstream stores some files with CRLF even in Git;
context whitespace is ignored during patch application only after complete
normalized before hashes agree. Complete after hashes must agree too.

The opt-in patch changes Base/App and seven selected modules to static archives,
registers their real `PyInit_*` entry points before FreeCAD initializes Python,
and retains Qt resource/type-registration objects through the Apple linker.
It omits the external-interpreter `FreeCAD.so` wrapper. This profile also
explicitly disables Measure, whose default remained enabled in the measured F2
closure but is not required by the enclosure model. The accepted F2 inventory
continues to describe its own actual closure.

The `static-desktop` stage provides an intermediate macOS development check:
configure with `prepare_static.py configure`, build, then `run_static.py`.
It requires actual static CMake artifacts and built-in import origins, rejects
FreeCAD/GUI dynamic links, and repeats the upstream/F1 regressions. Dependencies
remain desktop libraries in this intermediate check. A pass there would not
establish an iOS static dependency link. Source preparation/idempotence and
preservation on rejection have been exercised locally. The native build linked
in run 40213726645 after explicitly propagating OCCT's TBB dependency through
static Import. Bundled E57Format, which upstream builds even with Points
disabled, is inventoried alongside the core/module archives; unknown libraries
remain rejected. Run 40214097907 then passed all nine built-in module imports,
669 upstream tests (3 skipped), and both unchanged F1 enclosure proofs.
[Retained evidence](../../integrations/freecad_core/ios/evidence/static-desktop-mirror.json) records the actual
23,628,024-byte executable; its dependencies remain desktop libraries.

## OCCT target build

`build_occt.py --sdk iphonesimulator|iphoneos --jobs 3` builds checksum-pinned
OCCT 7.8.1 independently of vcpkg. All module switches are off; the selected
FreeCAD roots and their measured dependencies select 35 toolkits. The unchanged
STEP/XCAF path requires visualization data/presentation toolkits (`TKService`,
`TKV3d`, `TKVCAF`; FreeCAD also lists `TKMeshVS`). OpenGL/GLES drivers, Draw and
viewer executables are excluded. This is more precise than claiming all
Visualization code is absent. Every produced archive must match the exact
toolkit closure, arm64 SDK and minimum deployment target, with source/archive
hashes and upstream notices retained. Both SDK builds passed in run 40215029789;
[retained input evidence](../../integrations/freecad_core/ios/evidence/ios-inputs-mirror.json) records their actual
35-toolkit archives, source inventory digest and notices.

The `occt` manual stage runs both SDK builds. Its first configure exposed
CMake 4's removal of old policy compatibility; the build now requests a 3.5
policy minimum without modifying OCCT sources.
The first compile exposed a provider header dependency absent from upstream
`EXTERNLIB`: STEP/IGES/glTF/STL providers include `BinXCAFDrivers.hxx`. `TKBinXCAF`
is therefore an explicit root, and the build now checks provider header owners
against its closure before compiling.

## Complete iOS development check

The explicit `ios` stage builds dependencies, CPython with the same target
libffi/zlib/bzip2 archives, OCCT, and the static FreeCAD slice for both SDKs.
`all` retains the smaller independent development checks. `cross.py` verifies
input inventories and rejects host dependency headers or the wrong SDK in
target compilation commands.

The separately hashed `ios-profile.json`/`0002-ios-runtime.patch` adapts host
Python discovery, isolated interpreter initialization and final SDK linkage.
`prepare_ios.py` uses `build/freecad-ios/source-ios/FreeCAD`, preserving the
separate static desktop source checkout. The interpreter reads its explicit
bundled stdlib, disables user sites, environment discovery, signal handlers and
bytecode writes, and rejects process creation through a CPython audit hook.
Target-generated sysconfig data is staged instead of host configuration.

`stage_runtime.py` checks the source profile and reviewed resources, retains
notices and per-file hashes, compiles bytecode with matching host CPython, and
makes runtime files read-only. `run_ios.py` checks actual SDK/system-only linkage.
For simulator it creates its own iPad instance, runs both unchanged F1 fixtures,
compares accepted desktop geometry, measures recomputes/peak RSS, and checks
that runtime files stayed unchanged. Device validation only checks the link.
The native results below establish F3 acceptance; tool existence alone does not.
OCCT's static targets omit transitive toolkit/system linkage. The iOS patch
therefore links the complete measured toolkit list, rejects missing/shared or
unexpected toolkit exports, and explicitly links the required SDK runtimes.

The first complete pipeline passed target dependencies, shared-archive Python
and both OCCT builds, then stopped at missing host SWIG during FreeCAD configure.
The workflow now installs SWIG, Bison and Flex explicitly. The retry at
`c35da63` was rejected before allocation because the temporary PMO mirror's
billing/spending capacity is blocked. Runner recovery is on a separate card for
Claude to claim; source implementation remains with Codex.

Manual `ci.yml` accepts `mechanical_runner_labels` as a JSON array, defaulting
to `["macos-15"]`. After Claude documents an available dedicated Mac, supply its
actual registered labels (for example `["self-hosted","macOS","ARM64"]` if
those labels match that runner). The label array propagates to every F3 job,
including the reusable desktop static check. The direct mechanical workflows
expose the equivalent `runner_labels` input.

## Dedicated Codemagic builds

Claude provisioned a separate FreeCAD Codemagic account after both GitHub
mirrors exhausted their hosted capacity. The manual `freecad-ios` (simulator)
and `freecad-ios-device` workflows in
[`codemagic.yaml`](../../codemagic.yaml) uses an Apple silicon `mac_mini_m2`
and the latest installed Xcode. Select `FREECAD_SDK=iphonesimulator` or
`iphoneos` using its matching workflow; first run `FREECAD_STAGE=inputs`, then
`full`. The complete inputs checkpoint checks dependencies and Python/OCCT,
but supplies no FreeCAD compilation or runtime result. Each SDK has a separate
workflow/cache. A dependency-only stage remains available for diagnostics.
The full simulator run adds both unchanged F1 proofs and measurements; the
device run establishes linkage only. All runs retain logs and inventories,
without publishing an app or native runtime.

The API credential is `CODEMAGIC_TOKEN_FREECAD` in the owner's ignored settings;
keep it outside workflow files, comments and artifacts. App
`6adef14098c06b067e3a725c` is the owner's replacement GitHub-connected app.
Its simulator inputs build `6adef17be831eaabc1c966b6` at `c9892f1` allocated an
Apple silicon runner and fetched the branch successfully. This supersedes the
old manually added app `6adeece620318fecfb09e027`, whose first dispatch failed
repository authentication before checkout/allocation. Input and full build
acceptance still require their actual completed results.

The last allocated GitHub build, `40229142464`, reached the FreeCAD build step
and exposed missing Boost.Signals2 headers. The manifest now also supplies the
other missing modular Boost roots found in the measured F2 closure: Convert
(`MeshIO.cpp`), Format (`ExportOCAF2.cpp`), Geometry (`WireJoiner.cpp`), Iostreams
(`Base/Writer.cpp`), ScopeExit (`App/Application.cpp`) and UUID (`Part/Geometry.h`).
Their transitive ports cover the other selected Boost headers. The expanded
simulator manifest passed in Codemagic build `6adef17be831eaabc1c966b6`:
123 package/feature records, 50 archives and 1,601 Mach-O build-version checks.
[Retained reports](../../integrations/freecad_core/ios/evidence/codemagic-dependencies-simulator.json) include
archive/ABI/notice identities. The device target still needs a fresh inventory.

That inputs build passed all 90 source/runtime regressions, then failed
compiling CPython with Xcode 26.6/SDK 26.5. CPython's link-only `dup3`/`pipe2`
feature checks find exported symbols, but iOS headers omit their declarations.
The target configuration now explicitly disables those optional APIs, verifies
that generated `pyconfig.h` leaves both macros undefined, and records the
configuration hash/overrides. Upstream `dup2`/`pipe` fallbacks remain in use;
host Python configuration is unchanged. The simulator probe now checks a
pipe round trip, `dup2` and non-inheritable descriptors. Simulator inputs build
`6adf082b1977246ed8d507da` at `31bed0d` passed these native checks, all 92 local
boundary regressions, the dependency inventory and all 35 OCCT toolkits under
Xcode 26.6/SDK 26.5. Its [retained reports](../../integrations/freecad_core/ios/evidence/codemagic-inputs-simulator.json)
record the actual probe/configuration and archive identities. The Python probe
was 8,945,120 bytes with 69,828,608 bytes RSS; these are interpreter-only
measurements. FreeCAD compilation and enclosure proofs were deliberately skipped
in this inputs stage. The full simulator run is `6adf0ebd2a1ca3ca958af6cf`;
its results remain separate.

That full run passed the same inputs and reached FreeCAD compilation, then
stopped at `WildMagic4/Wm4System.cpp`: it included the macOS-only Carbon header
on all Apple targets, although its only Carbon-dependent code already excludes
arm64 and x86_64. The iOS patch now applies that existing legacy-Apple guard
to the include as well. Timing and path behavior are unchanged. A preprocessor
regression poisons Carbon for 64-bit targets while retaining the legacy branch;
the patch/profile records the additional file and its before/after hashes.
The later accepted full simulator/device runs validate this correction.

Codemagic also caches OCCT's pinned source and per-SDK build/stage directories.
Start an `inputs` checkpoint once to populate those paths before the next full
run. Subsequent runs still verify source integrity, regenerate CMake settings,
build as needed and repeat archive/SDK/notice checks. This avoids repeatedly
compiling the unchanged OCCT inputs while iterating on FreeCAD source failures.
Compiler-cache statistics are retained for diagnosing cache effectiveness.

Full run `6adf263856623e06590fd97c` at `c0a20f4` passed all 93 regressions and
compiled the FreeCAD sources, including the Carbon correction. Its final link
failed on `_qt_main_wrapper`: Qt Core's iOS interface selects the UIKit entry
point by default. The iOS static command target now sets Qt **6.11.1's lowercase
`qt_no_entrypoint` property**, retaining its own `main`. Qt's other targets and
platforms remain unchanged. CMake regression fixtures evaluate the actual
patched block against the pinned Qt consumer-target predicate. The
[link-attempt evidence](../../integrations/freecad_core/ios/evidence/codemagic-freecad-link-initial.json) records
the failed link separately from native runtime acceptance.

The cache statistics exposed why OCCT still rebuilt: Codemagic retains the
first successful cache and skips later uploads while it exists. The simulator
was still restoring dependency-only cache `6adf0820f0b14d8a1ee72019`; adding
paths and running a successful inputs job did not refresh it. An attempted
refresh during inputs build `6adf360303b536bee40af5a7` also failed: although
the server cache was revoked after restoration, cleanup still reported an
existing cache and skipped upload. That build passed all 96 tests and its
input checks, but the coordinator stopped before the full build because no
replacement cache appeared. Revoke only the matching workflow cache **before
starting** a fresh complete inputs job, then verify its upload and the next
restore. Do not rely on in-flight revocation to change upload behavior.
The device workflow uses a separate cache and should also be primed with a
complete inputs job. Verify the upload and subsequent restore in the actual
build logs before claiming cache reuse. See the upstream
[cache behavior](https://docs.codemagic.io/knowledge-codemagic/caching/) and
[cache API](https://docs.codemagic.io/rest-api/caches/).

Fresh-cache inputs run `6adf421569d659fbf5cd0306` passed at `1c31a69`, saving
a 1,750,388,295-byte replacement cache. Full simulator run
`6adf5238f9c0ffcb479103ad` reused it; OCCT's verified stage took 26.87 seconds.
Following simulator/device checks continue after source
success even if cache publication is unavailable; caching is an optimization
and must not block native acceptance work. Cache state and warnings are still
recorded.

The full run compiled and linked FreeCAD, confirming the entry-point correction,
then failed resource staging: upstream `configure_file` adds the final newline
missing from `part.py` (also `partdesign.py` and `sketcher.py`). The resource
checker now permits only CMake newline handling for explicitly selected
configured helpers, retaining strict byte equality for other resources. Actual
CMake regression fixtures reject content changes and extra blank lines. The
actual pinned resource selection passed local staging with a synthetic
binary/stdlib; the later accepted full run validates native runtime execution. See the
[compile/link and staging receipt](../../integrations/freecad_core/ios/evidence/codemagic-freecad-staging-initial.json).

## Accepted F3 results

Full simulator run `6adf5ba69da373ce40d9741a` at `3bfde68` passed all 97 runner
regressions, native FreeCAD compilation/link, staging and both unchanged F1
proofs. Nine modules resolve as built-ins. Both fixture reports pass desktop
geometry parity, STEP reimport, separate STEP/STL exports, bores, persistence
and parameter recomputes. The read-only runtime remained unchanged and its
owned iPad simulator was deleted. The [accepted simulator receipt](../../integrations/freecad_core/ios/evidence/codemagic-simulator-accepted.json)
records independently rechecked output/proof hashes and measurements:
91,861,192-byte executable; 123,428,595-byte staged runtime; maximum process
RSS 155,664,384 bytes. Demo/roundtrip recompute totals were 2.952944/5.386766
seconds over 40/41 calls. These are development simulator measurements.

Device inputs run `6adf6414b797b507c1146d45` passed at the same commit, including
the expanded 123-record/50-archive dependency inventory, CPython and 35 OCCT
toolkits. Its [receipt](../../integrations/freecad_core/ios/evidence/codemagic-inputs-device.json) does not establish
a FreeCAD device link: the inputs stage deliberately skips it. Final device
run `6adf997fc5102ac14cbfaa4f` passed all 97 regressions, complete FreeCAD
compile/link, resource staging and arm64 IOS/minimum 17.0 metadata checks with
system-only dynamic links. The [accepted device receipt](../../integrations/freecad_core/ios/evidence/codemagic-device-accepted.json)
records a 91,986,392-byte executable and 123,552,613-byte staged runtime across
1,711 files. The downloaded artifact's manifest, byte totals, source/profile,
staged proof and dependency identities were independently checked.

F3 is complete. F4 adds the session C ABI/app edition; F5
requires a physical iPad result before any iPad STEP availability claim.

## Intel simulator validation

Build tools accept `--architecture x86_64` for `iphonesimulator` only. Arm64
remains the default and the sole device architecture. Use a separate build root,
such as `build/freecad-ios-intel`, and dependency triplet `x64-ios-simulator`.
Host generators use the Mac's native triplet (`x64-osx` on Intel). Pass the
architecture through Python, OCCT, cross-build, staging and runtime validation;
the CPython `probe.sh` takes it as its fourth positional argument. SDK and
architecture markers reject incompatible caches. A successful arm64 F3 receipt
does not validate this new Intel lane. See the [F4 continuation](FREECAD_F4_HANDOFF.md#intel-imac-continuation).

# FreeCAD F3 acceptance and handoff

Codex owns implementation on branch `codex/freecad-f3`, stacked on F2 branch
`codex/freecad-f2`, in PR #2. Board F3 card: `cm6piwuo1ftygzno5o8rrm9gddc`.
**F3 accepted 2026-10-26** at native commit `3bfde68`: both arm64 SDK builds,
simulator geometry proofs and measurements, and final device-target link pass.
This document retains the implementation history below; earlier failed or
pending runs describe their state at that time. F4 app integration and F5
physical iPad validation remain separate.

## Established results

- Official checksum-verified CPython 3.13.7 builds a static archive with built-in
  stdlib extensions for both arm64 iOS SDK targets. Host Python comes from the
  same source; target bytecode is precompiled.
- Claude completed runner access on the owner-approved temporary private CI mirror.
  Run 40203833358
  at `cba1c04` passed the newer Mach-O/file-size/RSS checks on both arm64 SDK
  targets, including actual simulator execution. Its
  [verified artifacts](../../integrations/freecad_core/ios/evidence/python-mirror.json)
  record an 8,582,592-byte simulator probe and 40,583,168 bytes of interpreter
  RSS. These are not FreeCAD geometry size/memory measurements.
- [Run 40192622311](https://github.com/Corvusar/corvcad/actions/runs/40192622311),
  commit `b32726ddf885f2e0badc6acd5160ff49f20e0f86`, linked the device executable
  and **ran** the interpreter/synthetic inittab probe on an iPad simulator.
  `sys.platform` was `ios`; all 13 checked stdlib extensions were built-ins.
  Xcode 16.4/SDK 18.5 built it; the installed simulator runtime was iOS 26.2.
  [Retained evidence](../../integrations/freecad_core/ios/evidence/python-initial.json)
  records artifact hashes, pins and the limited measurement scope.
- The separate static FreeCAD source candidate applies to the pin on top of F2
  patches. Preparation, reentry, source-edit rejection and untracked-input
  rejection passed on a separate complete local checkout. Its
  [source report](../../integrations/freecad_core/ios/evidence/static-source-preparation.json)
  is not compilation evidence.
- Focused local tests pass, including temporary Git repositories with CRLF source
  blobs, source preservation, archive integrity, simulator selection and static
  artifact/dependency boundaries. The original F2 source/generator/licence/
  dependency verification still passes.

## Candidate ready for native testing

[`ios/`](FREECAD_F3_IOS_PORT.md) has the separately hashed
13-file static source patch and registry. It converts Base/App and seven
extensions to static libraries, retains Qt resource/registration objects, and
registers real `PyInit_*` functions before Python initializes. The external-
Python `FreeCAD.so` wrapper and unneeded Measure module are omitted.
`run_static.py` requires actual static artifacts, built-in import origins,
no FreeCAD/GUI dynamic links, and upstream/F1 regression success. The intermediate
macOS check uses desktop dependencies and cannot establish iOS acceptance.

KiCad's vcpkg baseline is reused, following the owner's F3 comment. Headless Qt
6.11.1, common dependencies and Eigen 3.4.0#5 are selected. Actual package ABI,
archive and notice inventories are required before app-level sharing. Matching
versions or two independently combined dependency archives are insufficient.

Dependency runs exposed ICU's identical Darwin host/build triplets on Apple
silicon. `darwin-ios` was rejected by `config.sub`; the current candidate uses
a versioned Darwin host to force cross configuration while compiler target/SDK
controls iOS ABI. The correction now applies to every target package after
libffi and libb2 hit the same issue. Both target builds compiled in
run 40213408732.
The inventory step then failed because feature records have no independent
version/ABI; the corrected parser requires their installed core package.
Same-version Qt Linguist tooling is added as a host-only package, keeping GUI
libraries out of the target package set.
Initial target builds/inventories passed in run 40214097907.
[Retained evidence](../../integrations/freecad_core/ios/evidence/dependencies-mirror.json)
now records the later strict pass in run 40215845149: 85 core/feature records,
43 archives and 1,427 actual Mach-O build-version checks per SDK. Run 40215385354
exposed Qt's arm64+x86_64 universal archive selection. Commit `7a2cbd4` explicitly
sets `VCPKG_OSX_ARCHITECTURES=arm64` before Qt's pre-project detection.

The static desktop executable linked in
run 40213726645
after propagating the OCCT-enabled TBB dependency through Import. The artifact
gate then found upstream's always-built E57Format archive; the corrected gate
records it alongside Base/App and the seven extension targets. Run 40214097907
then passed all nine built-in imports, 669 upstream tests (3 skipped), and both
unchanged F1 proofs.
[Retained evidence](../../integrations/freecad_core/ios/evidence/static-desktop-mirror.json)
records the 23,628,024-byte macOS executable with desktop dependencies. This
establishes static module development acceptance, not an iOS runtime result.

`build_occt.py` now pins OCCT 7.8.1 and selects the 35-toolkit closure for
the unchanged FreeCAD slice. STEP/XCAF requires visualization data/presentation
libraries; OpenGL/GLES drivers, Draw and viewer executables are absent. Every
archive is checked for architecture/SDK/minimum version and content identity.
The first native configure required a CMake 4 policy compatibility setting;
the next compile exposed `BinXCAFDrivers.hxx` in four providers, a dependency
missing from upstream EXTERNLIB. `TKBinXCAF` is now an explicit root; a provider
header-owner check rejects omitted dependencies before compilation. Both OCCT
SDK builds passed in run 40215029789. Their
[retained evidence](../../integrations/freecad_core/ios/evidence/ios-inputs-mirror.json)
contains archive identities, notices, SDK checks and source-inventory digests.

## Runner handoff to Claude

Claude completed **FreeCAD F3: provide an Apple silicon macOS runner (Claude)**,
`c394odspf6tn1bjxyahirfpduzc`, using `macos-15` on the owner-approved private
CI mirror. Codex retains source implementation ownership.

New Actions jobs stopped before runner allocation. The
[40193022919 annotation](https://github.com/Corvusar/corvcad/actions/runs/40193022919)
reports failed account payments or a spending limit needing increase. The
documented `corvmac` SSH alias also fails DNS resolution and is documented as
Intel. Use the verified private mirror rather than repeating those allocations.

The mirror credential is `GITHUB_TOKEN_PMO` in the owner's ignored local settings.
Use it only through the approved environment/secret store. Sync the current
`codex/freecad-f3` commit to the mirror without rewriting unrelated branches,
then start **Build and test** with `mechanical_ios=true` and these stages:

1. `mechanical_stage=python`: fresh Mach-O/file-size/RSS checks added after the
   initial pass; Claude's mirror run passed them. `size`'s virtual
   `__PAGEZERO` total is not executable file size.
2. `mechanical_stage=dependencies`: validate the ICU correction and complete
   both static dependency builds. Partial binary caches survive package failures.
3. `mechanical_stage=static-desktop`: compile the static FreeCAD candidate and
   run its import/upstream/F1 checks before adding iOS cross-build adaptations.
4. `mechanical_stage=occt`: build and inventory the pinned geometry/STEP closure
   independently for simulator and device.
5. `mechanical_stage=ios`: complete both SDK pipelines through target Python,
   OCCT, FreeCAD, reviewed runtime staging and the simulator F1 proof. Device
   execution is deliberately absent; it only establishes target linkage.

Run 40214097907
tests the latest bookkeeping corrections, target sysconfig staging and host Qt
tools at `9c6a2a1`. Inspect the actual jobs/artifacts before claiming acceptance.
Its Python probe step hit Apple's Bash 3.2 empty-array/nounset behavior, corrected
in `ba9102d`; the independent Python retry is run 40214326792.

The new iOS source/profile, cross configuration and runtime harness pass local
checks, including real CMake host/target discovery. Windows skips the native
interpreter embedding test because clang is absent; macOS CI runs that test.
The combined iOS source lives separately in `source-ios/FreeCAD`; never apply
its patch to the desktop static tree or weaken either profile's source checks.

Full pipeline 40215029789
at `876fb1c` passed the source/runtime checks and target Python built against
the actual shared libffi/zlib/bzip2 archives and both OCCT SDK builds. FreeCAD
configuration then stopped at missing host SWIG. The workflow now installs
SWIG/Bison/Flex, and `c35da63` also validates/links the complete measured static
OCCT toolkit set (its static exports omit transitive linkage).

## CI recovery and current build status

The retry 40227581009
at `c35da63` was refused before allocation: zero steps, `runner_id=0`, and the
check-run annotation reports failed payments or a spending limit. The PMO
mirror now has the same billing class of failure as origin; do not keep
dispatching hosted jobs until its state changes.

At the owner's explicit request, Codex created **FreeCAD F3: restore Apple
silicon runner capacity (Claude)**, card `cgmib3ctyc7di3e7mg77y9tu97c`, completed
after Codemagic allocation and all 90 source/runtime tests passed. The original
completed
runner card remains a record of the working lane it supplied.

`ci.yml` now accepts `mechanical_runner_labels`, a JSON array defaulting to
`["macos-15"]`. Once Claude documents a working dedicated runner, pass its actual
labels; all F3 reusable jobs honor that input. Direct mechanical workflows use
`runner_labels`. Run `mechanical_ios=true`, `mechanical_stage=ios` on the latest
`codex/freecad-f3` commit. Preserve/use existing compiler and package caches.

A second private CI mirror allocated an Apple silicon
runner in Actions run 40229142464
at `9521f8b`. The FreeCAD build step exposed missing Boost.Signals2 headers. The
manifest now includes Signals2 plus six further modular roots required by the
measured F2 source/header closure: Convert, Format, Geometry, Iostreams,
ScopeExit and UUID. This expanded manifest needs fresh target inventories;
the earlier dependency receipts remain evidence for their recorded inputs.

Both GitHub mirrors have now exhausted hosted capacity. Claude supplied a
separate FreeCAD Codemagic account, app `6adeece620318fecfb09e027`, with
`CODEMAGIC_TOKEN_FREECAD` in ignored local settings. The manual `freecad-ios`
workflow uses `mac_mini_m2` and `xcode: latest`. Run one SDK at a time using
`FREECAD_SDK=iphonesimulator|iphoneos`; first use `FREECAD_STAGE=dependencies`
to save a successful package cache, then `inputs` for Python/OCCT, then `full`
for FreeCAD compile/runtime checks.
An inputs-only success is explicitly not a FreeCAD result.

The API accepted the first simulator inputs build, `6adeeed0f4ad9d171cacd708`,
but it failed before checkout or allocation: repository authentication rejected
by GitHub. The app reports a manually added generic repository with no username
and authentication disabled. Codex handed this concrete authentication failure
back to Claude on the existing recovery card.

The owner replaced that app with GitHub-connected app
`6adef14098c06b067e3a725c`. Simulator inputs build
`6adef17be831eaabc1c966b6` resolved `c9892f1`, allocated the Apple silicon
runner and fetched sources successfully. All 90 regression tests and the
expanded simulator dependency inventory passed: 123 package/feature records,
50 archives and 1,601 actual Mach-O build-version checks. Their
[retained reports](../../integrations/freecad_core/ios/evidence/codemagic-dependencies-simulator.json)
preserve ABI/archive/notice hashes. The device inventory remains outstanding.
CPython then failed under Xcode 26.6/SDK 26.5: its generic link-only feature
checks enable `dup3`/`pipe2`, which iOS headers do not declare. OCCT and FreeCAD
never ran in this build.
Use the replacement app ID for subsequent dispatches.

The correction sets `ac_cv_func_dup3=no` and `ac_cv_func_pipe2=no` for target
configure only, selecting CPython's existing `dup2`/`pipe` fallbacks. It verifies
the generated header leaves both macros undefined before compiling (a macro
defined as zero would still enable upstream's `#ifdef` branches). Configuration
hash and overrides are recorded. The simulator probe now exercises pipe data,
`dup2` and non-inheritable descriptors. The dependency-only cache checkpoint
protects expensive inputs from a later independent stage failure.

Simulator inputs build `6adf082b1977246ed8d507da` at `31bed0d` passed all 92
boundary regressions, the expanded dependency inventory, corrected native
CPython compilation and actual simulator probe, and all 35 OCCT toolkits under
Xcode 26.6/SDK 26.5. The [retained reports](../../integrations/freecad_core/ios/evidence/codemagic-inputs-simulator.json)
verify the disabled feature probes, shared Python dependency archive hashes,
built-in module origins, POSIX descriptor fallbacks, OCCT source inventory
digest and actual SDK archive checks. The interpreter-only probe executable
was 8,945,120 bytes and its measured RSS was 69,828,608 bytes. These are not
FreeCAD geometry measurements. The inputs stage intentionally skipped FreeCAD
compilation and runtime checks. The sequence started full simulator build
`6adf0ebd2a1ca3ca958af6cf`. It passed the inputs, then stopped during FreeCAD
compilation at `WildMagic4/Wm4System.cpp` (789/1,416 build steps): the Carbon
header is included on every Apple target even though its only use is already
guarded out on arm64/x86_64. The iOS source patch now gives the include the same
legacy-Apple guard and records this 24th combined source edit. No timing/path
implementation change is needed. The regression preprocesses the actual patch
against a poisoned Carbon header for 64-bit targets and checks that legacy
Apple handling remains available. Both automatic sequences stopped on the
failure, so no device build was dispatched.

The next `inputs` checkpoint will populate newly cached OCCT source and per-SDK
build/stage directories; later native source iterations can reuse those builds
while repeating source, archive, SDK and notice verification. Compiler-cache
statistics are retained as build artifacts. Native validation of the Carbon
correction and the full FreeCAD link/runtime still remain outstanding.

Run `6adf263856623e06590fd97c` at `c0a20f4` passed all 93 native-runner
regressions and compiled the FreeCAD sources, including the Carbon fix. It
then failed at the final `FreeCADCmd` link: Qt Core transitively selects
`EntryPointPrivate`, adding `-Wl,-e,_qt_main_wrapper` for UIKit. The headless
harness uses its own `main`. The iOS static command target now sets lowercase
`qt_no_entrypoint`, the spelling used by pinned Qt 6.11.1; CMake fixtures check
its target/platform scope and preservation of other executables. This is not
the uppercase property introduced in Qt 6.12. [Retained link evidence](../../integrations/freecad_core/ios/evidence/codemagic-freecad-link-initial.json)
records source/target checks and the precise acceptance boundary.

OCCT still rebuilt because Codemagic kept the original first-success cache;
the successful warm run explicitly skipped uploading the newer paths. Both
warm/full runs restored the same 1,266,578,999-byte cache
`6adf0820f0b14d8a1ee72019`. The attempted in-flight refresh did not work:
inputs build `6adf360303b536bee40af5a7` restored it, passed all 96 regressions
and inputs, but cleanup still skipped cache upload even after server revocation.
The cache API was empty, and the coordinator stopped before dispatching full.
The cache must be absent before a new priming build starts; do not repeat the
in-flight revocation approach.
The manual `freecad-ios-device` workflow now separates device caching from
`freecad-ios`; prime each with complete `inputs` before `full`. Existing cache
entries are immutable until revoked or expired, so a dependencies-only warmup
must be refreshed before it can retain OCCT/Python inputs.

Fresh-cache inputs build `6adf421569d659fbf5cd0306` passed at `1c31a69` and
saved replacement cache `6adf5226f0b14d8a1ea7c233` (1,750,388,295 bytes).
Full simulator build `6adf5238f9c0ffcb479103ad` reused it: OCCT's complete
verified build stage took 26.87 seconds. The revised local sequence treats cache status as diagnostic:
successful inputs unlock full simulator and device checks independently of
cache publication. It still stops on source/runtime failure or a changed
revision. Active state is selected by the ignored
`build/freecad-core/codemagic/active-f3-chain.json` pointer.

The full simulator build compiled and linked `FreeCADCmd`, confirming the Qt
entry-point correction. It stopped during resource staging, before interpreter
startup: pinned `Ext/freecad/part.py` lacks a final newline, which upstream
`configure_file` adds. The staging checker now permits only CMake's line-ending
normalization and final newline for the explicitly selected configured helpers.
Other resource families still require byte-for-byte equality. A regression runs
actual CMake and rejects content edits and extra blank lines; staging the actual
pinned resource selection also passed locally with a synthetic binary/stdlib.
[Retained evidence](../../integrations/freecad_core/ios/evidence/codemagic-freecad-staging-initial.json)
records the successful compile/link and separate staging failure. No native
imports or geometry proof ran; device checks were not dispatched after failure.

Full simulator retry `6adf5ba69da373ce40d9741a` at `3bfde68` passed all 97
runner regressions, native compilation/link, corrected staging, all nine
built-in FreeCAD imports and both unchanged F1 enclosure proofs on an arm64
iPad simulator running iOS 26.5. STEP reimport, separate tray/lid STEP and STL,
valid solids, volume/bounds/bores, FCStd persistence and all eight parameter
recomputes match the accepted desktop checks. The
[accepted simulator receipt](../../integrations/freecad_core/ios/evidence/codemagic-simulator-accepted.json)
retains reports, hashes, exact scope and measurements. Export hashes and proof
assertions were independently rechecked after download against Git blobs at
the native commit (rather than Windows checkout line endings). The runtime
manifest records 1,711 files; the runner verified they remained unchanged and
deleted its owned simulator successfully.

| Measurement | Demo | Roundtrip |
| --- | ---: | ---: |
| Peak process RSS, bytes | 148,766,720 | 155,664,384 |
| Measured recompute calls | 40 | 41 |
| Total recompute time, seconds | 2.952944 | 5.386766 |
| Slowest recompute, seconds | 0.545010 | 0.924064 |

The simulator executable is 91,861,192 bytes. The complete staged runtime is
123,428,595 bytes, including 31,567,403 bytes of resources/notices. These are
development simulator measurements, not installed app or physical iPad figures.

Device inputs build `6adf6414b797b507c1146d45` at the same commit also passed:
123 package/feature records, 50 archives, 105 dependency notices, CPython and all
35 OCCT toolkits. Its [device-input receipt](../../integrations/freecad_core/ios/evidence/codemagic-inputs-device.json)
records actual device SDK metadata and archive identities; this inputs stage
deliberately skipped FreeCAD. The local coordinator ended after its last
14:45 UTC snapshot and did not dispatch full. On resumption Codex checked build
history for duplicates and dispatched the single remaining full device run
`6adf997fc5102ac14cbfaa4f`. It passed all 97 regressions, complete FreeCAD device
compilation/link, reviewed resource staging and actual arm64 IOS/minimum 17.0
Mach-O checks. Only system dynamic dependencies remain. The
[accepted device receipt](../../integrations/freecad_core/ios/evidence/codemagic-device-accepted.json)
records 672 checked compilation commands, 1,711 runtime files, a 91,986,392-byte
device executable and 123,552,613-byte staged runtime. Downloaded manifest,
source/profile, proof-source, dependency and executable identities were
independently checked. This validates device linkage only; physical execution
belongs to F5. Together with the accepted simulator result, it completes F3.

The latest local suite has 97 tests: 95 pass, native embedding and Carbon
preprocessor checks skip on Windows where Clang is absent. Source preparation
and original F2 source-only closure/licence
checks pass. All 97 ran successfully on the full simulator and device runners.

All workflows remain manual and publish no app/native runtime. A dedicated Mac
can use equivalent commands in the README/workflows. Keep credentials in the
approved secret store, outside comments, artifacts and commits.

## Next gates

F3 is complete. F4 adds the mechanical session C ABI, app edition, tessellation
preview and app export workflow. It must verify combined app dependency ABIs
and link each shared dependency once. F5 requires physical iPad execution and
desktop validation of exported files. No iPad STEP availability claim is
justified before those gates pass.

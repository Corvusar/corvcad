# Dependency and format notices

The original MIT build/diagnostic tools under `integrations/native_workspace/`
and `integrations/freecad_core/device/` do not vendor or ship new binaries.
The candidate shared dependency recipe pins the existing registry and adds an
explicit Expat input (MIT) for wx's system-Expat configuration. A combined
native distribution still requires the actual source/header closure, dependency
notices and complete corresponding source described below; its app build is
gated pending validation. See [the integration plan](docs/NATIVE_WORKSPACE.md).

The supplemental `integrations/native_workspace/headless/ios/dialog_plot.h`
is a KiCad-derived GPL-3.0-or-later header with its notice preserved. It omits
the unsupported desktop dialog declaration for the native plotting translation
unit; the actual KiCad plot/export implementation remains linked. Its content
hash and licence are recorded in the supplemental native build receipt.

The combined wx recipe also prefixes two private NanoSVG implementation globals
to keep wx's NanoSVG separate from KiCad's modified fork. Upstream sources and
their retained notices are unchanged; the compiler definitions are recorded in
the wx build receipt.

The isolated [FreeCAD F3 investigation](docs/history/FREECAD_F3_IOS_PORT.md)
also pins official CPython 3.13.7 source (PSF and bundled component terms) and
KiCad's vcpkg registry baseline for experimental iOS dependencies. Its static
FreeCAD patch retains upstream LGPL notices; its new registry/build tools are
MIT. Downloaded source, runtime archives and compiler caches remain in ignored
build directories. No native F3 runtime is distributed. Per-package notices and
ABI/content inventories must be retained before any eventual shared app link.
The same investigation separately pins OCCT 7.8.1's source archive by SHA-256,
retains its LGPL-2.1 text and OCCT exception, and records the actual static
toolkit/archive closure. SDK frameworks used by the development harness come
from the Apple toolchain. Runtime staging keeps reviewed FreeCAD resource and
dependency notices; this is not native app distribution acceptance.

- The base engine has no third-party runtime dependencies beyond a C++20 standard library.
- The iPad app uses Apple SwiftUI, SceneKit, Foundation, and UniformTypeIdentifiers frameworks supplied with the platform SDK. [XcodeGen](https://github.com/yonaskolb/XcodeGen) is an MIT-licensed build-time project generator; it is not shipped in the app.
- Optional desktop STEP export links against [Open CASCADE Technology (OCCT)](https://github.com/Open-Cascade-SAS/OCCT). OCCT has its own LGPL-2.1 license with an additional exception. Distributors enabling that option must include OCCT's license and comply with its terms; no OCCT code is vendored here.
- KiCad board files are read through this project's own limited S-expression parser. KiCad software libraries are not linked or vendored. The desktop bridge invokes an independently installed KiCad CLI. KiCad's source is predominantly GPL licensed; incorporating or linking it in a later iPad port will require a license review and suitable distribution terms. The board fixture is a synthetic design generated for this repository with KiCad 10's PCB writer and embeds three official KiCad footprints. KiCad's [library license](https://www.kicad.org/libraries/license/index.html) is CC-BY-SA 4.0 with an exception for designs using library data.
- The schematic compatibility reader and transaction session are CorvCAD code. The optional desktop `kicad_native` adapter imports an independently installed KiCad 10 `pcbnew` Python module; it does not distribute KiCad binaries. `integrations/kicad_core/upstream.json` pins source inputs.
- KiCad schematic source tag `10.0.6` is pinned to commit `caf7377e9cb6fa1535ec3596dcb8c99bf44a996e` as a **build input under investigation**, with the newer headless source audit retained separately. No KiCad translation unit is linked into the current app. If linked, the distributed combined app and corresponding source must satisfy each incorporated file's license, predominantly GPL-3.0-or-later; preserve the independent MIT notices for CorvCAD-authored files. The repository now holds KiCad-derived GPL files that are **not built into the app**: `integrations/kicad_core/patches/` (a patch series against 10.0.6), `integrations/kicad_core/headless/`. That covers replacements for editor-bound KiCad files and iOS stand-in headers under `headless/ios/` and board replacements under `headless/pcbnew/`, all under their original GPL-2.0-or-later or GPL-3.0-or-later terms, plus `wx_property_choices.cpp`, which copies a few wxWidgets 3.3.4 functions under the wxWindows Library Licence 3.1, the desktop measurement probes in `integrations/kicad_core/probe/`, and the native schematic session in `integrations/kicad_core/session/` (GPL-3.0-or-later, a desktop proof). Their measured compiled-file lists are `closure-10.0.6-headless.txt` (schematic with ERC, 336 files) and `closure-10.0.6-board.txt` (headless board with DRC, 415 files). These lists and the patch series are the inventory a native port must ship as corresponding source. The iOS simulator and arm64 device probes (`probe/ios_link.sh`, not part of the app) also statically links libraries that `integrations/kicad_core/ios_deps.sh` builds with vcpkg (tag pinned in `upstream.json`). Their licences: zstd (BSD-3-Clause or GPL-2.0-only), FreeType (FTL or GPL-2.0-or-later), HarfBuzz (MIT-Modern-Variant), libpng (libpng-2.0), zlib (Zlib), bzip2 (bzip2-1.0.6), Brotli (MIT), Protobuf (BSD-3-Clause), Abseil (Apache-2.0), Boost (BSL-1.0) and GLM (MIT). Shipping them in the app needs these notices.
- The official KiCad symbol repository tag `10.0.6` is pinned to commit `7800d91437ce44e2ed0928f2ad31a287457b8a68`. Its official packer builds all 223 libraries plus a `sym-lib-table`. The `NATIVE_KICAD` edition bundles the packed set in `KiCadSymbols/` (233 MB installed, about 11 MB of the compressed IPA) with `LICENSE-KICAD-SYMBOLS.md`, under CC-BY-SA 4.0 with KiCad's design exception; its Licences screen shows the retained `kicad-symbols` notice. The official KiCad footprint repository tag `10.0.6` is pinned to commit `819223b66f96508feaeaa305301b5e6bb5c1038b`; the same edition bundles its 155 `.pretty` libraries and `fp-lib-table` in `KiCadFootprints/` (160 MB installed) with `LICENSE-KICAD-FOOTPRINTS.md`. Its licence text is byte-identical to the symbol libraries', so the retained `kicad-symbols` notice covers both. KiCad's 3D model library is not bundled (see `docs/3D_MODELS.md`). The default edition bundles no KiCad libraries. `integrations/kicad_core/fetch.py` records SHA-256 hashes of packed files.
- FreeCAD and slicer software are not engine or app dependencies in this release. The isolated `integrations/freecad_core/` F1 tools invoke independently downloaded stock FreeCAD 1.1.4 (LGPL-2.1-or-later, with bundled dependencies under their own terms) and installed KiCad 10 in separate processes. The stock Windows/Linux archives and FreeCAD source commit are pinned in `upstream.json`; downloaded binaries and source checkouts stay in ignored `build/`. No upstream FreeCAD checkout or binaries are vendored or packaged with the app. F2 now retains a two-patch series of FreeCAD-derived CMake and Python adaptations in `integrations/freecad_core/patches/` with per-file LGPL-2.0-or-later grants/defaults retained, and the pinned root licence in `licenses/FREECAD-LGPL-2.1.txt`; source-patch and before/after file hashes are recorded in `slice.json`. The authoring/validation scripts are original MIT-licensed CorvCAD code; they use public Python APIs without copying upstream implementations. Experimental F2 source builds now pass on Linux and macOS. A measured source-input snapshot and 14 source-tree notices are retained under `audit/`; it includes notices for components outside the selected link closure. F2 now retains reviewed version/content dependency locks, generator provenance, source/asset licence catalogues, observed dependency-header declarations and complete dependency notices. Fresh Linux/macOS comparisons pass. The production payload guard excludes 35 reserved-rights assets and two font-related files. Package-wide notices can cover unlinked code; the complete corresponding-source archive and distribution review remain separate release requirements. See [the F2 handoff](docs/history/FREECAD_F2_HANDOFF.md). See [the FreeCAD gates](docs/FREECAD_PORT.md).
- wxWidgets 3.3.4 (wxWindows Library Licence 3.1) is a build input for the desktop probes (Homebrew) and for the iOS simulator and device builds that `integrations/kicad_core/wx_ios.sh` makes from the release tarball pinned by SHA-256 in `upstream.json`. Only the experimental `NATIVE_KICAD=YES` edition links it, through `integrations/kicad_core/probe/ios_package.sh`. That edition bundles the retained notice set from `integrations/kicad_core/audit/notices/` and shows it, with the GPL terms, in its Licences screen. The default build links neither wx nor KiCad, and bundles only CorvCAD's MIT `LICENSE`.

## Measured native proof inventory (2026-10-21)

The authoritative per-file list is [`integrations/kicad_core/audit/sources.json`](integrations/kicad_core/audit/sources.json), with [verification and refresh instructions](integrations/kicad_core/audit/README.md). It covers the schematic, board and session closures, directly linked harness files, compiler-discovered headers, generator inputs and local build scripts. The patch inventory lists every changed upstream path and patch hash. The dependency inventory is [`audit/dependencies.json`](integrations/kicad_core/audit/dependencies.json); the complete retained notice set is protected by [`audit/notices.json`](integrations/kicad_core/audit/notices.json). These expand the earlier translation-unit lists; the unpatched GUI closure is not the selected native release profile.

| Source component | Terms recorded from the pinned source |
| --- | --- |
| KiCad model/common sources | Per-file GPL-2.0-or-later or GPL-3.0-or-later; combined native distribution GPL-3.0-or-later |
| KiCad-derived CorvCAD replacements, native session and C++ probes | Original GPL notices retained; independent engine, tools and C ABI harness retain MIT |
| Clipper2, PEGTL | BSL-1.0 |
| CompoundFileReader, delaunator, dynamic_bitset, fmt, magic_enum, nlohmann/json, picoSHA2, thread-pool | MIT; copied LLVM excerpts in CompoundFileReader additionally retain Apache-2.0 WITH LLVM-exception |
| expected | CC0-1.0 |
| fast_float | MIT OR Apache-2.0 OR BSL-1.0 |
| libpopcnt, nanoflann | BSD-2-Clause |
| NanoSVG | Zlib |
| RTree | GPL-3.0-or-later |
| Zint backend | BSD-3-Clause; Apache-2.0 additions in embedded Arimo font data and GS1 excerpts are retained |
| GLAD generated headers | CC0-1.0 AND Apache-2.0, choosing its offered CC0 alternative; Khronos platform header MIT |
| Lemon parser generator/template | Public-domain dedication; generated grammars retain their KiCad terms |
| KiCad mixed-license headers | ISC triangulation, Apache-2.0 math excerpts, and Apache-2.0 WITH LLVM-exception JSON excerpts in addition to their GPL terms |
| wxWidgets bundled libraries | Expat MIT, IJG JPEG, libpng, libtiff, Zlib, WebP BSD-3-Clause plus patent grant, PCRE2 BSD-3-Clause, NanoSVG Zlib, LunaSVG/PlutoVG MIT; notices retained separately from wxWindows terms |

The vcpkg baseline pins the full port recipes and their dependency source checksums. The audit also records installed package versions, features, ABI fingerprints and per-link archive ownership, so installed-only packages and host build tools are distinguishable from actual link inputs. wx's source archive and all regular extracted files were checked; no wx source patch is declared. The exact 10.0.6 symbol-library license and design exception are retained in `integrations/kicad_core/licenses/KICAD-SYMBOLS.txt` and the audited notices.

## ngspice (KiCad edition builds with NGSPICE_BUILD)

ngspice 46 (pinned by SHA-256 in `integrations/kicad_core/upstream.json`) is built by `integrations/kicad_core/ngspice_ios.sh`. The `NATIVE_KICAD` package links it when the probe build has `NGSPICE_BUILD` (KiCad's simulator, from the schematic inspector). The default edition doesn't contain it. The static library includes XSPICE and its code models, registered by `ngspice/static_codemodels.c` (MIT) instead of being loaded as plug-ins.

[`audit/ngspice.json`](integrations/kicad_core/audit/ngspice.json) lists every ngspice file behind the device library's objects, plus the code-model generator and its inputs, with hashes and licences. Licences come from ngspice's `COPYING`, or from a file's own header where it differs:
- BSD-3-Clause for most files;
- LGPL-2.0 for KLU and LGPL-2.0-or-later for numparam;
- MIT for the sparse solver;
- public domain for XSPICE, except its `table` code models (GPL-2.0-or-later) and `dlmain.c` (BSD-3-Clause, of which only the helpers are compiled);
- Bison-generated parsers under the Bison exception.

OSDI (MPL-2.0), ADMS and the Verilog co-simulation plug-ins (GPL-2.0-or-later) are not in the library. All of this fits the KiCad edition's GPL-3.0-or-later combined work. The notices must join the packaged legal resources before an IPA with ngspice is shared.

## kicad-packages3D-generator (3D part generator, native editions)

`integrations/parts3d/` ports KiCad's 3D model generator (https://gitlab.com/kicad/libraries/kicad-packages3D-generator, GPL-3.0-or-later; the commit is pinned in `integrations/parts3d/upstream.json`) from CadQuery to C++ on OCCT. The construction rules in `parts3d.cpp` follow the generator's chip capacitor, chip resistor, pin header and gull-wing scripts. `generated/kicad_tables.inc` is converted from its `cq_parameters.yaml` tables by `tables.py`. The original authors are credited in the file headers, and the port is GPL-3.0-or-later. It is not part of the MIT `engine/`; the app compiles it (with `parts3d_c_api.cpp`) only in the `NATIVE_FREECAD` edition, against that edition's OCCT. No KiCad 3D model files (`kicad-packages3D`, CC-BY-SA 4.0 with an exception) are bundled; the desktop check downloads some for comparison only.

For a native binary release, retain the notices and publish complete corresponding source for the actual combined build, including local adaptations, generated-source inputs, dependency source and build scripts. An inventory or vcpkg recipe checkout is not by itself that source artifact. The packaging gate supplies the archive and app legal resources; this gate does not change the current MIT base-engine distribution or mark the native IPA shipped.

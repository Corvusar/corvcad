# Gate 1 source and dependency inventory

Captured 2026-10-21 from the macOS proof host after the schematic, board and native session proofs. The source half was last refreshed on 2026-11-05 for KiCad's simulator (patch 0011; native closure 637 units, 1,734 inputs). ngspice, which that build links, is inventoried per file in `ngspice.json` (`ngspice/inventory.py`), and `dependencies.json` points to it. This is an inventory of existing proof targets. App packaging and physical iPad validation have their own gates.

| Profile | Measured archive translation units | Additional directly linked files |
| --- | ---: | ---: |
| Schematic + ERC | 336 | `probe/probe.cpp` |
| Board + DRC | 415 | `probe/pcb_probe.cpp` |
| Native schematic session | 336 | `session/kicad_session.cpp`, `session/abi_test.c`, `engine/src/schematic_route.cpp` |

`sources.json` normalizes the linker maps' host paths. It records each profile's exact closure, the direct harness objects, and 1,585 source, header, generator and build inputs with SHA-256 hashes and license evidence. Of these, 1,522 are pinned KiCad inputs. Compiler dependency records supply the transitive headers, including 18 vendored library components; Lemon is also recorded as a generator. All eight patches are listed in order, with hashes and affected paths. The 16 generated translation units retain their observed desktop hashes; their source schemas, keywords, grammar templates and generation scripts are in the inventory. Generated output is regenerated per host with its matching Protobuf toolchain, rather than compared to the desktop bytes in a source-only check.

`dependencies.json` records the pinned vcpkg baseline, port recipe hashes, installed versions/features/ABIs for 166 package/triplet records, and the archive inputs observed in each simulator/device linker map. Installed-only packages and host build tools are retained for provenance; their presence does not mean their code is shipped. In particular, Python, libgit2, curl, ODBC, Cairo and Pixman were prerequisites of the broad desktop candidate experiment and do not belong to the selected iOS link. Some historical maps predate the Fontconfig replacement; each map records its own inputs.

wxWidgets is pinned by release SHA-256. Capture compares all 10,158 regular extracted files with that archive, so unrecorded wx edits fail. The inventory includes wx's own notices and its bundled image/XML/regex library notices, including WebP's patent grant. The default KiCad stroke font is compiled source; outline-font discovery remains a platform operation.

`notices.json` protects all 137 retained notice files. `license-policy.json` in the parent directory documents the reviewed component defaults and exceptions. Per-file GPL notices and SPDX additions take precedence over KiCad's general license summary. Mixed-license excerpts are retained, including the LLVM exception, Zint's embedded font notices and the ISC triangulation code. GLAD's CC0 alternative is selected. Independently authored CorvCAD files retain MIT notices. The symbol-library license is separately pinned and retained with its design exception.

## Verify without a Mac or a build

From the repository root, with Python 3.10+ and Git:

```sh
python3 integrations/kicad_core/fetch.py --output build/kicad-core --source-only
python3 integrations/kicad_core/audit.py verify --kicad build/kicad-core/kicad
python3 -m unittest discover -s integrations/kicad_core -p test_audit.py -v
```

Use `py -3` on Windows. Hashes normalize CRLF to LF. The verifier replays the complete patch series in a temporary Git index and checks the source tree against that result. It preserves the caller's real index and files. Undeclared changes, missing source files, altered license texts and an incomplete patch list fail the check. KiCad's generated `<tag>`/`<tag>-dirty` version sentinel files are the only ignored source-tree additions. `fetch.py` accepts a pristine or completely patched integration checkout; it refuses partial patches or other edits instead of overwriting them.

The `Verify KiCad schematic inputs` workflow runs this source verification when started by hand; no workflow runs on push. It also packs the full official symbol set. When CI isn't run, verify locally with `audit.py verify` before committing input changes. The artifact from the source check is an inventory, not corresponding source or an IPA.

## Refresh after native inputs change

Build and compare the proof targets first, using `docs/KICAD_CORE_PORT.md`. On the Mac, capture to a **new** output directory:

```sh
python3 integrations/kicad_core/audit.py capture \
  --kicad ~/kicad-core/kicad --build build/kicad-probe \
  --vcpkg build/ios-deps/vcpkg --installed build/ios-deps/installed \
  --wx build/wx-ios/wxWidgets-3.3.4 --output build/gate1-next
```

Review the manifest and notice changes before replacing this directory's generated JSON and `notices/`. Preserve this README. The capture command reads the existing build tree and does not rebuild or rewrite it. If transferring its output from macOS with `tar`, use `COPYFILE_DISABLE=1` to avoid AppleDouble metadata files.

## Native binary distribution

The combined native binary uses GPL-3.0-or-later distribution terms while each source file retains its own license. Publish the complete source used by the binary, local changes, interface definitions and build scripts alongside it; include the applicable notices and a clear way to find that source. Preserve the MIT permissions on independent CorvCAD code. These requirements follow the [pinned KiCad license summary](https://gitlab.com/kicad/code/kicad/-/blob/10.0.6/LICENSE.README) and [GPLv3 sections 1, 5 and 6](https://www.kicad.org/about/licenses/).

A list of source URLs or package-manager recipe pins alone is not a corresponding-source archive. The native packaging work must preserve the actual KiCad, wx and non-system dependency sources used for that release, their patches and applicable installation information. The ordinary Apple SDK and runtime remain system prerequisites. This audit does not certify App Store distribution or completion of the native IPA.

# FreeCAD F2 desktop input audit

F2 desktop build/input-audit criteria were completed on 2026-10-25 by Codex. The optional, headless FreeCAD 1.1.4 slice passed [fresh Linux/macOS lock verification](https://github.com/Corvusar/corvcad/actions/runs/40189353513), upstream suites and both model proofs. The accepted reports and artifact identities are in [`f2-acceptance.json`](../../integrations/freecad_core/evidence/f2-acceptance.json).
It does not link FreeCAD into CorvCAD's engine/app. F3 must still establish static
iOS linking, extension registration, dependency minimization and simulator results.
F4 adds the mechanical C ABI/app edition; F5 requires a physical iPad result.

## What is locked

`integrations/freecad_core/audit/desktop-inputs.json` retains the original 1,339
compiled source/header inputs. `dependency-lock.json` extends that measurement
with the selected build's Ninja graph, CMake configure inputs, XML/Python schemas,
binding generator package/templates, checked-in parser grammars, copied resources
and loaded Python files. Integration scripts, fixtures, licence policy and manual
workflows are also hashed. Changing a generator, build flag script or validation
script requires a reviewed refresh.

Compiler-discovered external files and recursive runtime-library links determine
dependency ownership. Linux records exact Debian binary/source package versions
and copyright texts, including system/runtime and build-tool packages. The build
selects distro CMake/Ninja instead of unowned tools preinstalled in the runner.
macOS records actual Cellar versions, installed recipes, source archive checksums,
package notices and installation receipts. Missing bottle notices come from the matching checksum-verified upstream source archive. Python distributions outside the Cellar
are matched against their installed file records and retain their own notices.
Apple's SDK/runtime are identified system prerequisites, not bundled dependencies.

These are **version/content locks of the actual inputs**, not a claim that apt or
Homebrew will preserve a version forever. Fresh captures must match versions,
header/library/tool bytes, recipes, notices and generator inputs. Missing versions
or changed inputs fail reproduction and require review; the workflow does not
silently accept newer packages or automatically downgrade a machine. Installation
timestamps in receipts are retained as provenance but excluded from comparisons.
Generated outputs are regenerated and tested per host; their bytes need not match
another platform. Source archives needed for a native release remain a separate
complete-corresponding-source requirement.

## Licence decisions and payload exclusions

The per-file catalogues in `audit/closure/` retain notice excerpts and attribution,
with original source hashes. SPDX declarations and specific file grants take
precedence over component defaults. Historical LGPL 2.0-or-later grants are kept
distinct from LGPL 2.1-or-later. The reviewed policy also records:

- Bison's parser-skeleton exception alongside the expression grammar's LGPL grant.
- PyCXX's complete BSD-style/DOE notice and PPEMBED's permissive permission.
- TensorFlow's Apache 2.0 lazy-loader helper, separately from FreeCAD's build wrapper.
- The VTK BSD Qt-selection helper, BSD Eigen finder, Artistic libkdtree and Boost E57 code.
- The omitted [GitInfo MIT notice](https://github.com/cppmf/GitInfo.cmake/blob/87dd0de1d5930f551d417518a471cbc9d192a7c8/LICENSE), pinned and retained under `supplemental-notices/`.
- Material data's own GPL/LGPL/Creative Commons fields and document metadata's
  Creative Commons attribution/share-alike grants, including authorship.

Do not apply FreeCAD's general LGPL notice to every copied asset. The measured
tree includes 35 material/example/test files explicitly marked **All rights
reserved**. Two OSIFONT example files are also excluded pending font-specific
packaging review. `production_payload_exclusions` identifies these files by
source path and content hash. Only attribution/licence metadata is retained for
restricted assets; their pattern/document contents are not vendored in this audit.

Before archiving any future native app payload, run:

```sh
python integrations/freecad_core/verify_payload.py --directory STAGED_APP --catalog integrations/freecad_core/audit/closure/linux-licenses.json --catalog integrations/freecad_core/audit/closure/macos-licenses.json
```

The guard rejects excluded content even after renaming or text newline conversion.
This is a necessary packaging check, not sufficient release approval. F3 should
omit unused examples, test tools, fonts and factory material libraries from the
production payload. Retaining package-wide notices does not mean every package
component is linked or shipped. No native binary is released by these workflows.

## Verify without a desktop build

Use Python 3.10+ and Git, from the repository root:

```sh
python integrations/freecad_core/slice.py prepare
python integrations/freecad_core/verify_closure.py --source build/freecad-core/FreeCAD
```

Preparation refuses undeclared edits and partial patch states. Verification is
read-only: it checks the source pin, complete patch state, compiled/generator
inputs, integration tooling, classification evidence and every retained notice,
recipe, graph and command snapshot. It preserves the source files and real index. Text checkout line endings can normalize; binary CAD/font inputs are checked exactly.

The manual `source_slice=true` workflow also compares its newly captured host
dependencies with the appropriate platform lock after rebuilding and testing.
It requires all target artifacts, nonempty successful upstream suites and both
F1 model proofs. The 669 upstream tests include three explicit skips: the excluded
Draft mirror case and upstream Sketcher secant/GUI mesh primitive-count cases.

## Refresh deliberately

Build and test both platforms first. A candidate rejected only for expected lock drift can be reviewed if all build, upstream-test and model stages passed. Download both artifacts, then
capture to a fresh directory:

```sh
python integrations/freecad_core/retain_closure.py --linux linux.zip --macos macos.zip --output build/freecad-core/closure-next --run-url RUN_URL --repository-commit FULL_COMMIT
```

Review added/removed/changed inputs, package versions and source/recipe pins,
notices, licence decisions and payload exclusions. Copy the reviewed generated
files into `audit/`, preserving its README and original desktop snapshot. Run
source-only verification, then a fresh Linux/macOS workflow against the retained
locks. A list of URLs or hashes alone does not replace a release's complete
corresponding-source archive or establish an iOS/device result.

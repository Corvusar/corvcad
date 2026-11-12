# F2 measured desktop inputs

This retains the original compiler-input measurement from the successful Linux and macOS source-build
[run 40182424954](https://github.com/Corvusar/corvcad/actions/runs/40182424954).
The complete reviewed dependency/generator/licence audit is in `dependency-lock.json`; fresh-run comparisons passed on both platforms. `desktop-inputs.json` records
1,339 distinct FreeCAD translation-unit/header inputs, generated output hashes,
copied resources and runtime link inspections. The two platforms agree on all
recorded source bytes. The 14 source-tree notice files are retained verbatim;
some describe components outside the selected build and are not a shipping list.
The original raw SPDX scan retains `review-required` markers; the complete per-file classifications and notice evidence are now in `closure/*-licenses.json`.

The raw artifact IDs and SHA-256 hashes identify the build evidence. The concise
test/model reports are retained in `../evidence/f2-*.json`. Each platform built
673 objects and 12 target artifacts, passed 669 upstream tests with three skips,
and passed both F1 model proofs. Linux used OCCT 7.6.3; macOS used OCCT 7.9.3.

The retained snapshot/verifier also passed from fresh checkouts on both platforms
in [run 40183927695](https://github.com/Corvusar/corvcad/actions/runs/40183927695),
at repository commit `a3ea0c7af154bbc356c811cc7555fc92e2aeeca4`. Both builds,
upstream suites and model proofs passed again, and every measured source hash
matched this snapshot. The result is retained in
[`f2-source-verification.json`](../evidence/f2-source-verification.json).

## Verify the source snapshot

With Python 3.10+ and Git, from the repository root:

```sh
python integrations/freecad_core/slice.py prepare
python integrations/freecad_core/verify_inventory.py --source build/freecad-core/FreeCAD
```

The first command fetches or verifies the pinned checkout and applies only the
declared, hashed patches. It refuses undeclared edits and partial patch states.
The second command is read-only: it requires the complete patch state, validates
source/profile/patch hashes and retained notices, and preserves the caller's
index and files. Source verification permits checkout CRLF for LF measurements and preserves
upstream files already recorded with CRLF; retained notices are exact bytes. This check cannot establish that a changed compiler/dependency produces
the same build; those changes require fresh builds and inventory review.

## Capture another measurement

Download both successful CI artifact ZIPs, then use a fresh output directory:

```sh
python integrations/freecad_core/retain_inventory.py --linux linux.zip --macos macos.zip --output build/freecad-core/audit-next --run-url RUN_URL --repository-commit FULL_COMMIT
```

The tool requires matching source/profile pins, complete target outputs and
successful upstream suites with the measured stock baseline's suite counts.
Review the differences before replacing this snapshot; preserve this README.

## Full desktop audit

`dependency-lock.json` extends this historical snapshot with 2,145 Linux and
2,157 macOS source/generator/resource inputs, 7,482 and 8,855 external input
files, and 129/27 package records respectively. The Linux total includes standard
licence texts; Apple's SDK is an identified system prerequisite. The full records
and strict comparisons are described in [the F2 handoff](../../../docs/history/FREECAD_F2_HANDOFF.md).

`dependency-review.json` preserves observed header SPDX declarations, package
licence alternatives and reviewed references to mixed/plain-text copyright
notices. Package-wide notices include unlinked components. The per-file catalogues
classify source and asset grants separately; 35 reserved-rights assets and two
font-related files are excluded from future production payloads. See the handoff
for the content-based packaging guard and complete corresponding-source limits.

Fresh Linux/macOS validation passed in [run 40189353513](https://github.com/Corvusar/corvcad/actions/runs/40189353513), including exact comparison against the retained locks, all target outputs, upstream suites and both F1 models. A passing source-only check alone does not establish host dependency
reproduction or iOS linking. F3-F5 remain separate gates.

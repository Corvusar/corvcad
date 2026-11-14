# Open CASCADE on iPadOS: evidence and CorvCAD path

Reviewed 2026-10-17. This records implementation evidence and a proposed integration path; it does not claim CorvCAD already ships OCCT on iPad. The referenced brainstorming chat raised native FreeCAD, Open CASCADE, a Swift front end, and GPU rendering. Its assistant messages are available here only as content references, so the concrete implementation details below were checked against the linked projects and upstream documentation rather than attributed to unseen chat text.

## What already exists

Open CASCADE Technology (OCCT) is a native C++ geometry kernel with exact boundary-representation (B-rep) solids, modeling algorithms, meshing, and CAD data exchange. Its [upstream documentation](https://github.com/Open-Cascade-SAS/OCCT/wiki) lists iOS arm64 among supported platforms and says core modules can be built without the optional viewer dependencies. This removes the assumption that OCCT must first be invented or newly ported to iOS. It does **not** mean every desktop module or a complete CAD app is ready to drop into CorvCAD.

| Existing implementation | Verified useful detail | Limit for CorvCAD |
| --- | --- | --- |
| [OpenShape3D](https://github.com/laanlabs/openshape3d) | Open-source iPad/iPhone app with SwiftUI, a custom Metal renderer, and OCCT B-rep behind an Objective-C++/Swift bridge. Its [OCCT port design](https://github.com/laanlabs/openshape3d/blob/main/docs/OCCT_BREP_PORT_DESIGN.md) describes the kernel boundary, B-rep-to-render-mesh conversion, persistence, and test spike. Its repository includes an [iOS device/simulator XCFramework build recipe](https://github.com/laanlabs/openshape3d/blob/main/scripts/build_occt_ios.sh). | Its documented iOS build omits DataExchange and STEP/IGES. Several operations still fall back to a mesh kernel, losing the exact B-rep. Reuse the *approach* or code only after checking versions and licenses; it is not a ready-made CorvCAD STEP module. |
| [OCCTSwift](https://github.com/SecondMouseAU/OCCTSwift) | Swift wrapper for OCCT with iOS/macOS XCFramework distribution; its README shows B-rep booleans, fillets, shelling, STEP and STL export. The [build guide](https://github.com/SecondMouseAU/OCCTSwift/blob/main/docs/guides/building-occt.md) gives a device/simulator build and explicitly includes DataExchange/STEP toolkits. | Its native bridging code and Swift APIs are useful references. Evaluate reuse against CorvCAD's portable C++ engine and C ABI, including binary size, ABI, version pin, wrapper coverage, maintenance, and license obligations. |
| CorvCAD today | `engine/CMakeLists.txt` has optional `CORVCAD_WITH_OCCT`. `engine/src/engine.cpp` uses OCCT primitives, cut/fuse, and `STEPControl_Writer` for desktop tray STEP. The iPad app compiles the dependency-free C++ mesh path. | Current desktop STEP produces a simple tray shape only; it does not export the separate lid, drilled bosses, or all geometry represented in the preview. There is no OCCT iOS binary, binding, or on-device STEP test in this repo. |

OpenShape3D is the strongest proof that a native iPad/Metal/OCCT architecture is feasible. OCCTSwift is the more direct example for on-device STEP. Neither proves that CorvCAD's *current* geometry and build settings yield a correct manufacturing-ready enclosure. The [upstream OCCT build guidance](https://github.com/Open-Cascade-SAS/OCCT/blob/master/dox/build/build_occt/building_occt.md) remains the source for module and dependency configuration.

## OCCTSwift as an implementation reference

Owner decision, 2026-10-26: continue the direct OCCT/FreeCAD integration with a portable C++ core and a small C ABI for platform bindings. When developing the OCCT-to-Swift boundary, consult [OCCTSwift upstream](https://github.com/SecondMouseAU/OCCTSwift), including its native bridge, tests and issue discussions, for capabilities and problems it may already have solved.

For each relevant problem, compare reusing or adapting the upstream solution with developing a CorvCAD solution. Choose the approach best supported by evidence on correctness, object ownership and lifetime, error handling, threading, dependency/ABI compatibility, portability, performance, binary size and maintenance. Preserve CorvCAD's architecture and validated dependency pins; validate any proposed change rather than assuming compatibility. Existing upstream work can save effort, while a custom solution is appropriate when it better meets CorvCAD's requirements.

Record the upstream revision examined, the choice and its rationale, and focused validation with the implementation. Check licenses and preserve required notices when reusing code. This policy guides implementation choices; it does not adopt OCCTSwift as a dependency.

## Recommended integration boundary

1. Keep the public CorvCAD model and C API platform-neutral. Add a geometry backend boundary that accepts board/enclosure parameters and returns exact tray and lid solids, triangulated preview meshes, and format exports. Do not make app-level Swift types the only owner of shape data.
2. Pin an OCCT release and a reproducible iOS build recipe, using the OpenShape3D and OCCTSwift scripts as implementation references. Build arm64 device and simulator slices separately; start headless with FoundationClasses, ModelingData, and ModelingAlgorithms. Add DataExchange and its dependencies for STEP, while leaving OCCT's own OpenGL viewer off because CorvCAD renders through Apple's graphics stack.
3. Keep B-rep solids authoritative through shell/cavity, bosses, bores, connector cutouts, and lid construction. Tessellate *those same solids* for preview and STL. A mesh-only fallback must be clearly identified and must not silently turn an exact feature into a faceted one.
4. Expose OCCT through a small C++ implementation behind the existing C ABI. SwiftUI/Metal (or the current SceneKit preview) consumes triangles, edge curves, part IDs, and optional face metadata; OCCT performs geometry, healing, and STEP export. FreeCAD itself is not required for this kernel path.
5. Export tray and lid as separate, named components when the chosen STEP writer supports them. Validate STEP by reimporting it and comparing part count, solid validity, bounding boxes, bore locations, and key dimensions against the source model. Check STL manifoldness separately.

## First implementation gate

On a Mac CI runner, build pinned OCCT for iOS arm64 and simulator arm64; link it into a minimal app or test target. Construct a tray with one real through-bore and a separate lid, tessellate both, write STEP and STL to the app's document area, and reopen those outputs on desktop. Measure binary size and cold operation time. Run the same smoke operation on a physical iPad before showing STEP export in CorvCAD's UI. (**UPDATE 2026-11-14:** done through the FreeCAD edition, whose STEP passed on an iPad Air (M2) on 2026-10-31; see [the FreeCAD port](FREECAD_PORT.md).) Then replace the present desktop-only tray exporter behind a shared exact-model API instead of maintaining two divergent enclosure implementations.

This is a **next step**, not a completed test. This Windows checkout cannot compile an iOS OCCT binary or confirm on-device behavior.

## Licensing and packaging

OCCT is distributed under [LGPL 2.1 with the Open CASCADE exception](https://dev.opencascade.org/sites/default/files/pdf/Preliminaries.pdf). An iOS distribution using static libraries or an XCFramework needs the correct notices, corresponding source/build inputs, and a practical way to satisfy applicable relinking terms. OpenShape3D's MIT license and OCCTSwift's LGPL license apply to their own code separately. Do a license review before copying either project's code or shipping a prebuilt binary; `THIRD_PARTY.md` must name the actual dependency and version used. No OCCT iOS package is vendored in CorvCAD today.

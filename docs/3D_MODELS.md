# 3D component models: generator, viewer and cache (design record)

Decided 2026-10-24. This is a design, not a shipped feature: nothing below is built yet.

## Owner's decision

- KiCad's footprint libraries are bundled like the symbol libraries: footprints only, no 3D models. The official 3D library is several gigabytes of STEP and VRML files.
- 3D comes from CorvCAD's own procedural generator instead, built on the same exact-geometry engine as the mechanical side (OCCT, then the FreeCAD port), not as a duplicate feature set.
- Generated parts are made on demand. They may be kept for reuse, and the user can clear them to free space.

The owner left four choices open. They are recorded below with the reasons.

## Choices

1. **One generator, shared by both sides, in C++ on OCCT.**
   - It does not run inside FreeCAD, and it is not a separate renderer-only mesh generator.
   - FreeCAD's Part module is OCCT, so a generated `TopoDS_Shape` drops into a FreeCAD document as a `Part::Feature`. That serves enclosure clearances and the assembly. The same shape also feeds the board's 3D view on the KiCad side.
   - The KiCad side never needs a FreeCAD session open. Both depend on the OCCT layer, which comes from the OCCT iOS gate and is then shared with FreeCAD (F2/F3 in [FREECAD_PORT.md](FREECAD_PORT.md)).
   - Running generators through FreeCAD's embedded Python would make every board view wait on the mechanical engine. Hundreds of parts per board would be slow on an iPad.
2. **KiCad's method, not a new one.**
   - KiCad's official 3D library is itself generated procedurally, with the `kicad-packages3D-generator` scripts. These are CadQuery on OCCT, driven by per-family parameter tables, and they write STEP and VRML.
   - CorvCAD ports the construction rules and parameter tables to C++ family by family: chip resistors and capacitors, SOIC/QFP/QFN, pin headers, and so on. It does not invent new geometry.
   - KiCad's model file names encode the family and parameters (`R_0603_1608Metric`, say). So a footprint's `(model ...)` path maps back to a generator and its parameters.
   - A model the generator cannot match becomes a box with the footprint courtyard's outline. It is marked as a placeholder in the viewer and is never exported as if it were exact.
   - The generator repository's licence and the per-family parameter sources must be reviewed before any code or table is copied.
3. **No VRML.**
   - KiCad uses `.wrl` files only as coloured render meshes for its own OpenGL viewer and ray tracer.
   - On iOS, the engine tessellates the exact shape (OCCT `BRepMesh`) and returns triangles, normals and per-face colours through the C ABI, as the enclosure preview path already plans.
   - Swift renders them with SceneKit for now, the same path as the enclosure, and Metal or RealityKit later. SceneKit is soft-deprecated; RealityKit's SwiftUI view needs iOS 18, and the app targets 17.
   - STEP export always comes from the exact shapes, never from meshes (AGENTS.md).
4. **Parameters are the stored truth; meshes are a cache.**
   - A part is identified by generator version, family and a hash of its parameters.
   - By default it is generated into memory when first shown.
   - Tessellated meshes may be kept in `Library/Caches/GeneratedParts`, which iOS may purge under storage pressure. A settings row shows the cache size and offers "Clear generated parts".
   - Nothing generated goes into the project folder. A STEP file is produced only on export.

## Gates

1. **OCCT on iOS.** The existing card: exact tray and lid with STEP on a device.
2. **Generator, desktop.** Three families: chip R/C, SOIC and pin headers. Check that bounding boxes and volumes match KiCad's official STEP models for the same names within tolerance, and that the generator is deterministic.
   - **Passed 2026-11-11.** `integrations/parts3d/` ports four upstream families (chip C, chip R, 2.54 mm THT pin headers, and the gull-wing generator, which covers SOIC, SSOP, TSSOP, QFP and SOT) from kicad-packages3D-generator e272fcc to C++ on OCCT. The tables have 1,058 names. `parts3d_check all` on Codemagic (mac mini M2, Homebrew OCCT 7.9.3, build 6af4a2a8493b03eb0379571d) builds all 1,058 with no failures, invalid solids or non-deterministic output, in 680–850 s; the slowest is PQFP-256 at 12–24 s. `parts3d_check compare` against 26 KiCad 10.0.0 STEP models gives the same result on Codemagic and on the iMac (OCCT 7.9.3 from source): every bounding box matches to 0.0000 mm. Volumes match exactly for chip R/C and pin headers, and within +0.08 to +0.37% for gull-wing (tolerance 1%).
   - Known difference: KiCad 10's `LQFP-48_7x7mm_P0.5mm` was not made from the current table (its lead shoulder is at z 0.625–0.725 instead of 0.75–0.85; the bounding box is identical; volume −1.35%). The check reports it rather than failing on it.
   - Not ported yet: surface-mount pin headers (Vertical_SMD) and every other family. Their footprints get placeholders in gate 3.
3. **Board 3D view in the app.** Footprints placed with their generated parts over the board outline and thickness. Placeholders are marked. Memory and generation time are measured on a large demo board on the iPad.
   - **Simulator part passed 2026-11-11.** The engine reads each footprint's first `(model ...)` with its offset, rotate and scale, its side, and its pads' box (`cc_placement`). `parts3d_c_api` (FreeCAD edition only) generates a part once per model path, meshes it with BRepMesh (0.01 mm chord) and keeps it in memory. `BoardParts.swift` builds parts in the background and places them with the transforms of KiCad 10's OpenGL viewer (`render_3d_opengl.cpp`: footprint translate and rotate; a flipped footprint turned 180 degrees about Y and then Z, which is 180 degrees about X; model offset, rotate −z, −y, −x, scale). Footprints with a model the generator does not cover get a translucent 1 mm box over their pads; footprints without a model show nothing, as in KiCad. A **Parts** pill toggles them and a status panel counts generated parts and placeholders. `parts3d_check compare` also checks each part's mesh: same box as the solids and outward winding. `NativeFreeCADTests.testBoardParts` (iMac, iPad Pro 11 simulator, FreeCAD edition) passes in portrait and landscape with R_0603, SOIC-8 and PinHeader_1x04 on RoundTrip; the default edition builds.
   - Still open: time and memory on a large board on a physical iPad (with the device validation card). The UI test has no back-side part.
4. **Cache and settings.** Cache size, clearing, and regeneration after clearing.
   - **Passed 2026-11-11 (simulator).** Meshes are stored in `Library/Caches/GeneratedParts/<key>.mesh`, where the key is `cc_parts3d_key`: `v<kGeneratorVersion>-<family>-<FNV-1a of the table parameters>`. Version 2 is the pin-header fix. A cached file is read instead of generating again. The app has no settings screen, so the Parts panel in the 3D view is the menu: it shows the cache size and offers **Clear generated parts**, which deletes the folder and the parts in memory (`cc_parts3d_clear`), and the board's parts generate again. `testBoardParts` clears and waits for the regeneration; R_0603, SOIC-8 and PinHeader_1x04 used 136 KB. The FreeCAD edition's Licences screen now states GPL-3.0-or-later for the app as a whole and shows the generator's licence; the default edition bundles neither.
5. **FreeCAD consumes the shapes.** Enclosure clearance checks against real component heights (FreeCAD gate F4 onward).
   - **Passed 2026-11-11 (simulator).** `BoardParts` gives each generated part's reach above the board's top face and below its bottom face. That reach is taken along the footprint's normal after the model's offset, rotation and scale; through-hole leads and back-side parts count below. The FreeCAD edition's inspector has **Component clearance**: the tallest part against the top clearance and the deepest reach against the standoff height, each needing 0.5 mm of room. A short one offers a fix that sets the FreeCAD document's parameter to reach + 0.5 mm, rounded up to 0.1 mm, as one undo step. Footprints whose model has no generator are listed as not checked. Connector cutouts are now as tall as the connector's generated body + 0.5 mm, or 5 mm without one; existing cutouts keep their saved size. `testBoardParts` checks the fix: PinHeader_1x04 at 8.54 mm raises the 2 mm top clearance to 9.1 mm in the document. The generated-height cutout path has no UI test (the fixture has no connector at an edge).
   - Not done: a typed height for footprints without a generated model. They are reported as not checked instead.

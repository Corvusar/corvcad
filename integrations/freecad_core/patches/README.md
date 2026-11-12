# FreeCAD 1.1.4 F2 adaptations

These patches adapt FreeCAD-authored CMake and Python files. The affected Python grants permit **LGPL-2.0-or-later**; bannerless build/test files use the pinned upstream application LGPL2+ default. Per-file terms remain visible in the F2 catalogue.
Original copyright and per-file notices remain in the pinned source; apply
these diffs without removing them. The independently authored surrounding
CorvCAD Python tools remain MIT. FreeCAD's complete LGPL text is at the pinned
source root `LICENSE`; an inventory does not replace corresponding source.

`0001-headless-spreadsheet-and-translations.patch`:

- Limit Spreadsheet's Draft dependency check to GUI builds. Its App target
  links only FreeCADApp and its Init.py does not import Draft.
- Find Qt LinguistTools for headless builds too: FreeCADApp's own CMake file
  compiles translation resources unconditionally. This adds a build tool,
  without intentionally linking a Qt Widgets frontend.

The exact patch and before/after file hashes are in `slice.json`. These are
F2 investigation inputs. Source configure/build/test evidence determines
whether further changes are needed; no iOS support is established here.

`0002-headless-app-test-imports.patch` preserves the upstream model assertions:

- A Part mirror regression that requires the excluded Draft workbench is
  reported as an explicit skip when Draft is absent. Other import errors still
  fail. No substitute clone implementation is introduced.
- Remove an unused Pivy/Coin import from PartDesign's Boolean tests. The GUI-only
  selection test already returns early when `GuiUp` is false upstream.
- Remove the unused FreeCADGui import/alias from Sketcher's regular-polygon
  geometry helper. Its geometry and constraint code is unchanged.
- Copy the existing involute-gear App helper in headless builds too. Its GUI
  imports stay under `GuiUp`; headless property descriptions use the same no-op
  source-text semantics as `QT_TRANSLATE_NOOP`. Gear model tests stay enabled.

These Python and CMake adaptations retain their upstream notices and project
LGPL terms. The mirror test has no per-file licence banner in the pinned source;
its recorded default is LGPL-2.0-or-later, based on `src/Doc/LICENSE.html`. The
root LGPL 2.1 text is also retained; it does not erase older per-file grants.

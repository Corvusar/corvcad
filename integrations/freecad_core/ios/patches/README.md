# Static FreeCAD investigation patch

`0001-static-module-libraries.patch` applies on top of the separately verified F2
patches to FreeCAD 1.1.4 commit `4fd3bf320d9566a27e60069fc8387448aaa3a094`.
`static-profile.json` locks its bytes, before/after file hashes, the F2 base
profile and CorvCAD's registry source. Preparation refuses partial, untracked or
undeclared source edits and never resets a checkout.

The patch adds an opt-in Apple/headless configuration, turns Base/App and the
seven selected extensions into real static archives, skips the external-Python
`FreeCAD.so` wrapper, and links/retains module registration and Qt resource
objects in `FreeCADCmd`. The embedding thread registers extensions before
FreeCAD initializes CPython. `_PartDesign` is the native module name;
`PartDesign` remains its upstream Python helper package. The optional Measure
module is explicitly disabled in this profile.

This is a candidate for native testing, not an accepted static or iOS build.
The unchanged F2 source profile remains the accepted desktop baseline. Native
dependency minimization, cross-Python discovery and app bundle initialization
are separate adaptations; this patch does not imply they work.

Upstream CMake files and `MainCmd.cpp` retain their complete notices in the
source checkout. File SPDX declarations retain LGPL-2.1-or-later where present;
bannerless CMake files use the project LGPL-2.0-or-later grant documented in the
F2 licence catalogue and pinned `src/Doc/LICENSE.html`. CorvCAD's new
`static_registry.cpp` and preparation tools are MIT. The patch does not replace
the source tree's notices or the complete corresponding-source requirements
for a future native distribution.

`0002-ios-runtime.patch` is a separate iOS-only adaptation, locked by
`ios-profile.json`. It discovers the matching host interpreter separately from
target Python headers/archive, retains isolated interpreter settings and the
explicit bundled stdlib path, denies process creation, and supplies final
static/SDK dependencies. Its Main/CMakeLists change explicitly continues the
static patch's declared after hash; all other changes have separate full-file
before/after hashes. Upstream `FCConfig.h` and `Base/Interpreter.cpp` retain
their original LGPL notices. `prepare_ios.py` and the new harness tools are MIT.

// SPDX-License-Identifier: GPL-3.0-or-later
// CorvCAD's 3D part generator: KiCad's own procedural package models (kicad-packages3D-generator,
// GPL-3.0-or-later) ported from CadQuery to C++ on OCCT, so a footprint's (model ...) name becomes
// exact solids without KiCad's 3D library. Native editions only; see docs/3D_MODELS.md.
#pragma once

#include <TopoDS_Shape.hxx>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace corvcad::parts3d {

// Bumped whenever construction changes, so cached meshes of older output are not reused.
// 2: pin-header bases keep their last corner.
constexpr int kGeneratorVersion = 2;

struct Solid {
  TopoDS_Shape shape;            // millimetres, KiCad's model frame (origin at the footprint anchor)
  std::array<float, 3> color;    // the generator's diffuse colour, 0..1
};

struct Part {
  std::string family;            // c_chip, r_chip, pin_header, gull_wing
  std::vector<Solid> solids;     // body, pins, then a mark or resistor top
};

// "${KICAD10_3DMODEL_DIR}/Package_SO.3dshapes/SOIC-8_3.9x4.9mm_P1.27mm.wrl" -> "SOIC-8_3.9x4.9mm_P1.27mm".
std::string model_name(std::string_view path);

// The generated part for a KiCad model name, or nullopt when no family covers it (the caller shows a
// placeholder) or construction failed (error says why).
std::optional<Part> generate(std::string_view name, std::string *error = nullptr);

// "v<generator version>-<family>-<hash of the table parameters>" for a covered name, else "": what a
// cached mesh is stored under, so a table or construction change never reuses an old mesh.
std::string cache_key(std::string_view name);

// Every model name the tables cover, in table order.
std::vector<std::string> names();

} // namespace corvcad::parts3d

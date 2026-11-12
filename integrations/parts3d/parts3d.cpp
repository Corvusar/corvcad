// SPDX-License-Identifier: GPL-3.0-or-later
// Construction rules ported from kicad-packages3D-generator (GPL-3.0-or-later; commit in
// upstream.json): C_Chip_SMD/c_chip_smd.py and Resistor_SMD/main_generator.py (chip capacitors and
// resistors), Connector_PinHeader/pinheader.py (2.54 mm THT headers) and
// GW_QFP_SOIC_SSOP_TSSOP_SOT/gw_qfp_soic_ssop_tssop_sot.py (gull-wing packages). Original authors:
// Maurice (easyw), hyOzd, Frank/Shack, jmwright (2021 update sponsored by KiCad Services Corp.)
// and the KiCad library team. Each CadQuery step is mirrored with the same OCCT operation so the
// solids match KiCad's published STEP models.
#include "parts3d.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace corvcad::parts3d {
namespace {

struct Row { const char *name, *family, *params; };
const Row kRows[] = {
#include "generated/kicad_tables.inc"
};

const double kPi = std::acos(-1.0);

// One table row's "key=value;key=value" parameters.
struct Params {
  std::map<std::string, std::string> values;
  explicit Params(std::string_view text) {
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(';', start);
      if (end == std::string_view::npos) end = text.size();
      const std::string_view pair = text.substr(start, end - start);
      const size_t eq = pair.find('=');
      values[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
      start = end + 1;
    }
  }
  bool has(const std::string &key) const { return values.count(key) > 0; }
  const std::string &text(const std::string &key) const {
    const auto found = values.find(key);
    if (found == values.end()) throw std::runtime_error("missing parameter " + key);
    return found->second;
  }
  double number(const std::string &key) const { return std::stod(text(key)); }
  double number(const std::string &key, double fallback) const { return has(key) ? number(key) : fallback; }
  std::vector<double> list(const std::string &key) const {
    std::vector<double> out;
    if (!has(key)) return out;
    const std::string &all = text(key);
    for (size_t start = 0; start < all.size();) {
      size_t end = all.find(',', start);
      if (end == std::string::npos) end = all.size();
      if (end > start) out.push_back(std::stod(all.substr(start, end - start)));
      start = end + 1;
    }
    return out;
  }
};

std::array<float, 3> color(const std::string &key) {
  // _tools/shaderColors.py diffuse colours.
  static const std::map<std::string, std::array<float, 3>> colors{
      {"metal grey pins", {0.824f, 0.820f, 0.781f}},  {"gold pins", {0.859f, 0.738f, 0.496f}},
      {"black body", {0.148f, 0.145f, 0.145f}},       {"resistor black body", {0.082f, 0.086f, 0.094f}},
      {"brown body", {0.379f, 0.270f, 0.215f}},       {"white body", {0.895f, 0.891f, 0.813f}},
      {"light brown label", {0.691f, 0.664f, 0.598f}}};
  const auto found = colors.find(key);
  if (found == colors.end()) throw std::runtime_error("unknown colour " + key);
  return found->second;
}

// --- CadQuery operations in OCCT -------------------------------------------------------------

TopoDS_Shape transformed(const TopoDS_Shape &shape, const gp_Trsf &t) {
  return BRepBuilderAPI_Transform(shape, t, true).Shape();
}
TopoDS_Shape moved(const TopoDS_Shape &shape, double x, double y, double z) {
  gp_Trsf t;
  t.SetTranslation(gp_Vec(x, y, z));
  return transformed(shape, t);
}
// cq rotate((0,0,0), (0,0,1), degrees)
TopoDS_Shape turned(const TopoDS_Shape &shape, double degrees) {
  if (degrees == 0) return shape;
  gp_Trsf t;
  t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), degrees * kPi / 180);
  return transformed(shape, t);
}
// cq box(dx, dy, dz) centred on (x, y) in plan, from z0 up.
TopoDS_Shape box(double dx, double dy, double dz, double x, double y, double z0) {
  return BRepPrimAPI_MakeBox(gp_Pnt(x - dx / 2, y - dy / 2, z0), dx, dy, dz).Shape();
}
TopoDS_Shape cut(const TopoDS_Shape &a, const TopoDS_Shape &b) {
  BRepAlgoAPI_Cut op(a, b);
  if (!op.IsDone()) throw std::runtime_error("cut failed");
  return op.Shape();
}
// ponytail: union_all as a compound; every family's pins are disjoint, so nothing needs fusing.
TopoDS_Shape compound(const std::vector<TopoDS_Shape> &shapes) {
  BRep_Builder builder;
  TopoDS_Compound out;
  builder.MakeCompound(out);
  for (const TopoDS_Shape &shape : shapes) builder.Add(out, shape);
  return out;
}

gp_Pnt middle(const TopoDS_Edge &edge) {
  BRepAdaptor_Curve curve(edge);
  return curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
}
gp_Pnt center(const TopoDS_Face &face) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  return props.CentreOfMass();
}
std::vector<TopoDS_Edge> edges(const TopoDS_Shape &shape, const std::function<bool(const TopoDS_Edge &)> &keep) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(shape, TopAbs_EDGE, map);
  std::vector<TopoDS_Edge> out;
  for (int i = 1; i <= map.Extent(); ++i)
    if (keep(TopoDS::Edge(map(i)))) out.push_back(TopoDS::Edge(map(i)));
  return out;
}
// Straight edges along (parallel) or across (perpendicular) an axis: cq "|X" and "#Z".
std::vector<TopoDS_Edge> line_edges(const TopoDS_Shape &shape, const gp_Dir &axis, bool parallel) {
  return edges(shape, [&](const TopoDS_Edge &edge) {
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Line) return false;
    const double along = std::abs(curve.Line().Direction().Dot(axis));
    return parallel ? along > 1 - 1e-9 : along < 1e-9;
  });
}
// cq faces(">Z") / faces("<X"): the face whose centre is furthest along the axis.
std::vector<TopoDS_Edge> extreme_face_edges(const TopoDS_Shape &shape, const gp_Dir &axis) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  int best = 0;
  double best_value = -1e300;
  for (int i = 1; i <= faces.Extent(); ++i) {
    const double value = gp_Vec(center(TopoDS::Face(faces(i))).XYZ()).Dot(gp_Vec(axis));
    if (value > best_value + 1e-7) { best_value = value; best = i; }
  }
  TopTools_IndexedMapOfShape face_edges;
  TopExp::MapShapes(faces(best), TopAbs_EDGE, face_edges);
  std::vector<TopoDS_Edge> out;
  for (int i = 1; i <= face_edges.Extent(); ++i) out.push_back(TopoDS::Edge(face_edges(i)));
  return out;
}
TopoDS_Shape fillet(const TopoDS_Shape &shape, const std::vector<TopoDS_Edge> &chosen, double radius) {
  if (radius <= 0 || chosen.empty()) return shape;
  BRepFilletAPI_MakeFillet op(shape);
  for (const TopoDS_Edge &edge : chosen) op.Add(radius, edge);
  op.Build();
  if (!op.IsDone()) throw std::runtime_error("fillet failed");
  return op.Shape();
}
TopoDS_Shape chamfer(const TopoDS_Shape &shape, const std::vector<TopoDS_Edge> &chosen, double distance) {
  if (distance <= 0 || chosen.empty()) return shape;
  BRepFilletAPI_MakeChamfer op(shape);
  for (const TopoDS_Edge &edge : chosen) op.Add(distance, edge);
  op.Build();
  if (!op.IsDone()) throw std::runtime_error("chamfer failed");
  return op.Shape();
}

// A closed profile drawn in a workplane's (u, v) coordinates: cq moveTo/line/threePointArc/close.
class Profile {
 public:
  explicit Profile(std::function<gp_Pnt(double, double)> at) : at_(std::move(at)) {}
  Profile &move_to(double u, double v) { first_ = cur_ = {u, v}; return *this; }
  Profile &line_to(double u, double v) {
    if (std::hypot(u - cur_[0], v - cur_[1]) > 1e-9)
      wire_.Add(BRepBuilderAPI_MakeEdge(at_(cur_[0], cur_[1]), at_(u, v)).Edge());
    cur_ = {u, v};
    return *this;
  }
  Profile &line(double du, double dv) { return line_to(cur_[0] + du, cur_[1] + dv); }
  Profile &arc_to(double mu, double mv, double u, double v) {
    GC_MakeArcOfCircle arc(at_(cur_[0], cur_[1]), at_(mu, mv), at_(u, v));
    wire_.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
    cur_ = {u, v};
    return *this;
  }
  TopoDS_Shape extrude(const gp_Vec &by) {
    line_to(first_[0], first_[1]);
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire_.Wire()).Face(), by).Shape();
  }

 private:
  std::function<gp_Pnt(double, double)> at_;
  BRepBuilderAPI_MakeWire wire_;
  std::array<double, 2> first_{}, cur_{};
};

TopoDS_Wire polygon(const std::vector<gp_Pnt> &points) {
  BRepBuilderAPI_MakePolygon poly;
  for (size_t i = 0; i < points.size(); ++i)
    if (i == 0 || !points[i].IsEqual(points[i - 1], 1e-9)) poly.Add(points[i]);
  poly.Close();
  return poly.Wire();
}

// --- Chip capacitors and resistors ------------------------------------------------------------

Part chip(const Params &p, bool resistor) {
  const double length = p.number("length"), width = p.number("width"), height = p.number("height");
  const double band = p.number("pin_band");
  const double thickness = p.text("pin_thickness") == "auto" ? (resistor ? height : band) / 10.0 : p.number("pin_thickness");
  const double edge = p.text("edge_fillet") == "auto" ? thickness : p.number("edge_fillet");
  const double rotation = p.number("rotation", 0);
  Part part;
  if (!resistor) {
    TopoDS_Shape body = box(length - 2 * band, width - 2 * thickness, height - 2 * thickness, 0, 0, thickness);
    body = fillet(body, line_edges(body, gp::DX(), true), edge);
    std::vector<TopoDS_Shape> pins;
    for (double side : {-1.0, 1.0}) {
      TopoDS_Shape pin = box(band, width, height, 0, 0, -height / 2);
      pin = fillet(pin, line_edges(pin, gp::DX(), true), edge);
      pins.push_back(moved(pin, side * (length / 2 - band / 2), 0, height / 2));
    }
    const TopoDS_Shape all = compound(pins);
    part.solids = {{turned(cut(body, all), rotation), color(p.text("body_color_key"))},
                   {turned(all, rotation), color(p.text("pins_color_key"))}};
  } else {
    const TopoDS_Shape body = box(length - 2 * thickness, width, height - 2 * thickness, 0, 0, thickness);
    const TopoDS_Shape top = box(length - 2 * band, width, thickness, 0, 0, height - thickness);
    std::vector<TopoDS_Shape> pins;
    for (double side : {-1.0, 1.0}) {
      TopoDS_Shape pin = box(band, width, height, 0, 0, -height / 2);
      pin = fillet(pin, line_edges(pin, gp::DY(), true), edge);
      pins.push_back(moved(pin, side * (length / 2 - band / 2), 0, height / 2));
    }
    part.solids = {{turned(body, rotation), color("white body")},
                   {turned(cut(compound(pins), body), rotation), color("metal grey pins")},
                   {turned(top, rotation), color("resistor black body")}};
  }
  return part;
}

// --- 2.54 mm pin headers ----------------------------------------------------------------------

// The base outline in a workplane: notches of `chamfer` between pins on both long sides.
std::vector<std::array<double, 2>> header_outline(int n, double pitch, double width, double u0, double chamfer) {
  std::vector<std::array<double, 2>> out;
  double u = u0 + chamfer, v = pitch / 2;
  auto step = [&](double du, double dv) { u += du; v += dv; out.push_back({u, v}); };
  out.push_back({u, v});
  if (chamfer == 0) {
    step(0, -n * pitch); step(width, 0); step(0, n * pitch);
  } else {
    for (int i = 0; i < n; ++i) { step(-chamfer, -chamfer); step(0, -pitch + 2 * chamfer); step(chamfer, -chamfer); }
    step(width - 2 * chamfer, 0);
    for (int i = 0; i < n; ++i) { step(chamfer, chamfer); step(0, pitch - 2 * chamfer); step(-chamfer, chamfer); }
  }
  return out; // the polygon closes back to the start
}

Part pin_header(const Params &p) {
  const int n = static_cast<int>(p.number("n")), rows = static_cast<int>(p.number("rows"));
  const double pitch = p.number("pitch"), width = p.number("base_width"), height = p.number("base_height");
  const double pin_width = p.number("pin_width"), above = p.number("pin_length_above_base");
  const double below = p.number("pin_length_below_board");
  const double base_chamfer = p.text("base_chamfer") == "auto" ? pitch / 10.0 : p.number("base_chamfer");
  const double end_chamfer = p.text("pin_end_chamfer") == "auto" ? pin_width / 4.0 : p.number("pin_end_chamfer");
  const bool vertical = p.text("type") == "Vertical_THT";
  TopoDS_Shape base;
  std::vector<TopoDS_Shape> pins;
  if (vertical) {
    std::vector<gp_Pnt> points;
    for (const auto &[u, v] : header_outline(n, pitch, width, -(width - (rows - 1) * pitch) / 2.0, base_chamfer))
      points.emplace_back(u, v, 0);
    base = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon(points)).Face(), gp_Vec(0, 0, height)).Shape();
    TopoDS_Shape pin = box(pin_width, pin_width, below + height + above, 0, 0, -below);
    pin = chamfer(pin, line_edges(pin, gp::DZ(), false), end_chamfer);
    for (int x = 0; x < rows; ++x)
      for (int y = 0; y < n; ++y) pins.push_back(moved(pin, x * pitch, -y * pitch, 0));
  } else {
    const double x_offset = p.number("base_x_offset"), x0 = x_offset + (rows - 1) * pitch;
    // cq Workplane("ZY") offset -x0: u is Z, v is Y, the plane at x = x0; extrude(-height) runs to +X.
    std::vector<gp_Pnt> points;
    for (const auto &[u, v] : header_outline(n, pitch, width, 0, base_chamfer)) points.emplace_back(x0, v, u);
    base = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon(points)).Face(), gp_Vec(height, 0, 0)).Shape();
    for (int row = 0; row < rows; ++row) {
      const double offset = row * pitch;
      // cq Workplane("XZ") offset -pin_width/2: u is X, v is Z, at y = pin_width/2; extrude to -Y.
      Profile profile([&](double u, double v) { return gp_Pnt(u, pin_width / 2, v); });
      TopoDS_Shape pin = profile.move_to(x0 + height + above, (width - (rows - 1) * pitch) / 2 + pin_width / 2 + row * pitch)
                             .line(0, -pin_width)
                             .line(-above - height - x_offset - offset + pin_width / 2, 0)
                             .line(0, -(width / rows - pin_width) / 2 - below - offset)
                             .line(-pin_width, 0)
                             .line(0, (width / rows - pin_width) / 2 + below + offset + pin_width)
                             .extrude(gp_Vec(0, -pin_width, 0));
      Bnd_Box bounds;
      BRepBndLib::Add(pin, bounds);
      double xmin, ymin, zmin, xmax, ymax, zmax;
      bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
      const double gap = bounds.GetGap();
      // edges("<X and >Z"): the outer corner of the bend.
      pin = fillet(pin, edges(pin, [&](const TopoDS_Edge &edge) {
                     const gp_Pnt m = middle(edge);
                     return std::abs(m.X() - (xmin + gap)) < 1e-4 && std::abs(m.Z() - (zmax - gap)) < 1e-4;
                   }), pin_width);
      pin = chamfer(pin, extreme_face_edges(pin, -gp::DZ()), end_chamfer);
      pin = chamfer(pin, extreme_face_edges(pin, gp::DX()), end_chamfer);
      for (int y = 0; y < n; ++y) pins.push_back(moved(pin, 0, -y * pitch, 0));
    }
  }
  return {"", {{base, color(p.text("body_color_key"))}, {compound(pins), color(p.text("pin_color_key"))}}};
}

// --- Gull-wing packages (QFP, SOIC, SSOP, TSSOP, SOT) -----------------------------------------

Part gull_wing(const Params &p) {
  const double c = p.number("c"), the = p.number("the") * kPi / 180, R2 = p.number("R2");
  double tb_s = p.number("tb_s");
  const double fp_r_in = p.number("fp_r"), fp_d = p.number("fp_d"), fp_z = p.number("fp_z"), ef = p.number("ef");
  double cc1 = p.number("cc1");
  const double D1 = p.number("D1"), E1 = p.number("E1"), E = p.number("E"), A1 = p.number("A1"), A2 = p.number("A2");
  const double b = p.number("b"), e = p.number("e");
  const int npx = static_cast<int>(p.number("npx")), npy = static_cast<int>(p.number("npy"));
  const bool round_mark = p.number("fp_s") != 0;
  std::optional<double> S, L, R1;
  if (p.has("S")) S = p.number("S");
  if (p.has("L")) L = p.number("L");
  if (p.has("R1")) R1 = p.number("R1");
  if (S && L && R1) S.reset();
  if (!L) L = (E - E1) / 2 - (*S + *R1);
  if (!S) S = (E - E1) / 2 - (*R1 + *L);
  if (!R1) R1 = (E - E1) / 2 - (*S + *L);
  if (*L < 0 || *S < 0 || *R1 < 0 || *L < c + R2) throw std::runtime_error("inconsistent pin parameters");
  if (tb_s == 0) tb_s = 0.001;

  const double A = A1 + A2, A2_t = (A2 - c) / 2, A2_b = A2_t;
  const double D1_b = D1 - 2 * std::tan(the) * A2_b, E1_b = E1 - 2 * std::tan(the) * A2_b;
  const double D1_t1 = D1 - tb_s, E1_t1 = E1 - tb_s;
  const double D1_t2 = D1_t1 - 2 * std::tan(the) * A2_t, E1_t2 = E1_t1 - 2 * std::tan(the) * A2_t;

  const double pins_x = (npx - 1) * e + b, pins_y = (npy - 1) * e + b;
  if (cc1 != 0) cc1 = std::min(std::abs(std::min({(D1 - pins_x) / 2.0, (E1 - pins_y) / 2.0, cc1}) - 0.5 * tb_s), 1.0);

  // The body: a ruled loft through the bottom, the two waist outlines and the top.
  auto outline = [&](double w, double h, double z, double chamfer) {
    if (cc1 == 0) return polygon({{-w / 2, -h / 2, z}, {w / 2, -h / 2, z}, {w / 2, h / 2, z}, {-w / 2, h / 2, z}});
    return polygon({{-w / 2, -h / 2 + chamfer, z}, {-w / 2, h / 2 - chamfer, z}, {-w / 2 + chamfer, h / 2, z},
                    {w / 2 - chamfer, h / 2, z}, {w / 2, h / 2 - chamfer, z}, {w / 2, -h / 2 + chamfer, z},
                    {w / 2 - chamfer, -h / 2, z}, {-w / 2 + chamfer, -h / 2, z}});
  };
  BRepOffsetAPI_ThruSections loft(true, true);
  loft.AddWire(outline(D1_b, E1_b, A1, cc1 - (D1 - D1_b) / 4.0));
  loft.AddWire(outline(D1, E1, A1 + A2_b, cc1));
  loft.AddWire(outline(D1, E1, A1 + A2_b + c, cc1));
  loft.AddWire(outline(D1_t1, E1_t1, A1 + A2_b + c, cc1 - (D1 - D1_t1) / 4.0));
  loft.AddWire(outline(D1_t2, E1_t2, A, cc1 - (D1 - D1_t2) / 4.0));
  loft.Build();
  if (!loft.IsDone()) throw std::runtime_error("body loft failed");
  TopoDS_Shape body = loft.Shape();
  if (ef != 0) {
    try { body = fillet(body, extreme_face_edges(body, gp::DZ()), ef); } catch (const Standard_Failure &) {
    } catch (const std::runtime_error &) {} // as upstream: a failed top fillet leaves sharp edges
    if (cc1 == 0) {
      for (double sx : {1.0, -1.0})
        for (double sy : {1.0, -1.0}) {
          // cq BoxSelector((sx*D1_t2/2, sy*E1_t2/2, 0), (sx*(D1/2+0.1), sy*(E1/2+0.1), A2)) on edge centres
          const double x0 = std::min(sx * D1_t2 / 2, sx * (D1 / 2 + 0.1)), x1 = std::max(sx * D1_t2 / 2, sx * (D1 / 2 + 0.1));
          const double y0 = std::min(sy * E1_t2 / 2, sy * (E1 / 2 + 0.1)), y1 = std::max(sy * E1_t2 / 2, sy * (E1 / 2 + 0.1));
          try {
            body = fillet(body, edges(body, [&](const TopoDS_Edge &edge) {
                            const gp_Pnt m = middle(edge);
                            return m.X() >= x0 && m.X() <= x1 && m.Y() >= y0 && m.Y() <= y1 && m.Z() >= 0 && m.Z() <= A2;
                          }), ef);
          } catch (const Standard_Failure &) {
          } catch (const std::runtime_error &) {}
        }
    }
  }

  // The pin-1 mark, cut into the body and kept as its own coloured solid.
  const bool place_mark = fp_r_in != 0;
  const double fp_r = place_mark ? fp_r_in : 0.1;
  TopoDS_Shape mark;
  if (!round_mark) {
    mark = box(fp_r, E1_t2 - fp_d, fp_z * 2, -D1_t2 / 2 + fp_r / 2.0 + fp_d / 2, 0, A - fp_z);
  } else {
    // cq Workplane("XZ", centre).rect(fp_r/2, -fp_z, False).revolve(): a disc fp_z deep under the top.
    mark = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-D1_t2 / 2 + fp_d + fp_r, -E1_t2 / 2 + fp_d + fp_r, A - fp_z), gp::DZ()),
                                    fp_r / 2, fp_z).Shape();
  }
  if (place_mark) body = cut(body, mark);

  // One pin at the middle of the +Y side, drawn in cq Workplane("YZ", (0, E1/2, 0)): u is Y, v is Z.
  const double R1_o = *R1 + c, R2_o = R2 + c, r2 = std::sqrt(2.0), h = A1 + A2_b;
  Profile profile([&](double u, double v) { return gp_Pnt(0, E1 / 2 + u, v); });
  const double s = *S, r1 = *R1, l = *L;
  const TopoDS_Shape pin = moved(profile.move_to(-tb_s, h)
                                     .line(s + tb_s, 0)
                                     .arc_to(s + r1 / r2, h - r1 * (1 - 1 / r2), s + r1, h - r1)
                                     .line(0, -(h - r1 - R2_o))
                                     .arc_to(s + r1 + R2_o * (1 - 1 / r2), R2_o * (1 - 1 / r2), s + r1 + R2_o, 0)
                                     .line(l - R2_o, 0)
                                     .line(0, c)
                                     .line(-(l - R2_o), 0)
                                     .arc_to(s + r1 + R2_o - R2 / r2, c + R2 * (1 - 1 / r2), s + r1 + R2_o - r1, c + R2)
                                     .line_to(s + r1 + c, h - r1)
                                     .arc_to(s + R1_o / r2, h + c - R1_o * (1 - 1 / r2), s, h + c)
                                     .line(-s - tb_s, 0)
                                     .extrude(gp_Vec(b, 0, 0)),
                                 -b / 2, 0, 0);
  std::set<int> excluded;
  for (double pin_number : p.list("excluded_pins")) excluded.insert(static_cast<int>(pin_number));
  std::vector<TopoDS_Shape> pins;
  int counter = 1;
  const double first_x = (npx - 1) * e / 2, first_y = (npy - 1) * e / 2;
  auto side = [&](int count, double first, double y, double degrees) {
    for (int i = 0; i < count; ++i, ++counter)
      if (!excluded.count(counter)) pins.push_back(turned(moved(pin, first - i * e, y, 0), degrees));
  };
  side(npx, first_x, 0, 180);
  side(npy, first_y, (D1 - E1) / 2, 270);
  side(npx, first_x, 0, 0);
  side(npy, first_y, (D1 - E1) / 2, 90);
  const std::vector<double> epad = p.list("epad");
  if (epad.size() == 1) {
    pins.push_back(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp::DZ()), epad[0], A1).Shape());
  } else if (epad.size() >= 2) {
    const double rotation = epad.size() > 2 ? epad[2] : 0, dx = epad.size() > 3 ? epad[3] : 0, dy = epad.size() > 4 ? epad[4] : 0;
    pins.push_back(turned(box(epad[0], epad[1], A1, dx, dy, 0), rotation));
  }
  const TopoDS_Shape all = compound(pins);
  body = cut(body, all);
  const double rotation = p.number("rotation", 0);
  return {"", {{turned(body, rotation), color(p.text("body_color_key"))},
               {turned(all, rotation), color(p.text("pin_color_key"))},
               {turned(mark, rotation), color(p.text("mark_color_key"))}}};
}

const Row *find(std::string_view name) {
  static const std::unordered_map<std::string_view, const Row *> index = [] {
    std::unordered_map<std::string_view, const Row *> out;
    for (const Row &row : kRows) out.emplace(row.name, &row);
    return out;
  }();
  const auto found = index.find(name);
  return found == index.end() ? nullptr : found->second;
}

} // namespace

std::string model_name(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
  const size_t dot = path.rfind('.');
  if (dot != std::string_view::npos) {
    const std::string_view ext = path.substr(dot + 1);
    if (ext == "wrl" || ext == "step" || ext == "stp" || ext == "WRL" || ext == "STEP" || ext == "STP")
      path = path.substr(0, dot);
  }
  return std::string(path);
}

std::optional<Part> generate(std::string_view name, std::string *error) {
  const Row *row = find(name);
  if (!row) return std::nullopt;
  try {
    const Params params(row->params);
    const std::string family = row->family;
    Part part = family == "c_chip"       ? chip(params, false)
                : family == "r_chip"     ? chip(params, true)
                : family == "pin_header" ? pin_header(params)
                                         : gull_wing(params);
    part.family = family;
    return part;
  } catch (const Standard_Failure &failure) {
    if (error) *error = std::string("OCCT: ") + failure.GetMessageString();
  } catch (const std::exception &failure) {
    if (error) *error = failure.what();
  }
  return std::nullopt;
}

std::string cache_key(std::string_view name) {
  const Row *row = find(name);
  if (!row) return {};
  uint64_t hash = 14695981039346656037ull; // FNV-1a over the parameter text
  for (const char *c = row->params; *c; ++c) hash = (hash ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
  return "v" + std::to_string(kGeneratorVersion) + "-" + row->family + "-" + hex;
}

std::vector<std::string> names() {
  std::vector<std::string> out;
  for (const Row &row : kRows) out.emplace_back(row.name);
  return out;
}

} // namespace corvcad::parts3d

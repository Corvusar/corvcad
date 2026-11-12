#include "corvcad/authoring.hpp"
#include "corvcad/eda.hpp"
#include "corvcad/engine.hpp"
#include "corvcad/schematic_route.hpp"
#include "corvcad/starter_parts.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace corvcad {
namespace {
namespace fs = std::filesystem;

const char *symbol_definition = two_pin_symbol_definition;

const char *two_pad_library = R"KICAD((footprint "TwoPad"
  (version 20260206) (generator "corvcad") (layer "F.Cu")
  (property "Reference" "REF**" (at 0 -2 0) (layer "F.SilkS")
    (effects (font (size 1 1) (thickness 0.15))))
  (property "Value" "TwoPad" (at 0 2 0) (layer "F.Fab")
    (effects (font (size 1 1) (thickness 0.15))))
  (attr smd)
  (pad "1" smd rect (at -0.8 0) (size 0.8 1.0) (layers "F.Cu" "F.Paste" "F.Mask"))
  (pad "2" smd rect (at 0.8 0) (size 0.8 1.0) (layers "F.Cu" "F.Paste" "F.Mask"))
  (embedded_fonts no)
)
)KICAD";

const char *hole_library = R"KICAD((footprint "MountingHole3.2"
  (version 20260206) (generator "corvcad") (layer "F.Cu")
  (property "Reference" "REF**" (at 0 -3 0) (layer "F.SilkS")
    (effects (font (size 1 1) (thickness 0.15))))
  (property "Value" "MountingHole" (at 0 3 0) (layer "F.Fab")
    (effects (font (size 1 1) (thickness 0.15))))
  (attr board_only exclude_from_pos_files exclude_from_bom)
  (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2)
    (layers "*.Cu" "*.Mask"))
  (embedded_fonts no)
)
)KICAD";

std::string uuid() {
  std::random_device random;
  std::uniform_int_distribution<unsigned> digit(0, 15);
  std::string id = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  for (char &c : id) {
    if (c == 'x') c = "0123456789abcdef"[digit(random)];
    else if (c == 'y') c = "89ab"[digit(random) % 4];
  }
  return id;
}

std::string number(double value) {
  if (!std::isfinite(value)) throw std::runtime_error("Position must be finite");
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::fixed << std::setprecision(3) << value;
  std::string text = stream.str();
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return text == "-0" ? "0" : text;
}

std::string quoted(const std::string &value) {
  std::string text = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') text += '\\';
    if (c == '\n') text += "\\n";
    else if (c == '\r') text += "\\r";
    else if (c == '\t') text += "\\t";
    else text += c;
  }
  return text + '"';
}

size_t closing(const std::string &source, size_t opening) {
  int depth = 0;
  bool in_string = false, escaped = false, comment = false;
  for (size_t i = opening; i < source.size(); ++i) {
    const char c = source[i];
    if (comment) { if (c == '\n') comment = false; continue; }
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == ';') { comment = true; continue; }
    if (c == '"') { in_string = true; continue; }
    if (c == '(') ++depth;
    if (c == ')' && --depth == 0) return i;
  }
  throw std::runtime_error("Unclosed KiCad expression");
}

std::string append(const std::string &source, const std::string &node) {
  const auto opening = source.find('(');
  if (opening == std::string::npos) throw std::runtime_error("Invalid KiCad document ending");
  const auto end = closing(source, opening);
  bool comment = false;
  for (size_t i = end + 1; i < source.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    if (comment) { if (c == '\n') comment = false; continue; }
    if (c == ';') { comment = true; continue; }
    if (!std::isspace(c)) throw std::runtime_error("Invalid KiCad document ending");
  }
  std::string result = source;
  result.insert(end, "  " + node + "\n");
  return result;
}

int next_reference(const std::vector<std::string> &existing, char prefix) {
  for (int candidate = 1; candidate < 10000; ++candidate) {
    const std::string wanted = std::string(1, prefix) + std::to_string(candidate);
    if (std::find(existing.begin(), existing.end(), wanted) == existing.end()) return candidate;
  }
  throw std::runtime_error("No free reference designator");
}

void schematic_position(double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > 297 || y > 210)
    throw std::runtime_error("Schematic position is outside the A4 page");
}

void board_position(const Board &board, double x, double y, double inset) {
  if (!std::isfinite(x) || !std::isfinite(y) ||
      x < board.min_x + inset || x > board.max_x - inset ||
      y < board.min_y + inset || y > board.max_y - inset)
    throw std::runtime_error("Place the part farther inside the board outline");
}

void write_if_missing(const fs::path &path, const std::string &data) {
  if (fs::exists(path)) return;
  std::ofstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot create project library");
  file << data;
  if (!file) throw std::runtime_error("Cannot finish project library");
}
} // namespace

void ensure_authoring_libraries(const std::string &project_directory) {
  const fs::path folder = utf8_path(project_directory);
  if (!fs::is_directory(folder)) throw std::runtime_error("Project folder is missing");
  const fs::path pretty = folder / "CorvCAD.pretty";
  fs::create_directory(pretty);
  write_if_missing(folder / "sym-lib-table",
      "(sym_lib_table\n  (version 7)\n"
      "  (lib (name \"CorvCAD\") (type \"KiCad\")"
      " (uri \"${KIPRJMOD}/CorvCAD.kicad_sym\") (options \"\")"
      " (descr \"CorvCAD starter symbols\"))\n)\n");
  write_if_missing(folder / "fp-lib-table",
      "(fp_lib_table\n  (version 7)\n"
      "  (lib (name \"CorvCAD\") (type \"KiCad\")"
      " (uri \"${KIPRJMOD}/CorvCAD.pretty\") (options \"\")"
      " (descr \"CorvCAD starter footprints\"))\n)\n");
  write_if_missing(folder / "CorvCAD.kicad_sym",
      "(kicad_symbol_lib\n  (version 20251024)\n"
      "  (generator \"corvcad\")\n  (generator_version \"10.0\")\n" +
      std::string(symbol_definition) + "\n)\n");
  write_if_missing(pretty / "TwoPad.kicad_mod", std::string(two_pad_library) + "\n");
  write_if_missing(pretty / "MountingHole3.2.kicad_mod", std::string(hole_library) + "\n");
}

std::string add_schematic_two_pin(const std::string &source, const std::string &project,
                                  double x, double y, const std::string &instance_path,
                                  const std::vector<std::string> &taken_references) {
  schematic_position(x, y);
  if (project.empty() || project.find_first_of("\"\\\r\n") != std::string::npos)
    throw std::runtime_error("Invalid project name");
  const Schematic model = parse_kicad_schematic(source);
  if (model.uuid.empty()) throw std::runtime_error("Schematic has no sheet UUID");
  for (const auto &placed : model.symbols)
    if (std::hypot(placed.position.x - x, placed.position.y - y) < 10.16)
      throw std::runtime_error("Place the part farther from an existing symbol");
  std::vector<std::string> references = taken_references;
  for (const auto &symbol : model.symbols) references.push_back(symbol.reference);
  const std::string path = instance_path.empty() ? "/" + model.uuid : instance_path;
  if (path.find_first_of("\"\\\r\n") != std::string::npos)
    throw std::runtime_error("Invalid sheet instance path");
  const std::string reference = "R" + std::to_string(next_reference(references, 'R'));
  const std::string sx = number(x), sy = number(y);
  std::string result = source;
  if (result.find("(symbol \"CorvCAD:TwoPin\"") == std::string::npos) {
    const auto start = result.find("(lib_symbols");
    if (start == std::string::npos) throw std::runtime_error("Schematic has no symbol library section");
    const auto end = closing(result, start);
    std::string definition = symbol_definition;
    definition.replace(definition.find("\"TwoPin\""), 8, "\"CorvCAD:TwoPin\"");
    result.insert(end, "\n    " + definition + "\n  ");
  }
  const std::string symbol =
      "(symbol (lib_id \"CorvCAD:TwoPin\") (at " + sx + " " + sy + " 0) (unit 1)\n"
      "    (in_bom yes) (on_board yes) (dnp no) (uuid \"" + uuid() + "\")\n"
      "    (property \"Reference\" " + quoted(reference) + " (at " + sx + " " + number(y - 5.08) +
      " 0) (effects (font (size 1.27 1.27))))\n"
      "    (property \"Value\" \"10k\" (at " + sx + " " + number(y + 5.08) +
      " 0) (effects (font (size 1.27 1.27))))\n"
      "    (property \"Footprint\" \"CorvCAD:TwoPad\" (at " + sx + " " + sy +
      " 0) (effects (font (size 1.27 1.27)) (hide yes)))\n"
      "    (pin \"1\" (uuid \"" + uuid() + "\"))\n"
      "    (pin \"2\" (uuid \"" + uuid() + "\"))\n"
      "    (instances (project " + quoted(project) + " (path " + quoted(path) +
      " (reference " + quoted(reference) + ") (unit 1))))\n  )";
  result = append(result, symbol);
  parse_kicad_schematic(result);
  return result;
}

namespace {
// KiCad's JUNCTION_HELPERS::AnalyzePoint for wires (eeschema/junction_helpers.cpp): count the
// distinct directions leaving `p`. A wire end gives its direction and a pin one of its own; a
// wire passing through gives both, but only where a wire end, pin or label breaks it there.
// Three or more need an explicit junction dot.
// ponytail: wires and symbol pins only; add sheet pins and bus entries when touch edits reach them.
bool junction_needed(const Schematic &model, Point p) {
  const auto at = [&](Point q) { return std::hypot(q.x - p.x, q.y - p.y) < 0.005; };
  const auto angle = [&](Point to) {
    return static_cast<int>(std::lround(std::atan2(to.y - p.y, to.x - p.x) * 1800 / 3.141592653589793));
  };
  for (const auto &junction : model.junctions)
    if (at(junction.position)) return false;
  std::vector<int> angles;
  std::vector<const SchWire *> through;
  bool broken = false;
  int unique = 100000;
  for (const auto &wire : model.wires) {
    if (at(wire.start) && at(wire.end)) continue;
    if (at(wire.start) || at(wire.end)) {
      angles.push_back(angle(at(wire.start) ? wire.end : wire.start));
      broken = true;
    } else {
      const double dx = wire.end.x - wire.start.x, dy = wire.end.y - wire.start.y;
      const double t = ((p.x - wire.start.x) * dx + (p.y - wire.start.y) * dy) / (dx * dx + dy * dy);
      if (t > 0 && t < 1 && at({wire.start.x + t * dx, wire.start.y + t * dy})) through.push_back(&wire);
    }
  }
  for (const auto &symbol : model.symbols)
    for (const auto &pin : symbol.pins)
      if (at(pin)) { angles.push_back(unique++); broken = true; }
  for (const auto &label : model.labels)
    if (at(label.position)) broken = true;
  if (broken)
    for (const auto *wire : through) {
      angles.push_back(angle(wire->start));
      angles.push_back(angle(wire->end));
    }
  std::sort(angles.begin(), angles.end());
  return std::unique(angles.begin(), angles.end()) - angles.begin() >= 3;
}
} // namespace

std::string add_schematic_wire(const std::string &source, double x0, double y0,
                               double x1, double y1) {
  schematic_position(x0, y0); schematic_position(x1, y1);
  if (std::hypot(x1 - x0, y1 - y0) < 0.01) throw std::runtime_error("Wire needs two distinct points");
  parse_kicad_schematic(source);
  const std::string node = "(wire (pts (xy " + number(x0) + " " + number(y0) +
      ") (xy " + number(x1) + " " + number(y1) +
      ")) (stroke (width 0) (type default)) (uuid \"" + uuid() + "\"))";
  auto result = append(source, node);
  const Schematic model = parse_kicad_schematic(result);
  for (const Point end : {Point{x0, y0}, Point{x1, y1}})
    if (junction_needed(model, end))
      result = append(result, "(junction (at " + number(end.x) + " " + number(end.y) +
                      ") (diameter 0) (color 0 0 0 0) (uuid \"" + uuid() + "\"))");
  return result;
}

namespace {
// KiCad treats a pin end, wire end, junction or label lying on a wire's interior
// as a connection point (eeschema junction_helpers.cpp / SCH_SCREEN::IsJunction).
// A route must not pass over them, or through a part's body and pin lines. The box is
// shrunk by 0.01 mm so the pin ends on its edge stay reachable, as in the native backend.
RouteObstacles route_obstacles(const Schematic &model) {
  RouteObstacles result;
  for (const auto &symbol : model.symbols) {
    result.points.insert(result.points.end(), symbol.pins.begin(), symbol.pins.end());
    if (symbol.has_body)
      result.boxes.push_back({symbol.body[0] + 0.01, symbol.body[1] + 0.01,
                              symbol.body[2] - 0.01, symbol.body[3] - 0.01});
  }
  for (const auto &wire : model.wires) {
    result.points.push_back(wire.start);
    result.points.push_back(wire.end);
  }
  for (const auto &junction : model.junctions) result.points.push_back(junction.position);
  for (const auto &label : model.labels) result.points.push_back(label.position);
  for (const auto &flag : model.no_connects) result.points.push_back(flag.position);
  return result;
}
} // namespace

std::vector<Point> route_schematic_wire(const std::string &source, double x0, double y0,
                                        double x1, double y1) {
  return route_schematic_wire(parse_kicad_schematic(source), x0, y0, x1, y1);
}

std::vector<Point> route_schematic_wire(const Schematic &sheet, double x0, double y0,
                                        double x1, double y1) {
  schematic_position(x0, y0); schematic_position(x1, y1);
  return plan_orthogonal_route(route_obstacles(sheet), x0, y0, x1, y1);
}

std::string add_schematic_orthogonal_wire(const std::string &source, double x0,
                                          double y0, double x1, double y1) {
  const auto path = route_schematic_wire(source, x0, y0, x1, y1);
  std::string result = source;
  for (size_t i = 0; i + 1 < path.size(); ++i)
    result = add_schematic_wire(result, path[i].x, path[i].y, path[i + 1].x, path[i + 1].y);
  return result;
}

std::string add_schematic_label(const std::string &source, const std::string &name,
                                double x, double y) {
  schematic_position(x, y);
  if (name.empty() || name.size() > 64) throw std::runtime_error("Enter a label of 1–64 characters");
  parse_kicad_schematic(source);
  const std::string node = "(label " + quoted(name) + " (at " + number(x) + " " + number(y) +
      " 0) (effects (font (size 1.27 1.27)) (justify left bottom))"
      " (uuid \"" + uuid() + "\"))";
  const auto result = append(source, node);
  parse_kicad_schematic(result);
  return result;
}

std::string add_board_two_pad(const std::string &source, const std::string &reference,
                              const std::string &value, double x, double y) {
  const Board board = parse_kicad_board(source);
  board_position(board, x, y, 3);
  if (reference.empty() || reference[0] != 'R' || reference.size() > 8 ||
      reference.find_first_not_of("0123456789", 1) != std::string::npos ||
      reference.size() == 1)
    throw std::runtime_error("Use a resistor reference such as R1");
  for (const auto &part : board.placements)
    if (part.reference == reference) throw std::runtime_error("Reference is already on the board");
  if (value.empty() || value.size() > 64) throw std::runtime_error("Enter a part value");
  const std::string node =
      "(footprint \"CorvCAD:TwoPad\" (layer \"F.Cu\")\n"
      "    (uuid \"" + uuid() + "\") (at " + number(x) + " " + number(y) + ")\n"
      "    (property \"Reference\" " + quoted(reference) +
      " (at 0 -2 0) (layer \"F.SilkS\") (uuid \"" + uuid() +
      "\") (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (property \"Value\" " + quoted(value) +
      " (at 0 2 0) (layer \"F.Fab\") (uuid \"" + uuid() +
      "\") (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (attr smd)\n"
      "    (pad \"1\" smd rect (at -0.8 0) (size 0.8 1.0)"
      " (layers \"F.Cu\" \"F.Paste\" \"F.Mask\") (uuid \"" + uuid() + "\"))\n"
      "    (pad \"2\" smd rect (at 0.8 0) (size 0.8 1.0)"
      " (layers \"F.Cu\" \"F.Paste\" \"F.Mask\") (uuid \"" + uuid() + "\"))\n  )";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

std::string add_board_track(const std::string &source, double x0, double y0, double x1, double y1,
                            const std::string &layer, const std::string &net, double width) {
  const Board board = parse_kicad_board(source);
  board_position(board, x0, y0, 0);
  board_position(board, x1, y1, 0);
  if (layer != "F.Cu" && layer != "B.Cu") throw std::runtime_error("Tracks go on F.Cu or B.Cu");
  if (std::hypot(x1 - x0, y1 - y0) < 0.01) throw std::runtime_error("A track needs two different ends");
  if (!std::isfinite(width) || width < 0.1 || width > 5) throw std::runtime_error("Use a track width of 0.1-5 mm");
  const std::string node =
      "(segment (start " + number(x0) + " " + number(y0) + ") (end " + number(x1) + " " + number(y1) +
      ") (width " + number(width) + ") (layer " + quoted(layer) + ")" +
      (net.empty() ? "" : " (net " + quoted(net) + ")") + " (uuid \"" + uuid() + "\"))";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

std::string add_board_via(const std::string &source, double x, double y, const std::string &net,
                          double size, double drill) {
  const Board board = parse_kicad_board(source);
  board_position(board, x, y, size / 2);
  if (!std::isfinite(size) || !std::isfinite(drill) || drill < 0.1 || size < drill + 0.1)
    throw std::runtime_error("A via needs a drill of at least 0.1 mm inside its pad");
  // Overlapping drills are never valid (KiCad DRC: co-located holes); a second tap on a via
  // must not stack another one.
  for (const auto &via : board.vias)
    if (std::hypot(via.center.x - x, via.center.y - y) < (via.drill + drill) / 2)
      throw std::runtime_error("A via is already there");
  const std::string node =
      "(via (at " + number(x) + " " + number(y) + ") (size " + number(size) + ") (drill " + number(drill) +
      ") (layers \"F.Cu\" \"B.Cu\")" + (net.empty() ? "" : " (net " + quoted(net) + ")") +
      " (uuid \"" + uuid() + "\"))";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

namespace {
// A simple polygon (no edge crosses another) of 3-256 corners, as KiCad needs for an outline
// or zone, written as KiCad's (pts ...).
std::string polygon_points(const std::vector<Point> &points) {
  if (points.size() < 3 || points.size() > 256) throw std::runtime_error("A shape needs 3-256 corners");
  const size_t n = points.size();
  double area = 0;
  for (size_t i = 0; i < n; ++i) {
    const Point a = points[i], b = points[(i + 1) % n];
    if (!std::isfinite(a.x) || !std::isfinite(a.y)) throw std::runtime_error("Position must be finite");
    if (std::hypot(b.x - a.x, b.y - a.y) < 0.01) throw std::runtime_error("Two corners of the shape are the same");
    area += a.x * b.y - b.x * a.y;
  }
  if (std::abs(area) < 0.01) throw std::runtime_error("The shape has no area");
  const auto cross = [](Point o, Point a, Point b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) {
      if (j == i + 1 || (i == 0 && j == n - 1)) continue; // neighbours share a corner
      const Point a = points[i], b = points[(i + 1) % n], c = points[j], d = points[(j + 1) % n];
      if (cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0)
        throw std::runtime_error("The shape's edges cross");
    }
  std::string text = "(pts";
  for (const auto &p : points) text += " (xy " + number(p.x) + " " + number(p.y) + ")";
  return text + ")";
}

bool drawing_layer(const std::string &layer) {
  return layer == "F.SilkS" || layer == "B.SilkS" || layer == "F.Fab" || layer == "B.Fab";
}
} // namespace

std::string set_board_outline(const std::string &source, const std::vector<Point> &points) {
  parse_kicad_board(source);
  const std::string node = "(gr_poly " + polygon_points(points) +
      " (stroke (width 0.05) (type default)) (fill no) (layer \"Edge.Cuts\") (uuid \"" + uuid() + "\"))";
  const auto result = append(remove_board_outline(source), node);
  parse_kicad_board(result);
  return result;
}

std::string add_board_zone(const std::string &source, const std::vector<Point> &points,
                           const std::string &layer, const std::string &net) {
  parse_kicad_board(source);
  if (layer != "F.Cu" && layer != "B.Cu") throw std::runtime_error("Zones go on F.Cu or B.Cu");
  // KiCad's default zone settings (ZONE_SETTINGS); unfilled until KiCad's zone filler runs.
  const std::string node =
      "(zone" + (net.empty() ? std::string() : " (net " + quoted(net) + ")") + " (layer " + quoted(layer) +
      ") (uuid \"" + uuid() + "\") (hatch edge 0.5) (connect_pads (clearance 0.5)) (min_thickness 0.25)"
      " (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon " + polygon_points(points) + "))";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

std::string add_board_line(const std::string &source, double x0, double y0, double x1, double y1,
                           const std::string &layer, double width) {
  parse_kicad_board(source);
  if (!drawing_layer(layer)) throw std::runtime_error("Lines go on F.SilkS, B.SilkS, F.Fab or B.Fab");
  if (std::hypot(x1 - x0, y1 - y0) < 0.01) throw std::runtime_error("A line needs two different ends");
  if (!std::isfinite(width) || width < 0.05 || width > 5) throw std::runtime_error("Use a line width of 0.05-5 mm");
  const std::string node =
      "(gr_line (start " + number(x0) + " " + number(y0) + ") (end " + number(x1) + " " + number(y1) +
      ") (stroke (width " + number(width) + ") (type solid)) (layer " + quoted(layer) + ") (uuid \"" + uuid() + "\"))";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

std::string add_board_text(const std::string &source, const std::string &text, double x, double y,
                           const std::string &layer, double size) {
  parse_kicad_board(source);
  if (!drawing_layer(layer)) throw std::runtime_error("Text goes on F.SilkS, B.SilkS, F.Fab or B.Fab");
  if (text.empty() || text.size() > 128 ||
      std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32; }))
    throw std::runtime_error("Enter one line of 1-128 characters");
  if (!std::isfinite(size) || size < 0.5 || size > 10) throw std::runtime_error("Use a text size of 0.5-10 mm");
  // Back-side text reads mirrored from the front, as KiCad places it.
  const std::string node =
      "(gr_text " + quoted(text) + " (at " + number(x) + " " + number(y) + " 0) (layer " + quoted(layer) +
      ") (uuid \"" + uuid() + "\") (effects (font (size " + number(size) + " " + number(size) + ") (thickness " +
      number(size * 0.15) + "))" + (layer[0] == 'B' ? " (justify mirror)" : "") + "))";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}

BoardUpdate update_board_from_schematic(const std::string &board_source, const std::string &schematic_source) {
  const Schematic schematic = parse_kicad_schematic(schematic_source);
  if (!schematic.sheets.empty())
    throw std::runtime_error("Update PCB supports single-sheet schematics for now");
  BoardUpdate update{board_source, {}, {}};
  std::map<std::string, std::map<std::string, std::string>> nets;
  for (const auto &symbol : schematic.symbols)
    if (!symbol.power && !symbol.reference.empty() && symbol.reference[0] != '#' && !symbol.footprint.empty())
      nets[symbol.reference];
  for (const auto &net : schematic_nets(schematic))
    for (const auto &pin : net.pins) {
      const auto found = nets.find(schematic.symbols[pin.symbol].reference);
      if (found != nets.end()) found->second[pin.number] = net.name;
    }
  for (const auto &symbol : schematic.symbols) {
    if (!nets.count(symbol.reference)) continue;
    const Board board = parse_kicad_board(update.board);
    const auto placed = std::find_if(board.placements.begin(), board.placements.end(),
                                     [&](const Placement &p) { return p.reference == symbol.reference; });
    if (placed != board.placements.end()) {
      // KiCad would swap the footprint; this subset leaves it and its nets alone.
      if (placed->library != symbol.footprint) { update.skipped.push_back(symbol.reference); nets.erase(symbol.reference); }
      continue;
    }
    if (symbol.footprint != "CorvCAD:TwoPad") { update.skipped.push_back(symbol.reference); continue; }
    // ponytail: first free 5 mm grid point at least 6 mm from every footprint; the user
    // arranges parts afterwards, as after KiCad's own update.
    bool placed_new = false;
    for (double y = board.min_y + 5; y <= board.max_y - 5 && !placed_new; y += 5)
      for (double x = board.min_x + 5; x <= board.max_x - 5 && !placed_new; x += 5) {
        const bool free = std::none_of(board.placements.begin(), board.placements.end(), [&](const Placement &p) {
          return std::hypot(p.center.x - x, p.center.y - y) < 6;
        });
        if (!free) continue;
        try {
          update.board = add_board_two_pad(update.board, symbol.reference,
                                           symbol.value.empty() ? "TwoPad" : symbol.value, x, y);
          update.added.push_back(symbol.reference);
        } catch (const std::exception &) {} // a reference add_board_two_pad does not accept
        placed_new = true;
      }
    if (update.added.empty() || update.added.back() != symbol.reference) {
      update.skipped.push_back(symbol.reference);
      nets.erase(symbol.reference);
    }
  }
  update.board = set_pad_nets(update.board, nets);
  return update;
}

std::string add_board_mounting_hole(const std::string &source, double x, double y) {
  const Board board = parse_kicad_board(source);
  board_position(board, x, y, 5);
  std::vector<std::string> references;
  for (const auto &part : board.placements) references.push_back(part.reference);
  const std::string reference = "H" + std::to_string(next_reference(references, 'H'));
  const std::string node =
      "(footprint \"CorvCAD:MountingHole3.2\" (layer \"F.Cu\")\n"
      "    (uuid \"" + uuid() + "\") (at " + number(x) + " " + number(y) + ")\n"
      "    (property \"Reference\" " + quoted(reference) +
      " (at 0 -3 0) (layer \"F.SilkS\") (uuid \"" + uuid() +
      "\") (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (property \"Value\" \"MountingHole\" (at 0 3 0) (layer \"F.Fab\")"
      " (uuid \"" + uuid() + "\") (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (attr board_only exclude_from_pos_files exclude_from_bom)\n"
      "    (pad \"\" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2)"
      " (layers \"*.Cu\" \"*.Mask\") (uuid \"" + uuid() + "\"))\n  )";
  const auto result = append(source, node);
  parse_kicad_board(result);
  return result;
}
} // namespace corvcad

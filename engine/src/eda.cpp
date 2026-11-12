#include "corvcad/eda.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace corvcad {
namespace {
struct Node {
  std::string atom;
  std::vector<Node> children;
  size_t begin = 0, end = 0;
};

struct Reader {
  const std::string &source;
  size_t position = 0;

  void skip() {
    while (position < source.size()) {
      const unsigned char c = static_cast<unsigned char>(source[position]);
      if (std::isspace(c)) { ++position; continue; }
      if (c == ';') {
        while (position < source.size() && source[position] != '\n') ++position;
        continue;
      }
      break;
    }
  }

  Node read(int depth = 0) {
    skip();
    if (position >= source.size()) throw std::runtime_error("Unexpected end of schematic");
    if (depth > 256) throw std::runtime_error("Schematic nesting is too deep");
    Node node;
    node.begin = position;
    if (source[position] == '(') {
      ++position;
      skip();
      while (position < source.size() && source[position] != ')') {
        node.children.push_back(read(depth + 1));
        skip();
      }
      if (position == source.size()) throw std::runtime_error("Unclosed schematic expression");
      ++position;
    } else if (source[position] == '"') {
      ++position;
      while (position < source.size() && source[position] != '"') {
        if (source[position] == '\\') {
          ++position;
          if (position == source.size()) throw std::runtime_error("Bad schematic escape");
          switch (source[position]) {
          case 'n': node.atom += '\n'; break;
          case 'r': node.atom += '\r'; break;
          case 't': node.atom += '\t'; break;
          default: node.atom += source[position]; break;
          }
          ++position;
        } else {
          node.atom += source[position++];
        }
      }
      if (position == source.size()) throw std::runtime_error("Unclosed schematic string");
      ++position;
    } else {
      if (source[position] == ')') throw std::runtime_error("Unexpected schematic closing parenthesis");
      while (position < source.size() && !std::isspace(static_cast<unsigned char>(source[position]))
             && source[position] != '(' && source[position] != ')')
        node.atom += source[position++];
    }
    node.end = position;
    return node;
  }
};

std::string head(const Node &node) {
  return node.children.empty() ? "" : node.children.front().atom;
}
const Node *child(const Node &node, const std::string &name) {
  for (const auto &candidate : node.children)
    if (head(candidate) == name) return &candidate;
  return nullptr;
}
std::string val(const Node *node, size_t index = 1) {
  return node && node->children.size() > index ? node->children[index].atom : "";
}
double number(const Node *node, size_t index = 1) {
  if (!node || node->children.size() <= index) throw std::runtime_error("Missing schematic coordinate");
  const auto &atom = node->children[index].atom;
  size_t consumed = 0;
  double result = 0;
  try { result = std::stod(atom, &consumed); }
  catch (...) { throw std::runtime_error("Invalid schematic coordinate"); }
  if (consumed != atom.size() || !std::isfinite(result))
    throw std::runtime_error("Invalid schematic coordinate");
  return result;
}
Point point(const Node *node) { return {number(node), number(node, 2)}; }

Node document(const std::string &source) {
  Reader reader{source};
  Node root = reader.read();
  reader.skip();
  if (reader.position != source.size()) throw std::runtime_error("Trailing schematic content");
  if (head(root) != "kicad_sch") throw std::runtime_error("Expected a .kicad_sch document");
  return root;
}

const Node *placed_symbol(const Node &root, const std::string &uuid) {
  const Node *found = nullptr;
  for (const auto &node : root.children) {
    if (head(node) != "symbol" || !child(node, "lib_id")) continue;
    if (val(child(node, "uuid")) != uuid) continue;
    if (found) throw std::runtime_error("Duplicate schematic symbol UUID");
    found = &node;
  }
  if (!found) throw std::runtime_error("Schematic symbol UUID not found");
  return found;
}

const Node *placed_item(const Node &root, const std::string &uuid) {
  if (uuid.empty()) throw std::runtime_error("Missing schematic item UUID");
  const Node *found = nullptr;
  for (const auto &node : root.children) {
    const std::string kind = head(node);
    if (kind != "symbol" && kind != "wire" && kind != "junction" &&
        kind != "label" && kind != "global_label" && kind != "hierarchical_label") continue;
    if (kind == "symbol" && !child(node, "lib_id")) continue;
    if (val(child(node, "uuid")) != uuid) continue;
    if (found) throw std::runtime_error("Duplicate schematic item UUID");
    found = &node;
  }
  if (!found) throw std::runtime_error("Schematic item UUID not found");
  return found;
}

std::string decimal(double value) {
  if (!std::isfinite(value)) throw std::runtime_error("Schematic coordinate must be finite");
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(6) << value;
  std::string result = output.str();
  while (result.find('.') != std::string::npos && result.back() == '0') result.pop_back();
  if (result.back() == '.') result.pop_back();
  return result == "-0" ? "0" : result;
}

std::string quoted(const std::string &value) {
  std::string result = "\"";
  for (const char c : value) {
    switch (c) {
    case '\\': result += "\\\\"; break;
    case '"': result += "\\\""; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result += c; break;
    }
  }
  return result + '"';
}

// Library to sheet coordinates as KiCad's TRANSFORM does: flip Y, rotate counter-clockwise
// on screen, then mirror ("mirror x" flips across the X axis). Rounded to 1 nm.
Point place(const SchSymbol &symbol, char mirror, double x, double y) {
  const double r = symbol.rotation * 3.14159265358979323846 / 180.0;
  double dx = x * std::cos(r) - y * std::sin(r), dy = -x * std::sin(r) - y * std::cos(r);
  if (mirror == 'x') dy = -dy;
  if (mirror == 'y') dx = -dx;
  const auto nm = [](double v) { return std::round(v * 1e6) / 1e6; };
  return {nm(symbol.position.x + dx), nm(symbol.position.y + dy)};
}

// "hide", "(hide yes)", or either inside (effects …).
bool hidden(const Node &node) {
  for (const auto &part : node.children)
    if (part.atom == "hide" || (head(part) == "hide" && val(&part) != "no") ||
        (head(part) == "effects" && hidden(part)))
      return true;
  return false;
}

// Pin ends and body box of a placed symbol from its embedded definition. Sub-symbols are
// named <name>_<unit>_<body style>; unit or style 0 is shared by all.
void symbol_geometry(const Node &placed, const Node &definition, SchSymbol &symbol) {
  const int unit = child(placed, "unit") ? static_cast<int>(number(child(placed, "unit"))) : 1;
  const Node *style_node = child(placed, "body_style") ? child(placed, "body_style") : child(placed, "convert");
  const int style = style_node ? static_cast<int>(number(style_node)) : 1;
  const char mirror = val(child(placed, "mirror")).empty() ? 0 : val(child(placed, "mirror"))[0];
  symbol.power = child(definition, "power") != nullptr;
  if (const Node *names = child(definition, "pin_names")) {
    if (child(*names, "offset")) symbol.pin_name_offset = number(child(*names, "offset"));
    symbol.pin_names_visible = !hidden(*names);
  }
  if (const Node *numbers = child(definition, "pin_numbers")) symbol.pin_numbers_visible = !hidden(*numbers);
  double box[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  const auto extend = [&](double x, double y) {
    const Point p = place(symbol, mirror, x, y);
    box[0] = std::min(box[0], p.x); box[1] = std::min(box[1], p.y);
    box[2] = std::max(box[2], p.x); box[3] = std::max(box[3], p.y);
  };
  for (const auto &part : definition.children) {
    if (head(part) != "symbol") continue;
    const std::string name = val(&part);
    const size_t b = name.rfind('_'), a = b == std::string::npos ? b : name.rfind('_', b - 1);
    if (a == std::string::npos) continue;
    int part_unit = 0, part_style = 0;
    try { part_unit = std::stoi(name.substr(a + 1, b - a - 1)); part_style = std::stoi(name.substr(b + 1)); }
    catch (...) { continue; }
    symbol.unit = unit;
    symbol.unit_count = std::max(symbol.unit_count, part_unit);
    // A placed body style 0 shows every style's items, as KiCad draws it.
    if ((part_unit != 0 && part_unit != unit) || (part_style != 0 && style != 0 && part_style != style)) continue;
    for (const auto &item : part.children) {
      const std::string kind = head(item);
      if (kind == "pin") {
        const Node *at = child(item, "at");
        const double x = number(at), y = number(at, 2);
        const double angle = at->children.size() > 3 ? number(at, 3) * 3.14159265358979323846 / 180.0 : 0;
        const double length = child(item, "length") ? number(child(item, "length")) : 0;
        symbol.pins.push_back(place(symbol, mirror, x, y));
        symbol.pin_ends.push_back(place(symbol, mirror, x + length * std::cos(angle), y + length * std::sin(angle)));
        symbol.pin_numbers.push_back(val(child(item, "number")));
        symbol.pin_names.push_back(val(child(item, "name")));
        // KiCad's SCH_PIN::IsGlobalPower: a power input that is hidden or on a power symbol.
        bool hidden = false;
        for (const auto &part_of_pin : item.children)
          hidden |= part_of_pin.atom == "hide" || (head(part_of_pin) == "hide" && val(&part_of_pin) != "no");
        symbol.pin_visible.push_back(!hidden);
        symbol.pin_global_power.push_back(item.children.size() > 1 && item.children[1].atom == "power_in" &&
                                          (hidden || symbol.power));
        extend(x, y);
        extend(x + length * std::cos(angle), y + length * std::sin(angle));
        continue;
      }
      // Body shapes, sampled into library-coordinate points, then placed like the pins.
      std::vector<Point> points;
      bool closed = false;
      const auto circle = [&](double cx, double cy, double radius, double from, double sweep, int steps) {
        for (int i = 0; i <= steps; ++i)
          points.push_back({cx + radius * std::cos(from + sweep * i / steps), cy + radius * std::sin(from + sweep * i / steps)});
      };
      if (kind == "rectangle") {
        const double x0 = number(child(item, "start")), y0 = number(child(item, "start"), 2);
        const double x1 = number(child(item, "end")), y1 = number(child(item, "end"), 2);
        points = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        closed = true;
      } else if (kind == "circle") {
        circle(number(child(item, "center")), number(child(item, "center"), 2), number(child(item, "radius")),
               0, 2 * 3.14159265358979323846, 32);
        closed = true;
      } else if (kind == "arc" && child(item, "start") && child(item, "mid") && child(item, "end")) {
        // The circle through start, mid and end; swept from start through mid to end.
        const double ax = number(child(item, "start")), ay = number(child(item, "start"), 2);
        const double bx = number(child(item, "mid")), by = number(child(item, "mid"), 2);
        const double cx = number(child(item, "end")), cy = number(child(item, "end"), 2);
        const double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
        if (std::abs(d) < 1e-12) {
          points = {{ax, ay}, {bx, by}, {cx, cy}};
        } else {
          const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
          const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
          const double two_pi = 2 * 3.14159265358979323846;
          const auto turn = [&](double from, double to) { return std::fmod(std::fmod(to - from, two_pi) + two_pi, two_pi); };
          const double start = std::atan2(ay - uy, ax - ux);
          const double to_mid = turn(start, std::atan2(by - uy, bx - ux)), to_end = turn(start, std::atan2(cy - uy, cx - ux));
          // Counter-clockwise when mid comes before end that way, else clockwise.
          const double sweep = to_mid <= to_end ? to_end : to_end - two_pi;
          circle(ux, uy, std::hypot(ax - ux, ay - uy), start, sweep, 16);
        }
      } else if (kind == "polyline" || kind == "bezier") {
        if (const Node *pts = child(item, "pts"))
          for (const auto &xy : pts->children)
            if (head(xy) == "xy") points.push_back({number(&xy), number(&xy, 2)});
      }
      if (points.empty()) continue;
      SchGraphic graphic;
      graphic.closed = closed;
      const Node *fill_node = child(item, "fill");
      const std::string fill = fill_node ? val(child(*fill_node, "type")) : "";
      graphic.fill = fill == "outline" || fill == "color" ? 1 : fill == "background" ? 2 : 0;
      for (const Point &p : points) {
        extend(p.x, p.y);
        graphic.points.push_back(place(symbol, mirror, p.x, p.y));
      }
      symbol.graphics.push_back(std::move(graphic));
    }
  }
  if (box[0] <= box[2]) {
    symbol.body = {box[0], box[1], box[2], box[3]};
    symbol.has_body = true;
  }
}

struct Edit { size_t begin, end; std::string replacement; };
std::string apply_edits(std::string source, std::vector<Edit> edits) {
  std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) {
    return a.begin > b.begin;
  });
  size_t previous = source.size();
  for (const auto &edit : edits) {
    if (edit.end > previous || edit.begin > edit.end)
      throw std::runtime_error("Overlapping schematic edits");
    source.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    previous = edit.begin;
  }
  return source;
}
}

Schematic parse_kicad_schematic(const std::string &source) {
  const Node root = document(source);
  Schematic result;
  result.uuid = val(child(root, "uuid"));
  std::map<std::string, const Node *> library;
  if (const Node *symbols = child(root, "lib_symbols"))
    for (const auto &definition : symbols->children)
      if (head(definition) == "symbol") library[val(&definition)] = &definition;
  if (const Node *version = child(root, "version")) {
    const std::string text = val(version);
    size_t used = 0;
    try { result.version = std::stoi(text, &used); }
    catch (...) { throw std::runtime_error("Invalid schematic version"); }
    if (used != text.size()) throw std::runtime_error("Invalid schematic version");
  }
  // KiCad's PAGE_INFO sizes, landscape; "portrait" turns a standard size, "User" gives its own.
  if (const Node *paper = child(root, "paper")) {
    static const std::map<std::string, Point> sizes = {
        {"A5", {210, 148}}, {"A4", {297, 210}}, {"A3", {420, 297}}, {"A2", {594, 420}},
        {"A1", {841, 594}}, {"A0", {1189, 841}}, {"A", {279.4, 215.9}}, {"B", {431.8, 279.4}},
        {"C", {558.8, 431.8}}, {"D", {863.6, 558.8}}, {"E", {1117.6, 863.6}},
        {"USLetter", {279.4, 215.9}}, {"USLegal", {355.6, 215.9}}, {"USLedger", {431.8, 279.4}}};
    const std::string name = val(paper);
    if (name == "User" && paper->children.size() > 3) {
      result.paper = {number(paper, 2), number(paper, 3)};
    } else if (const auto found = sizes.find(name); found != sizes.end()) {
      result.paper = found->second;
      if (val(paper, 2) == "portrait") std::swap(result.paper.x, result.paper.y);
    }
  }
  if (const Node *block = child(root, "title_block")) {
    result.title = val(child(*block, "title"));
    result.date = val(child(*block, "date"));
    result.revision = val(child(*block, "rev"));
    result.company = val(child(*block, "company"));
  }
  for (const auto &node : root.children) {
    const std::string kind = head(node);
    // Top-level cases accepted by KiCad 10.0.6's sch_io_kicad_sexpr_parser.cpp.
    // Preserve known but unmodelled constructs; flag unknown ones before save.
    static const std::set<std::string> known = {
        "kicad_sch", "version", "generator", "generator_version", "host", "uuid",
        "paper", "page", "title_block", "lib_symbols", "symbol", "image", "sheet",
        "junction", "no_connect", "bus_entry", "polyline", "bus", "wire", "arc",
        "circle", "rectangle", "bezier", "rule_area", "netclass_flag", "text",
        "label", "global_label", "hierarchical_label", "directive_label",
        "text_box", "table", "sheet_instances", "symbol_instances", "bus_alias",
        "embedded_fonts", "embedded_files", "group"};
    if (!kind.empty() && !known.count(kind))
      result.unknown_top_level.push_back(kind);
    if (kind == "symbol" && child(node, "lib_id")) {
      SchSymbol symbol;
      symbol.uuid = val(child(node, "uuid"));
      symbol.library_id = val(child(node, "lib_id"));
      const Node *at = child(node, "at");
      symbol.position = point(at);
      if (at->children.size() > 3) symbol.rotation = number(at, 3);
      for (const auto &property : node.children) {
        if (head(property) != "property") continue;
        const std::string name = val(&property);
        if (name == "Reference") symbol.reference = val(&property, 2);
        if (name == "Value") symbol.value = val(&property, 2);
        if (name == "Footprint") symbol.footprint = val(&property, 2);
        if (name != "Reference" && name != "Value") continue;
        const Node *field_at = child(property, "at");
        if (!field_at) continue;
        const double angle = field_at->children.size() > 3 ? number(field_at, 3) : 0;
        const auto turned = [](double a) { return std::fmod(std::abs(a), 180) == 90; };
        (name == "Reference" ? symbol.reference_field : symbol.value_field) =
            {point(field_at), !hidden(property), turned(angle) != turned(symbol.rotation)};
      }
      // As a root sheet, KiCad takes the reference from this sheet's instance path; the
      // property can be stale in copied designs.
      if (const Node *instances = child(node, "instances"))
        for (const auto &project : instances->children)
          for (const auto &path : project.children)
            if (head(path) == "path" && val(&path) == "/" + result.uuid && !val(child(path, "reference")).empty())
              symbol.reference = val(child(path, "reference"));
      // KiCad looks the cached definition up by lib_name when a symbol has one.
      const std::string key = child(node, "lib_name") ? val(child(node, "lib_name")) : symbol.library_id;
      if (const auto found = library.find(key); found != library.end())
        symbol_geometry(node, *found->second, symbol);
      result.symbols.push_back(std::move(symbol));
    } else if (kind == "wire" || kind == "bus") {
      const Node *pts = child(node, "pts");
      if (!pts) continue;
      std::vector<Point> ends;
      for (const auto &part : pts->children)
        if (head(part) == "xy") ends.push_back(point(&part));
      if (ends.size() == 2) {
        auto &lines = kind == "wire" ? result.wires : result.buses;
        lines.push_back({ends[0], ends[1], val(child(node, "uuid"))});
      }
    } else if (kind == "bus_entry") {
      const Point at = point(child(node, "at")), size = point(child(node, "size"));
      result.bus_entries.push_back({at, {at.x + size.x, at.y + size.y}, val(child(node, "uuid"))});
    } else if (kind == "junction") {
      result.junctions.push_back({point(child(node, "at")), val(child(node, "uuid"))});
    } else if (kind == "label" || kind == "global_label" || kind == "hierarchical_label") {
      result.labels.push_back({point(child(node, "at")), val(&node), val(child(node, "uuid")), kind});
    } else if (kind == "no_connect") {
      result.no_connects.push_back({point(child(node, "at")), val(child(node, "uuid"))});
    } else if (kind == "text") {
      result.texts.push_back({point(child(node, "at")), val(&node), val(child(node, "uuid")), kind});
    } else if (kind == "polyline") {
      // A graphic note line or path: one segment per pair of points, sharing the UUID.
      const Node *pts = child(node, "pts");
      std::vector<Point> corners;
      if (pts)
        for (const auto &part : pts->children)
          if (head(part) == "xy") corners.push_back(point(&part));
      for (size_t i = 1; i < corners.size(); ++i)
        result.note_lines.push_back({corners[i - 1], corners[i], val(child(node, "uuid"))});
    } else if (kind == "sheet") {
      SchSheet sheet;
      sheet.uuid = val(child(node, "uuid"));
      if (const Node *at = child(node, "at")) sheet.position = point(at);
      if (const Node *size = child(node, "size")) sheet.size = point(size);
      for (const auto &property : node.children) {
        if (head(property) != "property") continue;
        if (val(&property) == "Sheetname") sheet.name = val(&property, 2);
        if (val(&property) == "Sheetfile") sheet.file = val(&property, 2);
      }
      for (const auto &pin : node.children)
        if (head(pin) == "pin")
          sheet.pins.push_back({point(child(pin, "at")), val(&pin), val(child(pin, "uuid")), val(&pin, 2)});
      result.sheets.push_back(std::move(sheet));
    }
  }
  return result;
}

std::vector<SchNet> schematic_nets(const Schematic &schematic, const std::string &sheet_path) {
  struct PinRef { size_t symbol, pin; };
  std::vector<PinRef> pins;
  for (size_t s = 0; s < schematic.symbols.size(); ++s)
    for (size_t p = 0; p < schematic.symbols[s].pins.size(); ++p) pins.push_back({s, p});
  const auto &wires = schematic.wires;
  const auto &labels = schematic.labels;
  const auto &junctions = schematic.junctions;
  // Node order: pins, wires, labels, junctions.
  const size_t first_wire = pins.size(), first_label = first_wire + wires.size(),
               first_junction = first_label + labels.size();
  std::vector<size_t> parent(first_junction + junctions.size());
  for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
  const std::function<size_t(size_t)> find = [&](size_t i) {
    return parent[i] == i ? i : parent[i] = find(parent[i]);
  };
  const auto unite = [&](size_t a, size_t b) { parent[find(a)] = find(b); };
  const auto same = [](Point a, Point b) { return std::abs(a.x - b.x) < 1e-4 && std::abs(a.y - b.y) < 1e-4; };
  const auto at_end = [&](const SchWire &w, Point p) { return same(w.start, p) || same(w.end, p); };
  const auto along = [&](const SchWire &w, Point p) {
    const double dx = w.end.x - w.start.x, dy = w.end.y - w.start.y, length = dx * dx + dy * dy;
    if (length == 0) return same(w.start, p);
    const double t = ((p.x - w.start.x) * dx + (p.y - w.start.y) * dy) / length;
    if (t < -1e-9 || t > 1 + 1e-9) return false;
    return same({w.start.x + t * dx, w.start.y + t * dy}, p);
  };
  const auto pin_at = [&](size_t i) { return schematic.symbols[pins[i].symbol].pins[pins[i].pin]; };
  const auto global_power = [&](size_t i) {
    const auto &symbol = schematic.symbols[pins[i].symbol];
    return pins[i].pin < symbol.pin_global_power.size() && symbol.pin_global_power[pins[i].pin];
  };
  // Hidden power inputs of ordinary parts connect by name only (they are often stacked).
  const auto by_name_only = [&](size_t i) { return global_power(i) && !schematic.symbols[pins[i].symbol].power; };
  const auto power_name = [&](size_t i) {
    const auto &symbol = schematic.symbols[pins[i].symbol];
    return symbol.power ? symbol.value : symbol.pin_names[pins[i].pin];
  };

  // ponytail: O(n^2) pair scan, fine for hand-drawn sheets; bucket by grid point past a few
  // thousand items.
  for (size_t a = 0; a < pins.size(); ++a) {
    if (by_name_only(a)) continue;
    for (size_t b = a + 1; b < pins.size(); ++b)
      if (!by_name_only(b) && same(pin_at(a), pin_at(b))) unite(a, b);
    for (size_t w = 0; w < wires.size(); ++w)
      if (at_end(wires[w], pin_at(a))) unite(a, first_wire + w);
    for (size_t l = 0; l < labels.size(); ++l)
      if (same(labels[l].position, pin_at(a))) unite(a, first_label + l);
    for (size_t j = 0; j < junctions.size(); ++j)
      if (same(junctions[j].position, pin_at(a))) unite(a, first_junction + j);
  }
  for (size_t w = 0; w < wires.size(); ++w) {
    for (size_t v = w + 1; v < wires.size(); ++v)
      if (at_end(wires[v], wires[w].start) || at_end(wires[v], wires[w].end)) unite(first_wire + w, first_wire + v);
    for (size_t l = 0; l < labels.size(); ++l)
      if (along(wires[w], labels[l].position)) unite(first_wire + w, first_label + l);
    for (size_t j = 0; j < junctions.size(); ++j)
      if (along(wires[w], junctions[j].position)) unite(first_wire + w, first_junction + j);
  }
  for (size_t l = 0; l < labels.size(); ++l)
    for (size_t j = 0; j < junctions.size(); ++j)
      if (same(labels[l].position, junctions[j].position)) unite(first_label + l, first_junction + j);
  // Same names join: labels by text, power symbols by Value.
  std::map<std::string, size_t> named;
  const auto join_name = [&](const std::string &name, size_t node) {
    const auto [found, added] = named.emplace(name, node);
    if (!added) unite(node, found->second);
  };
  for (size_t l = 0; l < labels.size(); ++l) join_name(labels[l].text, first_label + l);
  for (size_t i = 0; i < pins.size(); ++i)
    if (global_power(i)) join_name(power_name(i), i);

  std::map<size_t, SchNet> groups;
  for (size_t i = 0; i < pins.size(); ++i) {
    const auto &symbol = schematic.symbols[pins[i].symbol];
    groups[find(i)].pins.push_back({pins[i].symbol, symbol.pin_numbers[pins[i].pin], pin_at(i)});
  }
  for (size_t w = 0; w < wires.size(); ++w) groups[find(first_wire + w)].wires.push_back(w);
  for (size_t l = 0; l < labels.size(); ++l) groups[find(first_label + l)].labels.push_back(l);

  std::vector<SchNet> nets;
  for (auto &group : groups) {
    SchNet &net = group.second;
    // Driver order as KiCad ranks them: global label, power symbol, local label, hierarchical
    // label; the alphabetically first wins a tie.
    int best = -1;
    const auto offer = [&](int rank, const std::string &name) {
      if (rank > best || (rank == best && name < net.name)) { best = rank; net.name = name; }
    };
    for (const size_t l : net.labels) {
      const auto &label = labels[l];
      if (label.kind == "global_label") offer(4, label.text);
      else if (label.kind == "hierarchical_label") offer(1, sheet_path + label.text);
      else offer(2, sheet_path + label.text);
    }
    // A pin's own name when it says more than its number ("C" for Q1 pin 2).
    const auto pin_name = [&](const SchNetPin &pin) {
      const auto &symbol = schematic.symbols[pin.symbol];
      const auto at = std::find(symbol.pin_numbers.begin(), symbol.pin_numbers.end(), pin.number);
      const auto index = static_cast<size_t>(at - symbol.pin_numbers.begin());
      const std::string name = index < symbol.pin_names.size() ? symbol.pin_names[index] : "";
      return name == "~" || name == pin.number ? std::string() : name;
    };
    const SchNetPin *first = nullptr;
    size_t component_pins = 0;
    for (const auto &pin : net.pins) {
      const auto &symbol = schematic.symbols[pin.symbol];
      const auto at = std::find(symbol.pin_numbers.begin(), symbol.pin_numbers.end(), pin.number);
      const auto index = static_cast<size_t>(at - symbol.pin_numbers.begin());
      if (index < symbol.pin_global_power.size() && symbol.pin_global_power[index])
        offer(3, symbol.power ? symbol.value : symbol.pin_names[index]);
      if (symbol.power || symbol.reference.empty() || symbol.reference[0] == '#') continue;
      ++component_pins;
    }
    // KiCad's default names: the unit letter follows the reference of a multi-unit part, and a
    // pin's name replaces "Pad<number>". Named pins win, then the first name as plain text.
    // ponytail: KiCad also ranks pin types first (inputs before passives), which shows only on
    // parts like valves whose named pins tie; the names differ, the nets do not.
    const auto default_name = [&](const SchNetPin &pin, bool unconnected) {
      const auto &symbol = schematic.symbols[pin.symbol];
      std::string reference = symbol.reference;
      if (symbol.unit_count > 1 && symbol.unit >= 1 && symbol.unit <= 26)
        reference += static_cast<char>('A' + symbol.unit - 1);
      const std::string name = pin_name(pin);
      return unconnected
          ? "unconnected-(" + reference + "-" + (name.empty() ? "" : name + "-") + "Pad" + pin.number + ")"
          : "Net-(" + reference + "-" + (name.empty() ? "Pad" + pin.number : name) + ")";
    };
    for (const auto &pin : net.pins) {
      const auto &symbol = schematic.symbols[pin.symbol];
      if (symbol.power || symbol.reference.empty() || symbol.reference[0] == '#') continue;
      const bool named = !pin_name(pin).empty(), first_named = first && !pin_name(*first).empty();
      if (!first || (named && !first_named) ||
          (named == first_named && default_name(pin, false) < default_name(*first, false)))
        first = &pin;
    }
    if (best < 0 && first) net.name = default_name(*first, component_pins == 1);
    nets.push_back(std::move(net));
  }
  return nets;
}

std::string move_schematic_symbol(const std::string &source, const std::string &uuid,
                                  double x, double y) {
  const Node root = document(source);
  const Node *symbol = placed_symbol(root, uuid);
  const auto model = parse_kicad_schematic(source);
  const auto moving = std::find_if(model.symbols.begin(), model.symbols.end(),
                                   [&](const SchSymbol &item) { return item.uuid == uuid; });
  if (moving != model.symbols.end() && moving->library_id == "CorvCAD:TwoPin")
    for (const auto &other : model.symbols)
      if (other.uuid != uuid && std::hypot(other.position.x - x, other.position.y - y) < 10.16)
        throw std::runtime_error("Place the part farther from an existing symbol");
  const Node *at = child(*symbol, "at");
  const Point old = point(at);
  const double dx = x - old.x, dy = y - old.y;
  std::vector<Edit> edits;
  edits.push_back({at->children[1].begin, at->children[1].end, decimal(x)});
  edits.push_back({at->children[2].begin, at->children[2].end, decimal(y)});
  for (const auto &property : symbol->children) {
    if (head(property) != "property") continue;
    const Node *property_at = child(property, "at");
    if (!property_at) continue;
    const Point location = point(property_at);
    edits.push_back({property_at->children[1].begin, property_at->children[1].end,
                     decimal(location.x + dx)});
    edits.push_back({property_at->children[2].begin, property_at->children[2].end,
                     decimal(location.y + dy)});
  }
  return apply_edits(source, std::move(edits));
}

// KiCad's rotate (SCH_SYMBOL::Rotate, counter-clockwise on screen): the angle grows by 90 and
// each field turns about the symbol origin, its text switching between horizontal and vertical.
std::string rotate_schematic_symbol(const std::string &source, const std::string &uuid) {
  const Node root = document(source);
  const Node *symbol = placed_symbol(root, uuid);
  const Node *at = child(*symbol, "at");
  const Point origin = point(at);
  std::vector<Edit> edits;
  const auto turn = [&](const Node *node, double degrees) {
    const std::string angle = decimal(std::fmod(degrees, 360));
    if (node->children.size() > 3) edits.push_back({node->children[3].begin, node->children[3].end, angle});
    else edits.push_back({node->children[2].end, node->children[2].end, " " + angle});
  };
  turn(at, (at->children.size() > 3 ? number(at, 3) : 0) + 90);
  for (const auto &property : symbol->children) {
    const Node *property_at = head(property) == "property" ? child(property, "at") : nullptr;
    if (!property_at) continue;
    const Point p = point(property_at);
    edits.push_back({property_at->children[1].begin, property_at->children[1].end,
                     decimal(origin.x + (p.y - origin.y))});
    edits.push_back({property_at->children[2].begin, property_at->children[2].end,
                     decimal(origin.y - (p.x - origin.x))});
    const double angle = property_at->children.size() > 3 ? number(property_at, 3) : 0;
    turn(property_at, std::fmod(angle, 180) == 0 ? 90 : 0);
  }
  return apply_edits(source, std::move(edits));
}

std::string set_schematic_property(const std::string &source, const std::string &uuid,
                                   const std::string &property, const std::string &value) {
  if (property != "Value" && property != "Footprint")
    throw std::runtime_error("Only Value and Footprint properties can be edited");
  const Node root = document(source);
  const Node *symbol = placed_symbol(root, uuid);
  for (const auto &entry : symbol->children) {
    if (head(entry) == "property" && val(&entry) == property && entry.children.size() > 2) {
      const Node &old = entry.children[2];
      return apply_edits(source, {{old.begin, old.end, quoted(value)}});
    }
  }
  throw std::runtime_error("Schematic property not found");
}

std::string delete_schematic_item(const std::string &source, const std::string &uuid) {
  const Node root = document(source);
  const Node *item = placed_item(root, uuid);
  const auto edited = apply_edits(source, {{item->begin, item->end, ""}});
  parse_kicad_schematic(edited);
  return edited;
}

std::string rename_schematic_symbol(const std::string &source, const std::string &uuid,
                                    const std::string &reference) {
  if (reference.empty() || reference.size() > 16 ||
      !std::isalpha(static_cast<unsigned char>(reference.front())) ||
      !std::all_of(reference.begin() + 1, reference.end(), [](unsigned char c) {
        return std::isalnum(c) != 0;
      })) throw std::runtime_error("Use a reference such as R1");
  const Node root = document(source);
  const Node *symbol = placed_symbol(root, uuid);
  for (const auto &other : parse_kicad_schematic(source).symbols)
    if (other.uuid != uuid && other.reference == reference)
      throw std::runtime_error("Reference is already used");
  std::vector<Edit> edits;
  for (const auto &entry : symbol->children)
    if (head(entry) == "property" && val(&entry) == "Reference" && entry.children.size() > 2)
      edits.push_back({entry.children[2].begin, entry.children[2].end, quoted(reference)});
  const Node *instances = child(*symbol, "instances");
  if (instances) {
    std::function<void(const Node &)> visit = [&](const Node &node) {
      if (head(node) == "reference" && node.children.size() > 1)
        edits.push_back({node.children[1].begin, node.children[1].end, quoted(reference)});
      for (const auto &nested : node.children) visit(nested);
    };
    visit(*instances);
  }
  if (edits.empty()) throw std::runtime_error("Schematic reference not found");
  const auto edited = apply_edits(source, std::move(edits));
  parse_kicad_schematic(edited);
  return edited;
}

std::string rename_schematic_label(const std::string &source, const std::string &uuid,
                                   const std::string &name) {
  if (name.empty() || name.size() > 64) throw std::runtime_error("Enter a net name of 1–64 characters");
  const Node root = document(source);
  const Node *label = placed_item(root, uuid);
  const std::string kind = head(*label);
  if ((kind != "label" && kind != "global_label" && kind != "hierarchical_label") ||
      label->children.size() < 2) throw std::runtime_error("Item is not a label");
  const auto &text = label->children[1];
  const auto edited = apply_edits(source, {{text.begin, text.end, quoted(name)}});
  parse_kicad_schematic(edited);
  return edited;
}

std::string move_schematic_label(const std::string &source, const std::string &uuid,
                                 double x, double y) {
  const Node root = document(source);
  const Node *label = placed_item(root, uuid);
  const std::string kind = head(*label);
  if (kind != "label" && kind != "global_label" && kind != "hierarchical_label")
    throw std::runtime_error("Item is not a label");
  const Node *at = child(*label, "at");
  if (!at || at->children.size() < 3) throw std::runtime_error("Label has no position");
  const auto edited = apply_edits(source, {
      {at->children[1].begin, at->children[1].end, decimal(x)},
      {at->children[2].begin, at->children[2].end, decimal(y)}});
  parse_kicad_schematic(edited);
  return edited;
}
}

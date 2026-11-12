#include "corvcad/library.hpp"
#include "corvcad/eda.hpp"
#include "corvcad/engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace corvcad {
namespace {
namespace fs = std::filesystem;

// An s-expression reader that keeps each node's source range (engine.cpp and eda.cpp keep
// their own, private ones).
struct Node { std::string atom; std::vector<Node> children; size_t begin = 0, end = 0; };
struct Reader {
  const std::string &s; size_t p = 0;
  void skip() {
    while (p < s.size()) {
      if (std::isspace(static_cast<unsigned char>(s[p]))) { ++p; continue; }
      if (s[p] == ';') { while (p < s.size() && s[p] != '\n') ++p; continue; }
      break;
    }
  }
  Node read() {
    skip();
    if (p >= s.size()) throw std::runtime_error("Unexpected end of KiCad file");
    Node n; n.begin = p;
    if (s[p] == '(') {
      ++p; skip();
      while (p < s.size() && s[p] != ')') { n.children.push_back(read()); skip(); }
      if (p == s.size()) throw std::runtime_error("Unclosed KiCad expression");
      n.end = ++p; return n;
    }
    if (s[p] == '"') {
      ++p;
      while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) ++p;
        n.atom += s[p++];
      }
      if (p == s.size()) throw std::runtime_error("Unclosed KiCad string");
      ++p;
    } else {
      while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '(' && s[p] != ')') n.atom += s[p++];
    }
    n.end = p; return n;
  }
};
Node document(const std::string &text) { Reader reader{text}; return reader.read(); }
std::string head(const Node &n) { return n.children.empty() ? "" : n.children.front().atom; }
const Node *child(const Node &n, const std::string &name) {
  for (const auto &c : n.children) if (head(c) == name) return &c;
  return nullptr;
}
std::string val(const Node *n, size_t i = 1) { return n && n->children.size() > i ? n->children[i].atom : ""; }

std::string number(double value) {
  if (!std::isfinite(value)) throw std::runtime_error("Sizes must be finite");
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(4) << value;
  std::string text = out.str();
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return text == "-0" ? "0" : text;
}
std::string quoted(const std::string &value) {
  std::string text = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') text += '\\';
    text += c;
  }
  return text + '"';
}
std::string json(const std::string &value) {
  std::string text = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { text += '\\'; text += static_cast<char>(c); }
    else if (c == '\n') text += "\\n";
    else if (c < 32) { char code[8]; std::snprintf(code, sizeof code, "\\u%04x", c); text += code; }
    else text += static_cast<char>(c);
  }
  return text + '"';
}

std::string read_file(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot read " + utf8_string(path.filename()));
  return {std::istreambuf_iterator<char>(file), {}};
}
// Written beside the target and renamed over it, so a failed write leaves the old file.
void write_file(const fs::path &path, const std::string &text) {
  fs::path temporary = path;
  temporary += ".corvcad-new";
  {
    std::ofstream file(temporary, std::ios::binary);
    if (!file || !(file << text)) throw std::runtime_error("Cannot write " + utf8_string(path.filename()));
  }
  fs::rename(temporary, path);
}

// Library and item names: what KiCad accepts in a library ID, kept to file-name-safe characters.
void check_name(const std::string &name, const char *what) {
  if (name.empty() || name.size() > 64 ||
      !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+';
      }) || name.front() == '.')
    throw std::runtime_error(std::string("Use letters, digits, _ - . + for the ") + what + " name");
}

const char *table_file(bool footprint) { return footprint ? "fp-lib-table" : "sym-lib-table"; }
struct Row { std::string name, uri; size_t begin = 0, end = 0; };
std::vector<Row> rows(const std::string &table) {
  std::vector<Row> result;
  const Node root = document(table);
  for (const auto &lib : root.children)
    if (head(lib) == "lib") result.push_back({val(child(lib, "name")), val(child(lib, "uri")), lib.begin, lib.end});
  return result;
}
// The library's path when its URI lies in the project folder (${KIPRJMOD}), else empty.
fs::path local_path(const fs::path &folder, const std::string &uri) {
  const std::string prefix = "${KIPRJMOD}/";
  if (uri.rfind(prefix, 0) != 0) return {};
  const fs::path path = (folder / utf8_path(uri.substr(prefix.size()))).lexically_normal();
  const auto relative = path.lexically_relative(folder.lexically_normal());
  if (relative.empty() || *relative.begin() == "..") return {};
  return path;
}
std::string table_text(const fs::path &folder, bool footprint) {
  const fs::path path = folder / table_file(footprint);
  if (fs::exists(path)) return read_file(path);
  return std::string("(") + (footprint ? "fp_lib_table" : "sym_lib_table") + "\n  (version 7)\n)\n";
}
// An editable library's path: a row of the project table whose file is in the project folder.
fs::path editable_library(const fs::path &folder, bool footprint, const std::string &library) {
  if (library == "CorvCAD") throw std::runtime_error("CorvCAD's starter library is read-only");
  for (const auto &row : rows(table_text(folder, footprint)))
    if (row.name == library) {
      const fs::path path = local_path(folder, row.uri);
      if (path.empty()) throw std::runtime_error("Only libraries inside the project folder can be edited");
      return path;
    }
  throw std::runtime_error("No project library named " + library);
}
void add_row(const fs::path &folder, bool footprint, const std::string &name, const std::string &file) {
  std::string table = table_text(folder, footprint);
  for (const auto &row : rows(table))
    if (row.name == name) throw std::runtime_error("The project already has a library named " + name);
  const size_t close = table.rfind(')');
  if (close == std::string::npos) throw std::runtime_error("Invalid library table");
  table.insert(close, "  (lib (name " + quoted(name) + ") (type \"KiCad\") (uri " + quoted("${KIPRJMOD}/" + file) +
                          ") (options \"\") (descr \"\"))\n");
  write_file(folder / table_file(footprint), table);
}
// Removes a node with its indentation and line break.
std::string erase(std::string text, size_t begin, size_t end) {
  while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
  if (end < text.size() && text[end] == '\r') ++end;
  if (end < text.size() && text[end] == '\n') ++end;
  return text.erase(begin, end - begin);
}

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> result;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    result.push_back(line);
  }
  return result;
}
std::vector<std::string> fields(const std::string &line) {
  std::vector<std::string> result;
  size_t start = 0;
  for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1)
    result.push_back(line.substr(start, tab - start));
  result.push_back(line.substr(start));
  return result;
}
double decimal(const std::string &text) {
  std::istringstream in(text); in.imbue(std::locale::classic());
  double value = 0;
  if (!(in >> value) || !std::isfinite(value)) throw std::runtime_error("Not a number: " + text);
  return value;
}

// Symbol listing: fields and pins from the definition, drawing from the schematic reader (the
// symbol placed at the origin of a minimal sheet), so it matches the schematic canvas.
std::string symbol_item(const std::string &source, const Node &symbol, const std::map<std::string, const Node *> &all) {
  const Node *base = &symbol;
  for (int depth = 0; depth < 8 && child(*base, "extends"); ++depth) {
    const auto found = all.find(val(child(*base, "extends")));
    if (found == all.end()) break;
    base = found->second;
  }
  std::map<std::string, std::string> property;
  for (const Node *from : {base, &symbol})
    for (const auto &item : from->children)
      if (head(item) == "property") property[val(&item)] = val(&item, 2);
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "{\"name\":" << json(val(&symbol)) << ",\"reference\":" << json(property["Reference"])
      << ",\"value\":" << json(property["Value"]) << ",\"footprint\":" << json(property["Footprint"])
      << ",\"description\":" << json(property["Description"]) << ",\"filters\":" << json(property["ki_fp_filters"])
      << ",\"derived\":" << (base != &symbol ? "true" : "false") << ",\"pins\":[";
  bool first = true;
  for (const auto &unit : base->children) {
    if (head(unit) != "symbol") continue;
    for (const auto &pin : unit.children) {
      if (head(pin) != "pin") continue;
      const Node *at = child(pin, "at");
      const double angle = at && at->children.size() > 3 ? decimal(val(at, 3)) : 0;
      out << (first ? "" : ",") << "[" << json(val(child(pin, "number"))) << "," << json(val(child(pin, "name")))
          << "," << json(pin.children.size() > 1 ? pin.children[1].atom : "") << ","
          << json(angle == 180 ? "right" : angle == 0 ? "left" : angle == 90 ? "bottom" : "top") << "]";
      first = false;
    }
  }
  out << "],\"shape\":{\"graphics\":[";
  try {
    const std::string definition = source.substr(base->begin, base->end - base->begin);
    const Schematic sheet = parse_kicad_schematic(
        "(kicad_sch (version 20250114) (uuid \"0\") (lib_symbols " + definition + ") (symbol (lib_id " +
        quoted(val(base)) + ") (at 0 0 0) (unit 1) (uuid \"1\")))");
    if (sheet.symbols.empty()) throw std::runtime_error("no symbol");
    const SchSymbol &drawn = sheet.symbols.front();
    for (size_t i = 0; i < drawn.graphics.size(); ++i) {
      out << (i ? "," : "") << "[" << (drawn.graphics[i].closed ? 1 : 0) << "," << drawn.graphics[i].fill;
      for (const auto &p : drawn.graphics[i].points) out << "," << number(p.x) << "," << number(p.y);
      out << "]";
    }
    out << "],\"pins\":[";
    for (size_t i = 0; i < drawn.pins.size(); ++i)
      out << (i ? "," : "") << "[" << number(drawn.pins[i].x) << "," << number(drawn.pins[i].y) << ","
          << number(drawn.pin_ends[i].x) << "," << number(drawn.pin_ends[i].y) << "," << json(drawn.pin_numbers[i])
          << "," << json(drawn.pin_names[i]) << "]";
    out << "]}}";
  } catch (const std::exception &) {
    out << "],\"pins\":[]}}";
  }
  return out.str();
}

std::string footprint_item(const std::string &text, const std::string &file_name) {
  const Node root = document(text);
  if (head(root) != "footprint") throw std::runtime_error(file_name + " is not a KiCad footprint");
  const Node *attr = child(root, "attr");
  bool smd = false;
  if (attr) for (const auto &a : attr->children) smd |= a.atom == "smd";
  // The board reader places pads; the footprint goes on a large board at the origin.
  std::string placed = text;
  if (!child(root, "at") && root.children.size() > 1) placed.insert(root.children[1].end, " (at 0 0)");
  const Board board = parse_kicad_board(
      "(kicad_pcb (gr_rect (start -1000 -1000) (end 1000 1000) (layer \"Edge.Cuts\")) " + placed + ")");
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "{\"name\":" << json(val(&root)) << ",\"description\":" << json(val(child(root, "descr")))
      << ",\"smd\":" << (smd ? "true" : "false") << ",\"pads\":[";
  for (size_t i = 0; i < board.pads.size(); ++i) {
    const Pad &pad = board.pads[i];
    out << (i ? "," : "") << "[" << number(pad.center.x) << "," << number(pad.center.y) << "," << number(pad.size_x)
        << "," << number(pad.size_y) << "," << number(pad.rotation) << "," << json(pad.shape) << ","
        << json(pad.number) << "," << json(pad.type) << "]";
  }
  out << "]}";
  return out.str();
}

std::string library_json(const fs::path &folder, bool footprint, const Row &row) {
  const fs::path path = local_path(folder, row.uri);
  std::ostringstream out;
  out << "{\"name\":" << json(row.name) << ",\"uri\":" << json(row.uri)
      << ",\"editable\":" << (!path.empty() && row.name != "CorvCAD" ? "true" : "false") << ",";
  std::string items, error;
  try {
    if (path.empty()) throw std::runtime_error("Outside the project folder");
    if (footprint) {
      if (!fs::is_directory(path)) throw std::runtime_error("Missing " + utf8_string(path.filename()));
      std::vector<fs::path> files;
      for (const auto &entry : fs::directory_iterator(path))
        if (entry.path().extension() == ".kicad_mod") files.push_back(entry.path());
      std::sort(files.begin(), files.end());
      for (const auto &file : files) {
        try { items += (items.empty() ? "" : ",") + footprint_item(read_file(file), utf8_string(file.filename())); }
        catch (const std::exception &e) { error = e.what(); }
      }
    } else {
      const std::string source = read_file(path);
      const Node root = document(source);
      if (head(root) != "kicad_symbol_lib") throw std::runtime_error(row.name + " is not a KiCad symbol library");
      std::map<std::string, const Node *> all;
      for (const auto &symbol : root.children) if (head(symbol) == "symbol") all[val(&symbol)] = &symbol;
      for (const auto &symbol : root.children)
        if (head(symbol) == "symbol") items += (items.empty() ? "" : ",") + symbol_item(source, symbol, all);
    }
  } catch (const std::exception &e) {
    error = e.what();
  }
  out << "\"error\":" << json(error) << ",\"items\":[" << items << "]}";
  return out.str();
}
} // namespace

SymbolSpec parse_symbol_spec(const std::string &text) {
  SymbolSpec spec;
  for (const auto &line : lines(text)) {
    const auto f = fields(line);
    if (f[0] == "name" && f.size() > 1) spec.name = f[1];
    else if (f[0] == "reference" && f.size() > 1) spec.reference = f[1];
    else if (f[0] == "value" && f.size() > 1) spec.value = f[1];
    else if (f[0] == "footprint" && f.size() > 1) spec.footprint = f[1];
    else if (f[0] == "description" && f.size() > 1) spec.description = f[1];
    else if (f[0] == "pin" && f.size() == 5) spec.pins.push_back({f[1], f[2], f[3], f[4] == "right"});
    else if (!f[0].empty()) throw std::runtime_error("Unknown symbol field: " + f[0]);
  }
  return spec;
}

FootprintSpec parse_footprint_spec(const std::string &text) {
  FootprintSpec spec;
  for (const auto &line : lines(text)) {
    const auto f = fields(line);
    if (f[0] == "name" && f.size() > 1) spec.name = f[1];
    else if (f[0] == "description" && f.size() > 1) spec.description = f[1];
    else if (f[0] == "type" && f.size() > 1) spec.smd = f[1] == "smd";
    else if (f[0] == "pads" && f.size() > 1) spec.pads = static_cast<int>(decimal(f[1]));
    else if (f[0] == "rows" && f.size() > 1) spec.rows = static_cast<int>(decimal(f[1]));
    else if (f[0] == "pitch" && f.size() > 1) spec.pitch = decimal(f[1]);
    else if (f[0] == "span" && f.size() > 1) spec.span = decimal(f[1]);
    else if (f[0] == "pad" && f.size() > 2) { spec.pad_width = decimal(f[1]); spec.pad_height = decimal(f[2]); }
    else if (f[0] == "drill" && f.size() > 1) spec.drill = decimal(f[1]);
    else if (!f[0].empty()) throw std::runtime_error("Unknown footprint field: " + f[0]);
  }
  return spec;
}

std::string library_symbol_text(const SymbolSpec &spec) {
  check_name(spec.name, "symbol");
  if (spec.reference.empty() || spec.reference.size() > 8 ||
      !std::all_of(spec.reference.begin(), spec.reference.end(), [](unsigned char c) { return std::isalpha(c); }))
    throw std::runtime_error("Use a reference prefix of 1-8 letters, such as U or J");
  if (spec.pins.empty() || spec.pins.size() > 256) throw std::runtime_error("A symbol needs 1-256 pins");
  static const std::set<std::string> types = {"input", "output", "bidirectional", "tri_state", "passive",
                                              "free", "unspecified", "power_in", "power_out",
                                              "open_collector", "open_emitter", "no_connect"};
  std::set<std::string> numbers;
  std::vector<const SymbolPin *> left, right;
  for (const auto &pin : spec.pins) {
    if (pin.number.empty() || pin.number.size() > 16 || pin.number.find_first_of(" \t\"") != std::string::npos)
      throw std::runtime_error("Pin numbers are 1-16 characters without spaces");
    if (!numbers.insert(pin.number).second) throw std::runtime_error("Pin " + pin.number + " is listed twice");
    if (!types.count(pin.type)) throw std::runtime_error("Unknown pin type " + pin.type);
    if (pin.name.size() > 32) throw std::runtime_error("Pin names are at most 32 characters");
    (pin.right ? right : left).push_back(&pin);
  }
  // Pin ends on the 2.54 mm grid; the body is wide enough for both sides' names.
  const size_t rows = std::max<size_t>({left.size(), right.size(), 1});
  const double top = 2.54 * static_cast<double>(rows / 2), bottom = top - 2.54 * static_cast<double>(rows - 1);
  const auto longest = [](const std::vector<const SymbolPin *> &side) {
    size_t n = 0;
    for (const auto *pin : side) n = std::max(n, pin->name.size());
    return static_cast<double>(n);
  };
  const double inner = (longest(left) + longest(right)) * 0.9 + 2.54 + 2.032;
  const double w = 2.54 * std::max(2.0, std::ceil(inner / 2 / 2.54));
  const std::string font = "(effects (font (size 1.27 1.27))";
  const std::string value = spec.value.empty() ? spec.name : spec.value;
  std::string text =
      "  (symbol " + quoted(spec.name) + "\n"
      "    (pin_names (offset 1.016))\n"
      "    (exclude_from_sim no) (in_bom yes) (on_board yes) (in_pos_files yes)\n"
      "    (duplicate_pin_numbers_are_jumpers no)\n"
      "    (property \"Reference\" " + quoted(spec.reference) + " (at " + number(-w) + " " + number(top + 3.81) +
      " 0) " + font + " (justify left)))\n"
      "    (property \"Value\" " + quoted(value) + " (at " + number(w) + " " + number(top + 3.81) + " 0) " + font +
      " (justify right)))\n"
      "    (property \"Footprint\" " + quoted(spec.footprint) + " (at 0 0 0) (hide yes) " + font + "))\n"
      "    (property \"Datasheet\" \"\" (at 0 0 0) (hide yes) " + font + "))\n"
      "    (property \"Description\" " + quoted(spec.description) + " (at 0 0 0) (hide yes) " + font + "))\n"
      "    (symbol " + quoted(spec.name + "_0_1") + "\n"
      "      (rectangle (start " + number(-w) + " " + number(top + 2.54) + ") (end " + number(w) + " " +
      number(bottom - 2.54) + ") (stroke (width 0.254) (type default)) (fill (type background))))\n"
      "    (symbol " + quoted(spec.name + "_1_1") + "\n";
  for (const auto *side : {&left, &right})
    for (size_t i = 0; i < side->size(); ++i) {
      const SymbolPin &pin = *(*side)[i];
      const bool on_right = side == &right;
      text += "      (pin " + pin.type + " line (at " + number(on_right ? w + 2.54 : -w - 2.54) + " " +
              number(top - 2.54 * static_cast<double>(i)) + " " + (on_right ? "180" : "0") + ") (length 2.54)\n"
              "        (name " + quoted(pin.name.empty() ? "~" : pin.name) + " " + font + "))\n"
              "        (number " + quoted(pin.number) + " " + font + ")))\n";
    }
  return text + "    )\n    (embedded_fonts no)\n  )";
}

std::string library_footprint_text(const FootprintSpec &spec) {
  check_name(spec.name, "footprint");
  if (spec.pads < 1 || spec.pads > 256) throw std::runtime_error("A footprint needs 1-256 pads");
  if (spec.rows != 1 && spec.rows != 2) throw std::runtime_error("Use one or two rows of pads");
  if (spec.rows == 2 && spec.pads % 2) throw std::runtime_error("Two rows need an even pad count");
  for (double size : {spec.pitch, spec.pad_width, spec.pad_height})
    if (!std::isfinite(size) || size < 0.05 || size > 100) throw std::runtime_error("Use sizes of 0.05-100 mm");
  const int per_row = spec.pads / spec.rows;
  // Pads along x in one row; down the left and up the right in two.
  if (per_row > 1 && spec.pitch < (spec.rows == 1 ? spec.pad_width : spec.pad_height) + 0.05)
    throw std::runtime_error("The pads overlap: increase the pitch or shrink the pads");
  if (spec.rows == 2 && (!std::isfinite(spec.span) || spec.span < spec.pad_width + 0.05 || spec.span > 100))
    throw std::runtime_error("The rows overlap: increase the row spacing");
  if (!spec.smd && (!std::isfinite(spec.drill) || spec.drill < 0.1 ||
                    spec.drill > std::min(spec.pad_width, spec.pad_height) - 0.1))
    throw std::runtime_error("The drill must be at least 0.1 mm and 0.1 mm smaller than the pad");
  std::vector<Point> at;
  for (int i = 0; i < spec.pads; ++i) {
    if (spec.rows == 1) {
      at.push_back({(i - (spec.pads - 1) / 2.0) * spec.pitch, 0});
    } else {
      const int index = i < per_row ? i : spec.pads - 1 - i;
      at.push_back({(i < per_row ? -0.5 : 0.5) * spec.span, (index - (per_row - 1) / 2.0) * spec.pitch});
    }
  }
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (size_t i = 0; i < at.size(); ++i) {
    x0 = std::min(i ? x0 : at[i].x, at[i].x - spec.pad_width / 2); x1 = std::max(i ? x1 : at[i].x, at[i].x + spec.pad_width / 2);
    y0 = std::min(i ? y0 : at[i].y, at[i].y - spec.pad_height / 2); y1 = std::max(i ? y1 : at[i].y, at[i].y + spec.pad_height / 2);
  }
  const auto round_out = [](double value, bool up) { return (up ? std::ceil(value * 100) : std::floor(value * 100)) / 100; };
  const auto rect = [&](double margin, const char *layer, const char *width) {
    return "  (fp_rect (start " + number(round_out(x0 - margin, false)) + " " + number(round_out(y0 - margin, false)) +
           ") (end " + number(round_out(x1 + margin, true)) + " " + number(round_out(y1 + margin, true)) +
           ") (stroke (width " + width + ") (type solid)) (fill no) (layer \"" + layer + "\"))\n";
  };
  const std::string font = "(effects (font (size 1 1) (thickness 0.15)))";
  std::string text =
      "(footprint " + quoted(spec.name) + "\n"
      "  (version 20260206)\n  (generator \"corvcad\")\n  (generator_version \"10.0\")\n  (layer \"F.Cu\")\n"
      "  (descr " + quoted(spec.description) + ")\n"
      "  (property \"Reference\" \"REF**\" (at 0 " + number(round_out(y0 - 1.4, false)) + " 0) (layer \"F.SilkS\") " +
      font + ")\n"
      "  (property \"Value\" " + quoted(spec.name) + " (at 0 " + number(round_out(y1 + 1.4, true)) +
      " 0) (layer \"F.Fab\") " + font + ")\n"
      "  (attr " + (spec.smd ? "smd" : "through_hole") + ")\n" +
      rect(0.2, "F.SilkS", "0.12") + rect(0, "F.Fab", "0.1") + rect(0.5, "F.CrtYd", "0.05");
  for (int i = 0; i < spec.pads; ++i) {
    const std::string where = "(at " + number(at[i].x) + " " + number(at[i].y) + ") (size " + number(spec.pad_width) +
                              " " + number(spec.pad_height) + ")";
    if (spec.smd)
      text += "  (pad \"" + std::to_string(i + 1) + "\" smd roundrect " + where +
              " (layers \"F.Cu\" \"F.Mask\" \"F.Paste\") (roundrect_rratio 0.25))\n";
    else
      text += "  (pad \"" + std::to_string(i + 1) + "\" thru_hole " +
              (i == 0 ? "rect" : spec.pad_width == spec.pad_height ? "circle" : "oval") + " " + where + " (drill " +
              number(spec.drill) + ") (layers \"*.Cu\" \"*.Mask\"))\n";
  }
  return text + "  (embedded_fonts no)\n)\n";
}

std::string project_libraries_json(const std::string &folder_text) {
  const fs::path folder = utf8_path(folder_text);
  if (!fs::is_directory(folder)) throw std::runtime_error("Project folder is missing");
  std::string out = "{";
  for (bool footprint : {false, true}) {
    out += footprint ? ",\"footprints\":[" : "\"symbols\":[";
    const fs::path table = folder / table_file(footprint);
    if (fs::exists(table)) {
      bool first = true;
      for (const auto &row : rows(read_file(table))) {
        out += (first ? "" : ",") + library_json(folder, footprint, row);
        first = false;
      }
    }
    out += "]";
  }
  return out + "}";
}

void import_project_library(const std::string &folder_text, const std::string &source_text) {
  const fs::path folder = utf8_path(folder_text), source = utf8_path(source_text);
  const bool footprint = fs::is_directory(source);
  if (footprint ? source.extension() != ".pretty" : source.extension() != ".kicad_sym")
    throw std::runtime_error("Import a .kicad_sym file or a .pretty folder");
  const std::string name = utf8_string(source.stem());
  check_name(name, "library");
  const fs::path target = folder / source.filename();
  if (fs::exists(target)) throw std::runtime_error(utf8_string(source.filename()) + " is already in the project");
  for (const auto &row : rows(table_text(folder, footprint)))
    if (row.name == name) throw std::runtime_error("The project already has a library named " + name);
  // Read before copying: a library KiCad could not open is refused.
  if (footprint) {
    for (const auto &entry : fs::directory_iterator(source))
      if (entry.path().extension() == ".kicad_mod") footprint_item(read_file(entry.path()), utf8_string(entry.path().filename()));
    fs::copy(source, target, fs::copy_options::recursive);
  } else {
    if (head(document(read_file(source))) != "kicad_symbol_lib")
      throw std::runtime_error(utf8_string(source.filename()) + " is not a KiCad symbol library");
    fs::copy_file(source, target);
  }
  try {
    add_row(folder, footprint, name, utf8_string(source.filename()));
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(target, ignored);
    throw;
  }
}

void create_project_library(const std::string &folder_text, bool footprint, const std::string &name) {
  const fs::path folder = utf8_path(folder_text);
  check_name(name, "library");
  const std::string file = name + (footprint ? ".pretty" : ".kicad_sym");
  const fs::path target = folder / utf8_path(file);
  if (fs::exists(target)) throw std::runtime_error(file + " is already in the project");
  for (const auto &row : rows(table_text(folder, footprint)))
    if (row.name == name) throw std::runtime_error("The project already has a library named " + name);
  if (footprint) fs::create_directory(target);
  else write_file(target, "(kicad_symbol_lib\n  (version 20251024)\n  (generator \"corvcad\")\n  (generator_version \"10.0\")\n)\n");
  add_row(folder, footprint, name, file);
}

void remove_project_library(const std::string &folder_text, bool footprint, const std::string &name) {
  const fs::path folder = utf8_path(folder_text);
  if (name == "CorvCAD") throw std::runtime_error("CorvCAD's starter library stays in the project");
  const std::string table = table_text(folder, footprint);
  for (const auto &row : rows(table))
    if (row.name == name) {
      write_file(folder / table_file(footprint), erase(table, row.begin, row.end));
      if (const fs::path path = local_path(folder, row.uri); !path.empty()) fs::remove_all(path);
      return;
    }
  throw std::runtime_error("No project library named " + name);
}

namespace {
// Source edits applied back to front, as the schematic and board edits do.
struct Patch { size_t begin, end; std::string text; };
std::string apply(std::string text, std::vector<Patch> patches) {
  std::sort(patches.begin(), patches.end(), [](const Patch &a, const Patch &b) { return a.begin > b.begin; });
  for (const auto &p : patches) text.replace(p.begin, p.end - p.begin, p.text);
  return text;
}
// A node's removal with its indentation and line break.
Patch removal(const std::string &text, const Node &n) {
  size_t begin = n.begin, end = n.end;
  while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
  if (end < text.size() && text[end] == '\r') ++end;
  if (end < text.size() && text[end] == '\n') ++end;
  return {begin, end, ""};
}
std::string source_of(const std::string &text, const Node &n) { return text.substr(n.begin, n.end - n.begin); }
// Inserted before a container's (embedded_fonts) or closing parenthesis.
Patch insertion(const Node &container, const std::string &item) {
  const Node *fonts = child(container, "embedded_fonts");
  const size_t at = fonts ? fonts->begin : container.end - 1;
  return {at, at, item + "\n  "};
}
double arg(const std::vector<std::string> &f, size_t i) {
  if (i >= f.size()) throw std::runtime_error("Missing value in the edit");
  return decimal(f[i]);
}
std::string text_arg(const std::vector<std::string> &f, size_t i) {
  if (i >= f.size()) throw std::runtime_error("Missing value in the edit");
  return f[i];
}
size_t index_arg(const std::vector<std::string> &f, size_t i, size_t count) {
  const double v = arg(f, i);
  if (v < 0 || v >= static_cast<double>(count) || v != std::floor(v)) throw std::runtime_error("No such item");
  return static_cast<size_t>(v);
}
void check_size(double v, double low, const char *what) {
  if (!std::isfinite(v) || v < low || v > 100)
    throw std::runtime_error(std::string("Use a ") + what + " of " + number(low) + "-100 mm");
}

// Symbol units: Name_<unit>_<style>; styles 0 and 1 are edited, other body styles stay as they are.
bool unit_of(const Node &sub, int &unit, int &style) {
  const std::string name = val(&sub);
  const size_t b = name.rfind('_');
  const size_t a = b == std::string::npos || b == 0 ? std::string::npos : name.rfind('_', b - 1);
  if (a == std::string::npos) return false;
  try { unit = std::stoi(name.substr(a + 1, b - a - 1)); style = std::stoi(name.substr(b + 1)); }
  catch (...) { return false; }
  return true;
}
const std::set<std::string> symbol_shape_kinds = {"rectangle", "circle", "polyline", "arc", "bezier", "text", "text_box"};
const std::set<std::string> footprint_shape_kinds = {"fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly", "fp_curve",
                                                      "fp_text", "fp_text_box"};
struct Indexed { const Node *node; int unit; };
std::vector<Indexed> symbol_items(const Node &symbol, bool pins) {
  std::vector<Indexed> items;
  for (const auto &sub : symbol.children) {
    int unit = 0, style = 0;
    if (head(sub) != "symbol" || !unit_of(sub, unit, style) || style > 1) continue;
    for (const auto &item : sub.children)
      if (pins ? head(item) == "pin" : symbol_shape_kinds.count(head(item)) > 0) items.push_back({&item, unit});
  }
  return items;
}
std::vector<const Node *> footprint_items(const Node &root, bool pads) {
  std::vector<const Node *> items;
  for (const auto &item : root.children)
    if (pads ? head(item) == "pad" : footprint_shape_kinds.count(head(item)) > 0) items.push_back(&item);
  return items;
}
int unit_count(const Node &symbol) {
  int units = 1;
  for (const auto &sub : symbol.children) {
    int unit = 0, style = 0;
    if (head(sub) == "symbol" && unit_of(sub, unit, style)) units = std::max(units, unit);
  }
  return units;
}
int fill_of(const Node &item) {
  const Node *fill = child(item, "fill");
  const std::string type = fill ? val(child(*fill, "type")) : "";
  return type == "outline" || type == "color" ? 1 : type == "background" ? 2 : 0;
}
Point xy(const Node *n) { return {decimal(val(n)), decimal(val(n, 2))}; }

// A shape's outline for drawing, in its own coordinates: corners, sampled circles and arcs,
// polyline points, a text's anchor.
std::vector<Point> outline_of(const Node &item, bool &closed) {
  const std::string kind = head(item);
  std::vector<Point> points;
  closed = false;
  const double pi = 3.14159265358979323846;
  const auto circle = [&](Point c, double r, double from, double sweep, int steps) {
    for (int i = 0; i <= steps; ++i)
      points.push_back({c.x + r * std::cos(from + sweep * i / steps), c.y + r * std::sin(from + sweep * i / steps)});
  };
  if (kind == "rectangle" || kind == "fp_rect") {
    const Point a = xy(child(item, "start")), b = xy(child(item, "end"));
    points = {a, {b.x, a.y}, b, {a.x, b.y}};
    closed = true;
  } else if (kind == "circle" || kind == "fp_circle") {
    const Point c = xy(child(item, "center"));
    double r = 0;
    if (child(item, "radius")) r = decimal(val(child(item, "radius")));
    else { const Point e = xy(child(item, "end")); r = std::hypot(e.x - c.x, e.y - c.y); }
    circle(c, r, 0, 2 * pi, 32);
    closed = true;
  } else if ((kind == "arc" || kind == "fp_arc") && child(item, "mid")) {
    const Point a = xy(child(item, "start")), b = xy(child(item, "mid")), c = xy(child(item, "end"));
    const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) < 1e-12) {
      points = {a, b, c};
    } else {
      const Point u{((a.x * a.x + a.y * a.y) * (b.y - c.y) + (b.x * b.x + b.y * b.y) * (c.y - a.y) +
                     (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d,
                    ((a.x * a.x + a.y * a.y) * (c.x - b.x) + (b.x * b.x + b.y * b.y) * (a.x - c.x) +
                     (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d};
      const double start = std::atan2(a.y - u.y, a.x - u.x);
      const auto turn = [&](Point p) {
        return std::fmod(std::fmod(std::atan2(p.y - u.y, p.x - u.x) - start, 2 * pi) + 2 * pi, 2 * pi);
      };
      const double sweep = turn(b) <= turn(c) ? turn(c) : turn(c) - 2 * pi;
      circle(u, std::hypot(a.x - u.x, a.y - u.y), start, sweep, 16);
    }
  } else if (kind == "fp_line") {
    points = {xy(child(item, "start")), xy(child(item, "end"))};
  } else if (const Node *pts = child(item, "pts")) {
    for (const auto &p : pts->children) if (head(p) == "xy") points.push_back(xy(&p));
    closed = kind == "fp_poly";
  } else if (const Node *at = child(item, "at")) {
    points = {xy(at)};
  }
  return points;
}
// Moves every coordinate of a shape by (dx, dy).
void translate(const Node &item, double dx, double dy, std::vector<Patch> &patches) {
  for (const auto &part : item.children) {
    const std::string kind = head(part);
    if ((kind == "start" || kind == "end" || kind == "center" || kind == "mid" || kind == "at" || kind == "xy") &&
        part.children.size() > 2) {
      patches.push_back({part.children[1].begin, part.children[1].end, number(decimal(part.children[1].atom) + dx)});
      patches.push_back({part.children[2].begin, part.children[2].end, number(decimal(part.children[2].atom) + dy)});
    } else if (kind == "pts") {
      translate(part, dx, dy, patches);
    }
  }
}
std::string fill_text(int fill) {
  return std::string("(fill (type ") + (fill == 1 ? "outline" : fill == 2 ? "background" : "none") + "))";
}
const std::set<std::string> pin_types = {"input", "output", "bidirectional", "tri_state", "passive", "free", "unspecified",
                                         "power_in", "power_out", "open_collector", "open_emitter", "no_connect"};
void check_pin_number(const std::string &number_text) {
  if (number_text.empty() || number_text.size() > 16 || number_text.find_first_of(" \t\"") != std::string::npos)
    throw std::runtime_error("Pin numbers are 1-16 characters without spaces");
}

std::string symbol_edit(const std::string &text, const std::vector<std::string> &f) {
  const Node root = document(text);
  if (head(root) != "symbol" || root.children.size() < 2) throw std::runtime_error("Not a library symbol");
  const std::string name = val(&root);
  const bool derived = child(root, "extends") != nullptr;
  const std::string op = f[0], what = f.size() > 1 ? f[1] : "";
  std::vector<Patch> patches;
  if (op == "field") {
    const std::string key = text_arg(f, 1), value = f.size() > 2 ? f[2] : "";
    if (key == "Reference" && (value.empty() || value.size() > 8)) throw std::runtime_error("Use a reference prefix of 1-8 characters");
    for (const auto &property : root.children)
      if (head(property) == "property" && val(&property) == key && property.children.size() > 2)
        return apply(text, {{property.children[2].begin, property.children[2].end, quoted(value)}});
    const Node *after = nullptr;
    for (const auto &property : root.children) if (head(property) == "property") after = &property;
    const size_t at = after ? after->end : root.children[1].end;
    return apply(text, {{at, at, "\n    (property " + quoted(key) + " " + quoted(value) +
                                     " (at 0 0 0) (hide yes) (effects (font (size 1.27 1.27))))"}});
  }
  if (op == "name") {
    const std::string renamed = text_arg(f, 1);
    check_name(renamed, "symbol");
    patches.push_back({root.children[1].begin, root.children[1].end, quoted(renamed)});
    for (const auto &sub : root.children)
      if (head(sub) == "symbol" && val(&sub).rfind(name + "_", 0) == 0)
        patches.push_back({sub.children[1].begin, sub.children[1].end, quoted(renamed + val(&sub).substr(name.size()))});
    return apply(text, patches);
  }
  if (derived) throw std::runtime_error("A derived symbol draws its base symbol: edit the base for pins and shapes");
  if (op == "units") {
    const int units = static_cast<int>(arg(f, 1));
    if (units < 1 || units > 26) throw std::runtime_error("Use 1-26 units");
    std::set<int> present;
    for (const auto &sub : root.children) {
      int unit = 0, style = 0;
      if (head(sub) != "symbol" || !unit_of(sub, unit, style)) continue;
      if (unit > units) patches.push_back(removal(text, sub));
      else present.insert(unit);
    }
    for (int unit = 1; unit <= units; ++unit)
      if (!present.count(unit))
        patches.push_back(insertion(root, "(symbol " + quoted(name + "_" + std::to_string(unit) + "_1") + ")"));
    return apply(text, patches);
  }
  const auto pins = symbol_items(root, true), shapes = symbol_items(root, false);
  // Into the unit's body-style-1 sub-symbol (unit 0: shared by every unit), made when missing.
  const auto add_to_unit = [&](int unit, const std::string &item) {
    if (unit < 0 || unit > unit_count(root)) throw std::runtime_error("No such unit");
    for (const auto &sub : root.children) {
      int u = 0, style = 0;
      if (head(sub) == "symbol" && unit_of(sub, u, style) && u == unit && style == 1)
        return apply(text, {{sub.end - 1, sub.end - 1, "\n      " + item + "\n    "}});
    }
    return apply(text, {insertion(root, "(symbol " + quoted(name + "_" + std::to_string(unit) + "_1") + "\n      " + item + ")")});
  };
  if (op == "pin" && what == "add") {
    const std::string number_text = text_arg(f, 3), pin_name = text_arg(f, 4), type = text_arg(f, 5);
    if (!pin_types.count(type)) throw std::runtime_error("Unknown pin type " + type);
    check_pin_number(number_text);
    for (const auto &pin : pins)
      if (val(child(*pin.node, "number")) == number_text) throw std::runtime_error("Pin " + number_text + " is already there");
    const std::string font = "(effects (font (size 1.27 1.27))))";
    return add_to_unit(static_cast<int>(arg(f, 2)),
                       "(pin " + type + " line (at " + number(arg(f, 6)) + " " + number(arg(f, 7)) + " " + number(arg(f, 8)) +
                           ") (length " + number(arg(f, 9)) + ") (name " + quoted(pin_name.empty() ? "~" : pin_name) + " " +
                           font + " (number " + quoted(number_text) + " " + font + ")");
  }
  if (op == "pin") {
    const Node &pin = *pins.at(index_arg(f, 2, pins.size())).node;
    if (what == "delete") return apply(text, {removal(text, pin)});
    if (what == "move") {
      const Node *at = child(pin, "at");
      if (!at || at->children.size() < 3) throw std::runtime_error("Pin without a position");
      patches.push_back({at->children[1].begin, at->children[1].end, number(arg(f, 3))});
      patches.push_back({at->children[2].begin, at->children[2].end, number(arg(f, 4))});
      if (at->children.size() > 3) patches.push_back({at->children[3].begin, at->children[3].end, number(arg(f, 5))});
      else patches.push_back({at->children[2].end, at->children[2].end, " " + number(arg(f, 5))});
      return apply(text, patches);
    }
    if (what == "set") {
      const std::string number_text = text_arg(f, 3), pin_name = text_arg(f, 4), type = text_arg(f, 5);
      if (!pin_types.count(type)) throw std::runtime_error("Unknown pin type " + type);
      check_pin_number(number_text);
      for (const auto &other : pins)
        if (other.node != &pin && val(child(*other.node, "number")) == number_text)
          throw std::runtime_error("Pin " + number_text + " is already there");
      patches.push_back({pin.children[1].begin, pin.children[1].end, type});
      if (const Node *n = child(pin, "number"); n && n->children.size() > 1)
        patches.push_back({n->children[1].begin, n->children[1].end, quoted(number_text)});
      if (const Node *n = child(pin, "name"); n && n->children.size() > 1)
        patches.push_back({n->children[1].begin, n->children[1].end, quoted(pin_name.empty() ? "~" : pin_name)});
      if (const Node *n = child(pin, "length"); n && n->children.size() > 1 && f.size() > 6)
        patches.push_back({n->children[1].begin, n->children[1].end, number(arg(f, 6))});
      return apply(text, patches);
    }
  }
  if (op == "shape" && what == "add") {
    const int unit = static_cast<int>(arg(f, 2));
    const std::string kind = text_arg(f, 3);
    const int fill = static_cast<int>(arg(f, 4));
    const std::string style = " (stroke (width 0.254) (type default)) " + fill_text(fill) + ")";
    if (kind == "rect")
      return add_to_unit(unit, "(rectangle (start " + number(arg(f, 5)) + " " + number(arg(f, 6)) + ") (end " +
                                   number(arg(f, 7)) + " " + number(arg(f, 8)) + ")" + style);
    if (kind == "circle") {
      check_size(arg(f, 7), 0.1, "radius");
      return add_to_unit(unit, "(circle (center " + number(arg(f, 5)) + " " + number(arg(f, 6)) + ") (radius " +
                                   number(arg(f, 7)) + ")" + style);
    }
    if (kind == "poly") {
      if (f.size() < 9 || (f.size() - 5) % 2) throw std::runtime_error("A line needs at least two points");
      std::string pts = "(pts";
      for (size_t i = 5; i + 1 < f.size(); i += 2) pts += " (xy " + number(arg(f, i)) + " " + number(arg(f, i + 1)) + ")";
      return add_to_unit(unit, "(polyline " + pts + ")" + style);
    }
    if (kind == "arc")
      return add_to_unit(unit, "(arc (start " + number(arg(f, 5)) + " " + number(arg(f, 6)) + ") (mid " + number(arg(f, 7)) +
                                   " " + number(arg(f, 8)) + ") (end " + number(arg(f, 9)) + " " + number(arg(f, 10)) + ")" + style);
    throw std::runtime_error("Unknown shape " + kind);
  }
  if (op == "shape") {
    const Node &shape = *shapes.at(index_arg(f, 2, shapes.size())).node;
    if (what == "delete") return apply(text, {removal(text, shape)});
    if (what == "move") { translate(shape, arg(f, 3), arg(f, 4), patches); return apply(text, patches); }
    if (what == "fill") {
      const std::string replacement = fill_text(static_cast<int>(arg(f, 3)));
      if (const Node *fill = child(shape, "fill")) return apply(text, {{fill->begin, fill->end, replacement}});
      return apply(text, {{shape.end - 1, shape.end - 1, " " + replacement}});
    }
  }
  throw std::runtime_error("Unknown symbol edit " + op + " " + what);
}

const std::set<std::string> drawing_layers = {"F.SilkS", "B.SilkS", "F.Fab", "B.Fab", "F.CrtYd", "B.CrtYd"};
std::string pad_text(const std::string &number_text, const std::string &type, const std::string &shape,
                     const std::string &at, double w, double h, double drill, const std::string &extras) {
  if (type != "smd" && type != "thru_hole" && type != "np_thru_hole") throw std::runtime_error("Unknown pad type " + type);
  if (shape != "rect" && shape != "roundrect" && shape != "circle" && shape != "oval")
    throw std::runtime_error("Unknown pad shape " + shape);
  if (number_text.size() > 16 || number_text.find_first_of(" \t\"") != std::string::npos)
    throw std::runtime_error("Pad numbers are at most 16 characters without spaces");
  check_size(w, 0.05, "pad size");
  check_size(h, 0.05, "pad size");
  const bool drilled = type != "smd";
  if (drilled && (!std::isfinite(drill) || drill < 0.1 || drill > std::min(w, h)))
    throw std::runtime_error("The drill must be 0.1 mm up to the pad's size");
  return "(pad " + quoted(type == "np_thru_hole" ? "" : number_text) + " " + type + " " + shape + " " + at + " (size " +
         number(w) + " " + number(h) + ")" + (drilled ? " (drill " + number(drill) + ")" : "") +
         (type == "smd" ? " (layers \"F.Cu\" \"F.Mask\" \"F.Paste\")" : " (layers \"*.Cu\" \"*.Mask\")") +
         (shape == "roundrect" ? " (roundrect_rratio 0.25)" : "") + extras + ")";
}

std::string footprint_edit(const std::string &text, const std::vector<std::string> &f) {
  const Node root = document(text);
  if (head(root) != "footprint" || root.children.size() < 2) throw std::runtime_error("Not a footprint");
  const std::string op = f[0], what = f.size() > 1 ? f[1] : "";
  std::vector<Patch> patches;
  if (op == "name") {
    const std::string renamed = text_arg(f, 1);
    check_name(renamed, "footprint");
    patches.push_back({root.children[1].begin, root.children[1].end, quoted(renamed)});
    for (const auto &property : root.children)
      if (head(property) == "property" && val(&property) == "Value" && property.children.size() > 2 &&
          val(&property, 2) == val(&root))
        patches.push_back({property.children[2].begin, property.children[2].end, quoted(renamed)});
    return apply(text, patches);
  }
  if (op == "field" && what == "descr") {
    const std::string value = f.size() > 2 ? f[2] : "";
    if (const Node *descr = child(root, "descr"); descr && descr->children.size() > 1)
      return apply(text, {{descr->children[1].begin, descr->children[1].end, quoted(value)}});
    return apply(text, {{root.children[1].end, root.children[1].end, "\n  (descr " + quoted(value) + ")"}});
  }
  if (op == "attr") {
    const std::string kind = text_arg(f, 1);
    if (kind != "smd" && kind != "through_hole") throw std::runtime_error("Use smd or through_hole");
    if (const Node *attr = child(root, "attr"); attr && attr->children.size() > 1)
      return apply(text, {{attr->children[1].begin, attr->children[1].end, kind}});
    return apply(text, {insertion(root, "(attr " + kind + ")")});
  }
  const auto pads = footprint_items(root, true), shapes = footprint_items(root, false);
  if (op == "pad" && what == "add")
    return apply(text, {insertion(root, pad_text(text_arg(f, 2), text_arg(f, 3), text_arg(f, 4),
                                                 "(at " + number(arg(f, 5)) + " " + number(arg(f, 6)) + ")", arg(f, 7),
                                                 arg(f, 8), f.size() > 9 ? arg(f, 9) : 0, ""))});
  if (op == "pad") {
    const Node &pad = *pads.at(index_arg(f, 2, pads.size()));
    if (what == "delete") return apply(text, {removal(text, pad)});
    const Node *at = child(pad, "at");
    if (!at || at->children.size() < 3) throw std::runtime_error("Pad without a position");
    if (what == "move") {
      patches.push_back({at->children[1].begin, at->children[1].end, number(arg(f, 3))});
      patches.push_back({at->children[2].begin, at->children[2].end, number(arg(f, 4))});
      return apply(text, patches);
    }
    if (what == "set") {
      // Rewritten with its position and every other attribute (mask margins, chamfers, nets) kept.
      std::string extras;
      for (size_t i = 4; i < pad.children.size(); ++i) {
        const std::string kind = head(pad.children[i]);
        if (kind != "at" && kind != "size" && kind != "drill" && kind != "layers" && kind != "roundrect_rratio")
          extras += " " + source_of(text, pad.children[i]);
      }
      return apply(text, {{pad.begin, pad.end, pad_text(text_arg(f, 3), text_arg(f, 4), text_arg(f, 5), source_of(text, *at),
                                                        arg(f, 6), arg(f, 7), f.size() > 8 ? arg(f, 8) : 0, extras)}});
    }
  }
  if (op == "shape" && what == "add") {
    const std::string kind = text_arg(f, 2), layer = text_arg(f, 3);
    if (!drawing_layers.count(layer)) throw std::runtime_error("Draw on the silkscreen, fab or courtyard layers");
    const double width = arg(f, kind == "circle" ? 7 : 8);
    check_size(width, 0.01, "line width");
    const std::string style = " (stroke (width " + number(width) + ") (type solid))";
    const std::string tail = " (layer " + quoted(layer) + "))";
    if (kind == "line")
      return apply(text, {insertion(root, "(fp_line (start " + number(arg(f, 4)) + " " + number(arg(f, 5)) + ") (end " +
                                              number(arg(f, 6)) + " " + number(arg(f, 7)) + ")" + style + tail)});
    if (kind == "rect")
      return apply(text, {insertion(root, "(fp_rect (start " + number(arg(f, 4)) + " " + number(arg(f, 5)) + ") (end " +
                                              number(arg(f, 6)) + " " + number(arg(f, 7)) + ")" + style + " (fill no)" + tail)});
    if (kind == "circle") {
      check_size(arg(f, 6), 0.05, "radius");
      return apply(text, {insertion(root, "(fp_circle (center " + number(arg(f, 4)) + " " + number(arg(f, 5)) + ") (end " +
                                              number(arg(f, 4) + arg(f, 6)) + " " + number(arg(f, 5)) + ")" + style +
                                              " (fill no)" + tail)});
    }
    throw std::runtime_error("Unknown shape " + kind);
  }
  if (op == "shape") {
    const Node &shape = *shapes.at(index_arg(f, 2, shapes.size()));
    if (what == "delete") return apply(text, {removal(text, shape)});
    if (what == "move") { translate(shape, arg(f, 3), arg(f, 4), patches); return apply(text, patches); }
  }
  throw std::runtime_error("Unknown footprint edit " + op + " " + what);
}

std::string symbol_editor_json(const std::string &text, const std::string &base_text) {
  const Node root = document(text);
  if (head(root) != "symbol") throw std::runtime_error("Not a library symbol");
  const bool derived = child(root, "extends") != nullptr;
  const Node base = derived && !base_text.empty() ? document(base_text) : Node{};
  const Node &geometry = derived && !base_text.empty() ? base : root;
  std::map<std::string, std::string> properties;
  for (const Node *from : {&geometry, &root})
    for (const auto &item : from->children)
      if (head(item) == "property") properties[val(&item)] = val(&item, 2);
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "{\"name\":" << json(val(&root)) << ",\"derived\":" << (derived ? "true" : "false") << ",\"base\":"
      << json(val(child(root, "extends"))) << ",\"units\":" << unit_count(geometry) << ",\"fields\":{";
  bool first = true;
  for (const auto &[key, value] : properties) { out << (first ? "" : ",") << json(key) << ":" << json(value); first = false; }
  out << "},\"shapes\":[";
  const auto shapes = symbol_items(geometry, false);
  for (size_t i = 0; i < shapes.size(); ++i) {
    const Node &shape = *shapes[i].node;
    bool closed = false;
    const auto points = outline_of(shape, closed);
    out << (i ? "," : "") << "{\"i\":" << i << ",\"unit\":" << shapes[i].unit << ",\"kind\":" << json(head(shape))
        << ",\"fill\":" << fill_of(shape) << ",\"closed\":" << (closed ? "true" : "false") << ",\"text\":"
        << json(head(shape) == "text" ? val(&shape) : "") << ",\"points\":[";
    for (size_t k = 0; k < points.size(); ++k) out << (k ? "," : "") << number(points[k].x) << "," << number(points[k].y);
    out << "]}";
  }
  out << "],\"pins\":[";
  const auto pins = symbol_items(geometry, true);
  for (size_t i = 0; i < pins.size(); ++i) {
    const Node &pin = *pins[i].node;
    const Node *at = child(pin, "at");
    bool hidden = false;
    for (const auto &part : pin.children) hidden |= part.atom == "hide" || (head(part) == "hide" && val(&part) != "no");
    out << (i ? "," : "") << "{\"i\":" << i << ",\"unit\":" << pins[i].unit << ",\"number\":" << json(val(child(pin, "number")))
        << ",\"name\":" << json(val(child(pin, "name")))
        << ",\"type\":" << json(pin.children.size() > 1 ? pin.children[1].atom : "")
        << ",\"x\":" << number(at ? decimal(val(at)) : 0) << ",\"y\":" << number(at ? decimal(val(at, 2)) : 0)
        << ",\"angle\":" << number(at && at->children.size() > 3 ? decimal(val(at, 3)) : 0)
        << ",\"length\":" << number(child(pin, "length") ? decimal(val(child(pin, "length"))) : 0)
        << ",\"hidden\":" << (hidden ? "true" : "false") << "}";
  }
  return out.str() + "]}";
}

std::string footprint_editor_json(const std::string &text) {
  const Node root = document(text);
  if (head(root) != "footprint") throw std::runtime_error("Not a footprint");
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "{\"name\":" << json(val(&root)) << ",\"description\":" << json(val(child(root, "descr")))
      << ",\"attr\":" << json(val(child(root, "attr"))) << ",\"pads\":[";
  const auto pads = footprint_items(root, true);
  for (size_t i = 0; i < pads.size(); ++i) {
    const Node &pad = *pads[i];
    const Node *at = child(pad, "at"), *size = child(pad, "size"), *drill = child(pad, "drill");
    double hole = 0;
    if (drill)
      for (size_t k = 1; k < drill->children.size() && hole == 0; ++k)
        try { hole = decimal(drill->children[k].atom); } catch (const std::exception &) {}
    out << (i ? "," : "") << "{\"i\":" << i << ",\"number\":" << json(val(&pad)) << ",\"type\":" << json(val(&pad, 2))
        << ",\"shape\":" << json(val(&pad, 3)) << ",\"x\":" << number(at ? decimal(val(at)) : 0)
        << ",\"y\":" << number(at ? decimal(val(at, 2)) : 0)
        << ",\"rot\":" << number(at && at->children.size() > 3 ? decimal(val(at, 3)) : 0)
        << ",\"w\":" << number(size ? decimal(val(size)) : 0) << ",\"h\":" << number(size ? decimal(val(size, 2)) : 0)
        << ",\"drill\":" << number(hole) << "}";
  }
  out << "],\"shapes\":[";
  const auto shapes = footprint_items(root, false);
  for (size_t i = 0; i < shapes.size(); ++i) {
    const Node &shape = *shapes[i];
    bool closed = false;
    const auto points = outline_of(shape, closed);
    const Node *stroke = child(shape, "stroke");
    const Node *width = stroke ? child(*stroke, "width") : child(shape, "width");
    out << (i ? "," : "") << "{\"i\":" << i << ",\"kind\":" << json(head(shape))
        << ",\"layer\":" << json(val(child(shape, "layer"))) << ",\"width\":" << number(width ? decimal(val(width)) : 0)
        << ",\"closed\":" << (closed ? "true" : "false")
        << ",\"text\":" << json(head(shape) == "fp_text" ? val(&shape, 2) : "") << ",\"points\":[";
    for (size_t k = 0; k < points.size(); ++k) out << (k ? "," : "") << number(points[k].x) << "," << number(points[k].y);
    out << "]}";
  }
  return out.str() + "]}";
}

// A listed library's path, editable or not (reading only needs it to be in the project folder).
fs::path library_path(const fs::path &folder, bool footprint, const std::string &library) {
  for (const auto &row : rows(table_text(folder, footprint)))
    if (row.name == library) {
      const fs::path path = local_path(folder, row.uri);
      if (path.empty()) throw std::runtime_error("Only libraries inside the project folder can be opened");
      return path;
    }
  throw std::runtime_error("No project library named " + library);
}
} // namespace

std::string library_generate(bool footprint, const std::string &spec_text) {
  if (footprint) return library_footprint_text(parse_footprint_spec(spec_text));
  const std::string text = library_symbol_text(parse_symbol_spec(spec_text));
  return text.substr(text.find('('));
}

std::string library_item_text(const std::string &folder, bool footprint, const std::string &library,
                              const std::string &name) {
  const fs::path path = library_path(utf8_path(folder), footprint, library);
  check_name(name, footprint ? "footprint" : "symbol");
  if (footprint) return read_file(path / utf8_path(name + ".kicad_mod"));
  const std::string text = read_file(path);
  const Node root = document(text);
  for (const auto &node : root.children)
    if (head(node) == "symbol" && val(&node) == name) return source_of(text, node);
  throw std::runtime_error(name + " is not in " + library);
}

std::string library_edit(bool footprint, const std::string &text, const std::string &op) {
  const auto f = fields(op);
  if (f.empty() || f[0].empty()) throw std::runtime_error("Empty edit");
  const std::string edited = footprint ? footprint_edit(text, f) : symbol_edit(text, f);
  // The result must read back before the editor takes it.
  if (footprint) footprint_editor_json(edited);
  else symbol_editor_json(edited, "");
  return edited;
}

std::string library_item_json(bool footprint, const std::string &text, const std::string &base_text) {
  return footprint ? footprint_editor_json(text) : symbol_editor_json(text, base_text);
}

void save_library_item_text(const std::string &folder_text, bool footprint, const std::string &library,
                            const std::string &original, const std::string &item, bool replace) {
  const fs::path path = editable_library(utf8_path(folder_text), footprint, library);
  const Node node = document(item);
  if (head(node) != (footprint ? "footprint" : "symbol") || node.children.size() < 2)
    throw std::runtime_error("Not a library item");
  const std::string name = val(&node);
  check_name(name, footprint ? "footprint" : "symbol");
  if (footprint) {
    footprint_item(item, name);
    const fs::path file = path / utf8_path(name + ".kicad_mod");
    if (fs::exists(file) && !replace && name != original) throw std::runtime_error(name + " is already in " + library);
    fs::create_directories(path);
    write_file(file, item.back() == '\n' ? item : item + "\n");
    if (!original.empty() && original != name) fs::remove(path / utf8_path(original + ".kicad_mod"));
    return;
  }
  std::string text = read_file(path);
  const Node root = document(text);
  if (head(root) != "kicad_symbol_lib") throw std::runtime_error(library + " is not a KiCad symbol library");
  const Node *target = nullptr, *same = nullptr;
  for (const auto &symbol : root.children) {
    if (head(symbol) != "symbol") continue;
    if (!original.empty() && val(&symbol) == original) target = &symbol;
    if (val(&symbol) == name) same = &symbol;
    if (!original.empty() && original != name && val(child(symbol, "extends")) == original)
      throw std::runtime_error(original + " is the base of " + val(&symbol) + "; it keeps its name");
  }
  if (same && same != target && !replace) throw std::runtime_error(name + " is already in " + library);
  std::vector<Patch> patches;
  if (same && same != target) patches.push_back(removal(text, *same));
  if (target) {
    patches.push_back({target->begin, target->end, item});
  } else {
    size_t close = root.end - 1;
    while (close > 0 && (text[close - 1] == ' ' || text[close - 1] == '\t')) --close;
    patches.push_back({close, close, std::string(close > 0 && text[close - 1] != '\n' ? "\n  " : "  ") + item + "\n"});
  }
  text = apply(text, patches);
  document(text);
  write_file(path, text);
}

void save_library_symbol(const std::string &folder, const std::string &library, const SymbolSpec &spec, bool replace) {
  const std::string text = library_symbol_text(spec);
  save_library_item_text(folder, false, library, replace ? spec.name : "", text.substr(text.find('(')), replace);
}

void save_library_footprint(const std::string &folder, const std::string &library, const FootprintSpec &spec,
                            bool replace) {
  save_library_item_text(folder, true, library, replace ? spec.name : "", library_footprint_text(spec), replace);
}

void import_library_footprint(const std::string &folder_text, const std::string &library, const std::string &source_text) {
  const fs::path source = utf8_path(source_text);
  if (source.extension() != ".kicad_mod") throw std::runtime_error("Import a .kicad_mod footprint file");
  const fs::path path = editable_library(utf8_path(folder_text), true, library);
  const std::string name = utf8_string(source.stem());
  check_name(name, "footprint");
  const fs::path target = path / source.filename();
  if (fs::exists(target)) throw std::runtime_error(name + " is already in " + library);
  footprint_item(read_file(source), name);
  fs::create_directories(path);
  fs::copy_file(source, target);
}

void delete_library_item(const std::string &folder_text, bool footprint, const std::string &library,
                         const std::string &name) {
  const fs::path path = editable_library(utf8_path(folder_text), footprint, library);
  check_name(name, footprint ? "footprint" : "symbol");
  if (footprint) {
    if (!fs::remove(path / utf8_path(name + ".kicad_mod"))) throw std::runtime_error(name + " is not in " + library);
    return;
  }
  const std::string text = read_file(path);
  const Node root = document(text);
  const Node *target = nullptr;
  for (const auto &node : root.children) {
    if (head(node) != "symbol") continue;
    if (val(&node) == name) target = &node;
    if (val(child(node, "extends")) == name)
      throw std::runtime_error(name + " is the base of " + val(&node) + "; delete that first");
  }
  if (!target) throw std::runtime_error(name + " is not in " + library);
  write_file(path, erase(text, target->begin, target->end));
}
}


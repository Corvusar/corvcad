#include "corvcad/gerber.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace corvcad {
namespace {
constexpr double pi = 3.14159265358979323846;

double parse_number(const std::string &text) {
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  double value = 0;
  if (!(in >> value)) throw std::runtime_error("Not a number: " + text);
  return value;
}

double signed_area(const std::vector<Point> &p) {
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
  return a / 2;
}
// Dark polygons counter-clockwise, clear ones clockwise: a non-zero fill of an item then adds
// the dark shapes and cuts the clear ones, as an aperture's exposure does.
GerberPolygon oriented(std::vector<Point> points, bool dark) {
  if ((signed_area(points) < 0) == dark) std::reverse(points.begin(), points.end());
  return {std::move(points), dark};
}
std::vector<Point> circle(Point c, double diameter) {
  const double r = diameter / 2;
  const int n = std::clamp(static_cast<int>(std::ceil(2 * pi * r / 0.05)), 16, 64);
  std::vector<Point> points;
  for (int i = 0; i < n; ++i) points.push_back({c.x + r * std::cos(2 * pi * i / n), c.y + r * std::sin(2 * pi * i / n)});
  return points;
}
std::vector<Point> rectangle(Point c, double w, double h) {
  return {{c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2}, {c.x - w / 2, c.y + h / 2}};
}
std::vector<Point> obround(Point c, double w, double h) {
  if (std::abs(w - h) < 1e-9) return circle(c, w);
  const double r = std::min(w, h) / 2;
  std::vector<Point> points;
  // Two half circles joined: along x when wider than tall, else along y.
  const bool horizontal = w > h;
  const double half = (std::max(w, h) - std::min(w, h)) / 2;
  const Point a = horizontal ? Point{c.x + half, c.y} : Point{c.x, c.y + half};
  const Point b = horizontal ? Point{c.x - half, c.y} : Point{c.x, c.y - half};
  const double start = horizontal ? -pi / 2 : 0;
  for (int i = 0; i <= 16; ++i) points.push_back({a.x + r * std::cos(start + pi * i / 16), a.y + r * std::sin(start + pi * i / 16)});
  for (int i = 0; i <= 16; ++i)
    points.push_back({b.x + r * std::cos(start + pi + pi * i / 16), b.y + r * std::sin(start + pi + pi * i / 16)});
  return points;
}
Point rotated(Point p, double degrees) {
  const double a = degrees * pi / 180;
  return {p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a)};
}
std::vector<Point> rotated(std::vector<Point> points, double degrees) {
  if (degrees != 0) for (auto &p : points) p = rotated(p, degrees);
  return points;
}

// Aperture macro arithmetic: numbers, $n, unary minus, x and /, + and -, parentheses.
struct Expression {
  const std::string &s;
  const std::map<int, double> &vars;
  size_t p = 0;
  double value() {
    double v = term();
    while (p < s.size() && (s[p] == '+' || s[p] == '-')) { const char op = s[p++]; const double r = term(); v = op == '+' ? v + r : v - r; }
    return v;
  }
  double term() {
    double v = factor();
    while (p < s.size() && (s[p] == 'x' || s[p] == 'X' || s[p] == '/')) {
      const char op = s[p++];
      const double r = factor();
      v = op == '/' ? (r == 0 ? 0 : v / r) : v * r;
    }
    return v;
  }
  double factor() {
    if (p >= s.size()) throw std::runtime_error("Incomplete macro expression");
    if (s[p] == '-') { ++p; return -factor(); }
    if (s[p] == '+') { ++p; return factor(); }
    if (s[p] == '(') { ++p; const double v = value(); if (p < s.size() && s[p] == ')') ++p; return v; }
    if (s[p] == '$') {
      ++p;
      const size_t start = p;
      while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
      const auto found = vars.find(std::stoi(s.substr(start, p - start)));
      return found == vars.end() ? 0 : found->second;
    }
    const size_t start = p;
    while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) ++p;
    if (start == p) throw std::runtime_error("Bad macro expression: " + s);
    return parse_number(s.substr(start, p - start));
  }
};
double evaluate(const std::string &text, const std::map<int, double> &vars) {
  Expression e{text, vars};
  return e.value();
}
std::vector<std::string> split(const std::string &text, char by) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t at; (at = text.find(by, start)) != std::string::npos; start = at + 1) parts.push_back(text.substr(start, at - start));
  parts.push_back(text.substr(start));
  return parts;
}

struct Aperture {
  std::vector<GerberPolygon> shape; // around the flash point
  double width = 0;                 // drawing width
  bool round = true;
};

struct GerberReader {
  GerberLayer layer;
  int dec_digits = 6, int_digits = 4;
  bool trailing = false, incremental = false;
  double scale = 1; // to millimetres
  Point at{0, 0};
  int aperture = 0, interpolation = 1, last_operation = 2;
  bool multi_quadrant = true, region = false, dark = true;
  std::map<int, Aperture> apertures;
  std::map<std::string, std::vector<std::string>> macros;
  std::vector<std::vector<Point>> contours; // region in progress
  std::vector<Point> contour;
  struct Repeat { int x = 1, y = 1; double dx = 0, dy = 0; size_t first = 0; } repeat;
  bool repeating = false;
  Point offset{0, 0}; // %OF image offset, applied to the finished image
  std::map<std::string, bool> warned;

  void warn(const std::string &message) {
    if (!warned[message]) { warned[message] = true; layer.warnings.push_back(message); }
  }
  double coordinate(std::string digits) {
    if (digits.find('.') != std::string::npos) return parse_number(digits) * scale;
    bool negative = false;
    if (!digits.empty() && (digits[0] == '-' || digits[0] == '+')) { negative = digits[0] == '-'; digits.erase(0, 1); }
    if (trailing) while (static_cast<int>(digits.size()) < int_digits + dec_digits) digits += '0';
    double v = digits.empty() ? 0 : parse_number(digits) / std::pow(10.0, dec_digits);
    return (negative ? -v : v) * scale;
  }
  bool bounded = false;
  void extend(Point p) {
    if (!bounded) { layer.min_x = layer.max_x = p.x; layer.min_y = layer.max_y = p.y; bounded = true; }
    layer.min_x = std::min(layer.min_x, p.x); layer.max_x = std::max(layer.max_x, p.x);
    layer.min_y = std::min(layer.min_y, p.y); layer.max_y = std::max(layer.max_y, p.y);
  }
  void add(GerberItem item) {
    for (const auto &polygon : item.polygons) for (const auto &p : polygon.points) extend(p);
    for (const auto &p : item.path) extend(p);
    layer.items.push_back(std::move(item));
  }

  // Points from the current point to `to` (exclusive of the start) for the interpolation mode.
  std::vector<Point> segment(Point from, Point to, double i, double j) {
    if (interpolation == 1) return {to};
    const bool clockwise = interpolation == 2;
    Point c{from.x + i, from.y + j};
    if (!multi_quadrant) {
      warn("Single-quadrant arcs (G74) read as the shorter arc");
      double best = INFINITY;
      for (double si : {1.0, -1.0})
        for (double sj : {1.0, -1.0}) {
          const Point candidate{from.x + si * std::abs(i), from.y + sj * std::abs(j)};
          const double error = std::abs(std::hypot(from.x - candidate.x, from.y - candidate.y) -
                                        std::hypot(to.x - candidate.x, to.y - candidate.y));
          if (error < best) { best = error; c = candidate; }
        }
    }
    const double r = std::hypot(from.x - c.x, from.y - c.y);
    const double a0 = std::atan2(from.y - c.y, from.x - c.x), a1 = std::atan2(to.y - c.y, to.x - c.x);
    double sweep = a1 - a0;
    if (clockwise) { while (sweep >= 0) sweep -= 2 * pi; } else { while (sweep <= 0) sweep += 2 * pi; }
    if (!multi_quadrant && std::abs(sweep) > pi / 2 + 1e-6) sweep += clockwise ? 2 * pi : -2 * pi;
    const int steps = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (pi / 48))));
    std::vector<Point> points;
    for (int k = 1; k < steps; ++k) points.push_back({c.x + r * std::cos(a0 + sweep * k / steps), c.y + r * std::sin(a0 + sweep * k / steps)});
    points.push_back(to);
    return points;
  }

  void close_contour() {
    if (contour.size() > 1 && std::hypot(contour.front().x - contour.back().x, contour.front().y - contour.back().y) < 1e-9)
      contour.pop_back(); // a contour returns to its start; keep each corner once
    if (contour.size() >= 3) contours.push_back(contour);
    contour.clear();
  }
  void end_region() {
    close_contour();
    for (auto &points : contours) {
      GerberItem item;
      item.dark = dark;
      item.polygons.push_back(oriented(std::move(points), true));
      add(std::move(item));
      ++layer.regions;
    }
    contours.clear();
  }

  void define_aperture(const std::string &body) {
    // ADD<code><template>[,<p1>X<p2>...]
    size_t p = 3;
    while (p < body.size() && std::isdigit(static_cast<unsigned char>(body[p]))) ++p;
    const int code = std::stoi(body.substr(3, p - 3));
    const size_t comma = body.find(',', p);
    const std::string name = body.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
    std::vector<double> params;
    if (comma != std::string::npos)
      for (const auto &part : split(body.substr(comma + 1), 'X')) if (!part.empty()) params.push_back(parse_number(part));
    Aperture a;
    const auto param = [&](size_t i) { return i < params.size() ? params[i] * scale : 0.0; };
    if (name == "C") {
      a.shape = {oriented(circle({0, 0}, param(0)), true)};
      a.width = param(0);
    } else if (name == "R") {
      a.shape = {oriented(rectangle({0, 0}, param(0), param(1)), true)};
      a.width = std::min(param(0), param(1));
      a.round = false;
    } else if (name == "O") {
      a.shape = {oriented(obround({0, 0}, param(0), param(1)), true)};
      a.width = std::min(param(0), param(1));
      a.round = false;
    } else if (name == "P") {
      const int n = std::max(3, static_cast<int>(params.size() > 1 ? params[1] : 3));
      std::vector<Point> points;
      for (int i = 0; i < n; ++i)
        points.push_back({param(0) / 2 * std::cos(2 * pi * i / n), param(0) / 2 * std::sin(2 * pi * i / n)});
      a.shape = {oriented(rotated(points, params.size() > 2 ? params[2] : 0), true)};
      a.width = param(0);
      a.round = false;
    } else if (const auto macro = macros.find(name); macro != macros.end()) {
      std::map<int, double> vars;
      for (size_t i = 0; i < params.size(); ++i) vars[static_cast<int>(i) + 1] = params[i];
      a.shape = run_macro(macro->second, vars);
      a.round = false;
      double x0 = INFINITY, x1 = -INFINITY;
      for (const auto &polygon : a.shape) for (const auto &q : polygon.points) { x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); }
      a.width = x1 > x0 ? x1 - x0 : 0;
    } else {
      warn("Unknown aperture template " + name);
    }
    if (a.shape.empty() && name != "C") warn("Aperture D" + std::to_string(code) + " has no shape");
    apertures[code] = std::move(a);
  }

  // Macro primitives in macro units (the file's), scaled to millimetres here.
  std::vector<GerberPolygon> run_macro(const std::vector<std::string> &statements, std::map<int, double> vars) {
    std::vector<GerberPolygon> shape;
    const auto mm = [&](double v) { return v * scale; };
    for (const auto &statement : statements) {
      if (statement.empty() || statement[0] == '0') continue; // comment
      if (statement[0] == '$') {
        const size_t eq = statement.find('=');
        if (eq != std::string::npos) vars[std::stoi(statement.substr(1, eq - 1))] = evaluate(statement.substr(eq + 1), vars);
        continue;
      }
      const auto parts = split(statement, ',');
      std::vector<double> v;
      for (size_t i = 1; i < parts.size(); ++i) v.push_back(evaluate(parts[i], vars));
      const auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0; };
      const int code = std::stoi(parts[0]);
      const bool exposure = at(0) != 0;
      if (code == 1) {
        shape.push_back(oriented(rotated(circle({mm(at(2)), mm(at(3))}, mm(at(1))), at(4)), exposure));
      } else if (code == 20) {
        const Point s{mm(at(2)), mm(at(3))}, e{mm(at(4)), mm(at(5))};
        const double length = std::hypot(e.x - s.x, e.y - s.y), w = mm(at(1));
        if (length <= 0) continue;
        const double nx = -(e.y - s.y) / length * w / 2, ny = (e.x - s.x) / length * w / 2;
        shape.push_back(oriented(rotated({{s.x + nx, s.y + ny}, {e.x + nx, e.y + ny}, {e.x - nx, e.y - ny}, {s.x - nx, s.y - ny}},
                                         at(6)), exposure));
      } else if (code == 21) {
        shape.push_back(oriented(rotated(rectangle({mm(at(3)), mm(at(4))}, mm(at(1)), mm(at(2))), at(5)), exposure));
      } else if (code == 4) {
        const int n = static_cast<int>(at(1));
        std::vector<Point> points;
        for (int i = 0; i <= n && 3 + 2 * i < static_cast<int>(v.size()); ++i) points.push_back({mm(at(2 + 2 * i)), mm(at(3 + 2 * i))});
        if (points.size() > 1 && std::hypot(points.front().x - points.back().x, points.front().y - points.back().y) < 1e-9)
          points.pop_back();
        if (points.size() >= 3) shape.push_back(oriented(rotated(points, at(2 + 2 * (n + 1))), exposure));
      } else if (code == 5) {
        const int n = std::max(3, static_cast<int>(at(1)));
        std::vector<Point> points;
        for (int i = 0; i < n; ++i)
          points.push_back({mm(at(2)) + mm(at(4)) / 2 * std::cos(2 * pi * i / n), mm(at(3)) + mm(at(4)) / 2 * std::sin(2 * pi * i / n)});
        shape.push_back(oriented(rotated(points, at(5)), exposure));
      } else if (code == 7) {
        // Thermal: a ring; its gaps are not cut (shown as a full ring).
        warn("Thermal macro primitives are shown without their gaps");
        shape.push_back(oriented(rotated(circle({mm(at(0)), mm(at(1))}, mm(at(2))), at(5)), true));
        shape.push_back(oriented(rotated(circle({mm(at(0)), mm(at(1))}, mm(at(3))), at(5)), false));
      } else {
        warn("Unsupported macro primitive " + parts[0]);
      }
    }
    return shape;
  }

  void finish_repeat() {
    if (!repeating) return;
    const size_t last = layer.items.size();
    for (int ix = 0; ix < repeat.x; ++ix)
      for (int iy = 0; iy < repeat.y; ++iy) {
        if (ix == 0 && iy == 0) continue;
        for (size_t k = repeat.first; k < last; ++k) {
          GerberItem copy = layer.items[k];
          for (auto &polygon : copy.polygons) for (auto &p : polygon.points) { p.x += ix * repeat.dx; p.y += iy * repeat.dy; }
          for (auto &p : copy.path) { p.x += ix * repeat.dx; p.y += iy * repeat.dy; }
          add(std::move(copy));
        }
      }
    repeating = false;
  }

  void extended(const std::string &block) {
    // One extended block may hold several *-terminated commands (an aperture macro does).
    const std::string head = block.substr(0, 2);
    if (head == "AM") {
      auto statements = split(block, '*');
      const std::string name = statements.front().substr(2);
      statements.erase(statements.begin());
      for (auto &s : statements) {
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
      }
      macros[name] = statements;
      return;
    }
    for (std::string command : split(block, '*')) {
      if (command.empty()) continue;
      const std::string code = command.substr(0, 2);
      if (code == "FS") {
        trailing = command.find('T') != std::string::npos && command.find('L') == std::string::npos;
        incremental = command.find('I', 2) != std::string::npos && command.find('I', 2) < command.find('X');
        const size_t x = command.find('X');
        if (x != std::string::npos && x + 2 < command.size()) {
          int_digits = command[x + 1] - '0';
          dec_digits = command[x + 2] - '0';
        }
        if (incremental) warn("Incremental coordinates are not supported");
      } else if (code == "MO") {
        scale = command.substr(2, 2) == "IN" ? 25.4 : 1;
      } else if (code == "AD") {
        define_aperture(command);
      } else if (code == "LP") {
        dark = command.substr(2, 1) != "C";
      } else if (code == "SR") {
        finish_repeat();
        Repeat r;
        for (char key : {'X', 'Y', 'I', 'J'}) {
          const size_t k = command.find(key, 2);
          if (k == std::string::npos) continue;
          size_t e = k + 1;
          while (e < command.size() && (std::isdigit(static_cast<unsigned char>(command[e])) || command[e] == '.' || command[e] == '-')) ++e;
          const double v = parse_number(command.substr(k + 1, e - k - 1));
          if (key == 'X') r.x = static_cast<int>(v);
          if (key == 'Y') r.y = static_cast<int>(v);
          if (key == 'I') r.dx = v * scale;
          if (key == 'J') r.dy = v * scale;
        }
        if (r.x > 1 || r.y > 1) { r.first = layer.items.size(); repeat = r; repeating = true; }
      } else if (code == "TF") {
        if (command.rfind("TF.FileFunction,", 0) == 0) layer.function = command.substr(16);
      } else if (code == "TA" || code == "TO" || code == "TD" || code == "IN" || code == "IP") {
        // Attributes and the image name/polarity of old files: nothing to draw.
      } else if (code == "OF") {
        // Image offset (deprecated): the whole image moves by A and B.
        for (char key : {'A', 'B'}) {
          const size_t k = command.find(key, 2);
          if (k == std::string::npos) continue;
          size_t e = k + 1;
          while (e < command.size() && (std::isdigit(static_cast<unsigned char>(command[e])) || command[e] == '.' || command[e] == '-')) ++e;
          (key == 'A' ? offset.x : offset.y) = parse_number(command.substr(k + 1, e - k - 1)) * scale;
        }
      } else if (code == "LM" || code == "LR" || code == "LS" || code == "AB" || code == "MI" ||
                 code == "SF" || code == "AS" || code == "IR") {
        warn("Unsupported Gerber command %" + code + " (shapes drawn without it)");
      }
    }
  }

  void word(std::string w) {
    if (w.empty()) return;
    // G04 comments carry KiCad's X2 attributes when X2 output is off.
    if (w.rfind("G04", 0) == 0) {
      const size_t at = w.find("TF.FileFunction,");
      if (at != std::string::npos) layer.function = w.substr(at + 16);
      return;
    }
    while (w.size() >= 3 && w[0] == 'G') {
      size_t e = 1;
      while (e < w.size() && std::isdigit(static_cast<unsigned char>(w[e]))) ++e;
      const int g = std::stoi(w.substr(1, e - 1));
      if (g == 1 || g == 2 || g == 3) interpolation = g;
      else if (g == 36) { region = true; contours.clear(); contour.clear(); }
      else if (g == 37) { end_region(); region = false; }
      else if (g == 74) multi_quadrant = false;
      else if (g == 75) multi_quadrant = true;
      else if (g == 70) scale = 25.4;
      else if (g == 71) scale = 1;
      else if (g == 90) incremental = false;
      else if (g == 91) { incremental = true; warn("Incremental coordinates are not supported"); }
      w.erase(0, e);
    }
    if (w.empty() || w[0] == 'M') return; // M02 end, M00/M01
    if (w[0] == 'D' && w.size() > 1) {
      const int d = std::stoi(w.substr(1));
      if (d >= 10) { aperture = d; return; }
    }
    // Coordinates and an operation.
    double x = at.x, y = at.y, i = 0, j = 0;
    int operation = last_operation;
    size_t p = 0;
    while (p < w.size()) {
      const char key = w[p++];
      size_t e = p;
      while (e < w.size() && (std::isdigit(static_cast<unsigned char>(w[e])) || w[e] == '-' || w[e] == '+' || w[e] == '.')) ++e;
      const std::string digits = w.substr(p, e - p);
      p = e;
      if (key == 'X') x = coordinate(digits);
      else if (key == 'Y') y = coordinate(digits);
      else if (key == 'I') i = coordinate(digits);
      else if (key == 'J') j = coordinate(digits);
      else if (key == 'D') operation = std::stoi(digits);
    }
    last_operation = operation;
    const Point to{x, y};
    if (operation == 2) {
      if (region) close_contour();
      at = to;
      return;
    }
    if (operation == 3) {
      const auto found = apertures.find(aperture);
      if (found == apertures.end()) { warn("Flash with an undefined aperture"); at = to; return; }
      GerberItem item;
      item.dark = dark;
      for (auto polygon : found->second.shape) {
        for (auto &q : polygon.points) { q.x += to.x; q.y += to.y; }
        item.polygons.push_back(std::move(polygon));
      }
      add(std::move(item));
      ++layer.flashes;
      at = to;
      return;
    }
    if (operation != 1) { at = to; return; }
    const auto points = segment(at, to, i, j);
    if (region) {
      if (contour.empty()) contour.push_back(at);
      contour.insert(contour.end(), points.begin(), points.end());
    } else {
      const auto found = apertures.find(aperture);
      const double width = found == apertures.end() ? 0 : found->second.width;
      if (found != apertures.end() && !found->second.round) warn("Draws with non-circular apertures are shown round");
      GerberItem *last = layer.items.empty() ? nullptr : &layer.items.back();
      if (last && !last->path.empty() && last->dark == dark && last->width == width &&
          std::hypot(last->path.back().x - at.x, last->path.back().y - at.y) < 1e-9) {
        last->path.insert(last->path.end(), points.begin(), points.end());
        for (const auto &q : points) extend(q);
      } else {
        GerberItem item;
        item.dark = dark;
        item.width = width;
        item.path.push_back(at);
        item.path.insert(item.path.end(), points.begin(), points.end());
        add(std::move(item));
      }
      ++layer.draws;
    }
    at = to;
  }
};

std::string json_text(const std::string &value) {
  std::string text = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { text += '\\'; text += static_cast<char>(c); }
    else if (c < 32) { char code[8]; std::snprintf(code, sizeof code, "\\u%04x", c); text += code; }
    else text += static_cast<char>(c);
  }
  return text + '"';
}
} // namespace

GerberLayer parse_gerber(const std::string &text, const std::string &name) {
  GerberReader reader;
  reader.layer.name = name;
  size_t p = 0;
  while (p < text.size()) {
    const char c = text[p];
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') { ++p; continue; }
    if (c == '%') {
      const size_t end = text.find('%', p + 1);
      if (end == std::string::npos) throw std::runtime_error("Unclosed Gerber extended command");
      std::string block = text.substr(p + 1, end - p - 1);
      block.erase(std::remove(block.begin(), block.end(), '\r'), block.end());
      block.erase(std::remove(block.begin(), block.end(), '\n'), block.end());
      reader.extended(block);
      p = end + 1;
      continue;
    }
    const size_t end = text.find('*', p);
    if (end == std::string::npos) break;
    std::string w = text.substr(p, end - p);
    w.erase(std::remove(w.begin(), w.end(), '\r'), w.end());
    w.erase(std::remove(w.begin(), w.end(), '\n'), w.end());
    if (w.rfind("G04", 0) != 0) w.erase(std::remove(w.begin(), w.end(), ' '), w.end());
    if (w == "M02" || w == "M00") { p = end + 1; break; }
    reader.word(w);
    p = end + 1;
  }
  if (reader.region) reader.end_region();
  reader.finish_repeat();
  if (reader.offset.x != 0 || reader.offset.y != 0) {
    auto &l = reader.layer;
    for (auto &item : l.items) {
      for (auto &polygon : item.polygons) for (auto &p : polygon.points) { p.x += reader.offset.x; p.y += reader.offset.y; }
      for (auto &p : item.path) { p.x += reader.offset.x; p.y += reader.offset.y; }
    }
    l.min_x += reader.offset.x; l.max_x += reader.offset.x; l.min_y += reader.offset.y; l.max_y += reader.offset.y;
  }
  return reader.layer;
}

GerberLayer parse_excellon(const std::string &text, const std::string &name) {
  GerberLayer layer;
  layer.name = name;
  layer.drill = true;
  double scale = 1;
  bool leading_kept = false, decimal_seen = false;
  std::map<int, double> tools;
  double diameter = 0, x = 0, y = 0;
  bool routing = false, plunged = false;
  std::vector<Point> route;
  bool first = true;
  const auto extend = [&](Point p) {
    if (first) { layer.min_x = layer.max_x = p.x; layer.min_y = layer.max_y = p.y; first = false; }
    layer.min_x = std::min(layer.min_x, p.x); layer.max_x = std::max(layer.max_x, p.x);
    layer.min_y = std::min(layer.min_y, p.y); layer.max_y = std::max(layer.max_y, p.y);
  };
  const auto value = [&](const std::string &digits) {
    if (digits.find('.') != std::string::npos) { decimal_seen = true; return parse_number(digits) * scale; }
    // Integer coordinates: 3.3 in millimetres, 2.4 in inches (Excellon's usual formats).
    const int decimals = scale == 1 ? 3 : 4, integers = scale == 1 ? 3 : 2;
    std::string d = digits;
    bool negative = false;
    if (!d.empty() && (d[0] == '-' || d[0] == '+')) { negative = d[0] == '-'; d.erase(0, 1); }
    if (leading_kept) while (static_cast<int>(d.size()) < integers + decimals) d += '0';
    const double v = d.empty() ? 0 : parse_number(d) / std::pow(10.0, decimals);
    return (negative ? -v : v) * scale;
  };
  const auto hole = [&](Point p) {
    GerberItem item;
    item.polygons.push_back(oriented(circle(p, diameter), true));
    for (const auto &q : item.polygons.back().points) extend(q);
    layer.items.push_back(std::move(item));
    ++layer.holes;
  };
  const auto slot = [&](std::vector<Point> path) {
    if (path.size() < 2) return;
    GerberItem item;
    item.path = std::move(path);
    item.width = diameter;
    for (const auto &q : item.path) extend(q);
    layer.items.push_back(std::move(item));
    ++layer.slots;
  };
  // Reads X/Y from a line part; returns whether any coordinate was present.
  const auto read_xy = [&](const std::string &part, double &px, double &py) {
    bool any = false;
    for (char key : {'X', 'Y'}) {
      const size_t k = part.find(key);
      if (k == std::string::npos) continue;
      size_t e = k + 1;
      while (e < part.size() && (std::isdigit(static_cast<unsigned char>(part[e])) || part[e] == '.' || part[e] == '-' || part[e] == '+')) ++e;
      (key == 'X' ? px : py) = value(part.substr(k + 1, e - k - 1));
      any = true;
    }
    return any;
  };
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    if (line[0] == ';') {
      const size_t at = line.find("TF.FileFunction,");
      if (at != std::string::npos) layer.function = line.substr(at + 16);
      continue;
    }
    if (line.rfind("METRIC", 0) == 0 || line.rfind("INCH", 0) == 0) {
      scale = line[0] == 'M' ? 1 : 25.4;
      leading_kept = line.find("LZ") != std::string::npos;
      continue;
    }
    if (line == "M71") { scale = 1; continue; }
    if (line == "M72") { scale = 25.4; continue; }
    if (line[0] == 'T') {
      size_t e = 1;
      while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) ++e;
      const int tool = e > 1 ? std::stoi(line.substr(1, e - 1)) : 0;
      const size_t c = line.find('C', e);
      if (c != std::string::npos) {
        size_t f = c + 1;
        while (f < line.size() && (std::isdigit(static_cast<unsigned char>(line[f])) || line[f] == '.')) ++f;
        tools[tool] = parse_number(line.substr(c + 1, f - c - 1)) * scale;
      }
      if (tools.count(tool)) diameter = tools[tool];
      continue;
    }
    if (line == "M15") { plunged = true; route = {{x, y}}; continue; }
    if (line == "M16") { if (plunged) slot(route); plunged = false; route.clear(); continue; }
    if (line == "G05") { routing = false; continue; }
    if (line.rfind("G00", 0) == 0) { routing = true; read_xy(line.substr(3), x, y); continue; }
    if (line.rfind("G01", 0) == 0) {
      double nx = x, ny = y;
      if (read_xy(line.substr(3), nx, ny)) { x = nx; y = ny; if (plunged) route.push_back({x, y}); }
      continue;
    }
    if (line[0] == 'X' || line[0] == 'Y') {
      const size_t g85 = line.find("G85");
      if (g85 != std::string::npos) {
        double sx = x, sy = y;
        read_xy(line.substr(0, g85), sx, sy);
        double ex = sx, ey = sy;
        read_xy(line.substr(g85 + 3), ex, ey);
        slot({{sx, sy}, {ex, ey}});
        x = ex; y = ey;
        continue;
      }
      read_xy(line, x, y);
      if (routing && plunged) route.push_back({x, y});
      else if (!routing) hole({x, y});
      continue;
    }
    // M48, %, G90, FMAT, M30 and feeds: nothing to draw.
  }
  if (!decimal_seen && layer.holes + layer.slots > 0)
    layer.warnings.push_back(scale == 1 ? "Integer coordinates read as 3.3 millimetres" : "Integer coordinates read as 2.4 inches");
  return layer;
}

GerberLayer parse_gerber_file(const std::string &text, const std::string &name) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  const bool excellon = (start != std::string::npos && text.compare(start, 3, "M48") == 0) ||
                        (text.find("%FS") == std::string::npos && text.find("%MO") == std::string::npos &&
                         text.find("G04") == std::string::npos);
  return excellon ? parse_excellon(text, name) : parse_gerber(text, name);
}

std::string gerber_json(const GerberLayer &layer) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(10);
  out << "{\"name\":" << json_text(layer.name) << ",\"function\":" << json_text(layer.function)
      << ",\"drill\":" << (layer.drill ? "true" : "false") << ",\"bounds\":[" << layer.min_x << "," << layer.min_y << ","
      << layer.max_x << "," << layer.max_y << "],\"counts\":{\"flashes\":" << layer.flashes << ",\"draws\":" << layer.draws
      << ",\"regions\":" << layer.regions << ",\"holes\":" << layer.holes << ",\"slots\":" << layer.slots << "},\"warnings\":[";
  for (size_t i = 0; i < layer.warnings.size(); ++i) out << (i ? "," : "") << json_text(layer.warnings[i]);
  out << "],\"items\":[";
  for (size_t i = 0; i < layer.items.size(); ++i) {
    const auto &item = layer.items[i];
    out << (i ? "," : "") << "{\"dark\":" << (item.dark ? "true" : "false") << ",\"width\":" << item.width << ",\"path\":[";
    for (size_t k = 0; k < item.path.size(); ++k) out << (k ? "," : "") << item.path[k].x << "," << item.path[k].y;
    out << "],\"polygons\":[";
    for (size_t k = 0; k < item.polygons.size(); ++k) {
      out << (k ? "," : "") << "{\"dark\":" << (item.polygons[k].dark ? "true" : "false") << ",\"points\":[";
      for (size_t q = 0; q < item.polygons[k].points.size(); ++q)
        out << (q ? "," : "") << item.polygons[k].points[q].x << "," << item.polygons[k].points[q].y;
      out << "]}";
    }
    out << "]}";
  }
  return out.str() + "]}";
}
}

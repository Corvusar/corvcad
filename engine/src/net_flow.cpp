// How current spreads through one net's copper, for the simulator's board view. SPICE knows the
// current at each device pin, not in each track, so the tracks are solved as a resistor network:
// a segment's conductance is width / length (one copper thickness). Vias, pads and filled zones
// join what touches them without resistance.
#include "corvcad/engine.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

namespace corvcad {
namespace {
// ponytail: dense elimination, so a net's joined copper is limited to this many nodes; a sparse
// solver if large signal nets need current dots. Filled zones collapse to one node each.
constexpr size_t kMaxNodes = 500;

struct Joins {
  std::vector<int> parent;
  int add() { parent.push_back(static_cast<int>(parent.size())); return parent.back(); }
  int find(int a) {
    while (parent[a] != a) a = parent[a] = parent[parent[a]];
    return a;
  }
  void join(int a, int b) { parent[find(a)] = find(b); }
};

double distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

// Position along a->b of the closest point to p, 0..1.
double along(Point p, Point a, Point b) {
  const double dx = b.x - a.x, dy = b.y - a.y, squared = dx * dx + dy * dy;
  if (squared <= 0) return 0;
  return std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0, 1.0);
}
Point at(Point a, Point b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

bool on_layer(const Pad &pad, const std::string &layer) {
  return pad.layers.find("*.Cu") != std::string::npos || pad.layers.find(layer) != std::string::npos;
}

bool inside(const Pad &pad, Point p) {
  const double angle = -pad.rotation * std::acos(-1.0) / 180, dx = p.x - pad.center.x, dy = p.y - pad.center.y;
  const double x = dx * std::cos(angle) - dy * std::sin(angle), y = dx * std::sin(angle) + dy * std::cos(angle);
  return std::abs(x) <= pad.size_x / 2 && std::abs(y) <= pad.size_y / 2;
}

// Even-odd, as a fractured filled polygon is drawn.
bool inside(const std::vector<Point> &polygon, Point p) {
  bool in = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const Point a = polygon[i], b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

struct End { int node; Point point; std::string layer; };
} // namespace

NetFlow net_flow(const Board &board, int net, const std::vector<size_t> &pins) {
  std::vector<const Track *> tracks;
  for (const Track &track : board.tracks)
    if (track.net == net && distance(track.start, track.end) > 1e-6) tracks.push_back(&track);
  std::vector<const Pad *> pads;
  std::vector<int> pad_nodes(board.pads.size(), -1);
  Joins joins;
  for (size_t i = 0; i < board.pads.size(); ++i)
    if (board.pads[i].net == net) { pads.push_back(&board.pads[i]); pad_nodes[i] = joins.add(); }
  for (size_t pin : pins)
    if (pin >= board.pads.size() || pad_nodes[pin] < 0) throw std::runtime_error("A pin is not a pad of this net");

  // Where each track must be cut: at track ends of its layer, and pads and vias, that touch it.
  struct Mark { Point point; const std::string *layer; }; // null layer: a pad or via
  std::vector<Mark> marks;
  for (const Track *track : tracks) { marks.push_back({track->start, &track->layer}); marks.push_back({track->end, &track->layer}); }
  for (const Pad *pad : pads) marks.push_back({pad->center, nullptr});
  for (const Via &via : board.vias) if (via.net == net) marks.push_back({via.center, nullptr});

  std::map<std::string, std::map<std::array<long long, 2>, int>> nodes;
  const auto node = [&](Point p, const std::string &layer) {
    const std::array<long long, 2> key{std::llround(p.x * 1000), std::llround(p.y * 1000)};
    auto &on_layer_nodes = nodes[layer];
    const auto found = on_layer_nodes.find(key);
    if (found != on_layer_nodes.end()) return found->second;
    return on_layer_nodes[key] = joins.add();
  };

  struct Piece { int a, b; double conductance; FlowEdge edge; };
  std::vector<Piece> pieces;
  std::vector<End> ends;
  struct Touch { Point mark, cut; const std::string *layer; };
  std::vector<Touch> touches;
  for (const Track *track : tracks) {
    std::vector<double> cuts{0, 1};
    for (const Mark &mark : marks) {
      if (mark.layer && *mark.layer != track->layer) continue;
      const double t = along(mark.point, track->start, track->end);
      const Point cut = at(track->start, track->end, t);
      if (distance(mark.point, cut) > track->width / 2) continue;
      cuts.push_back(t);
      if (mark.layer) touches.push_back({mark.point, cut, &track->layer});
    }
    std::sort(cuts.begin(), cuts.end());
    double from = 0;
    for (double to : cuts) {
      if (to - from <= 1e-9) continue;
      const Point a = at(track->start, track->end, from), b = at(track->start, track->end, to);
      from = to;
      const double length = distance(a, b);
      if (length < 1e-6) continue;
      const int na = node(a, track->layer), nb = node(b, track->layer);
      pieces.push_back({na, nb, track->width / length, {a, b, track->width, track->layer}});
      ends.push_back({na, a, track->layer});
      ends.push_back({nb, b, track->layer});
    }
  }
  // A track end within another track's width joins it at the cut, off the centre line or not.
  for (const Touch &touch : touches) joins.join(node(touch.mark, *touch.layer), node(touch.cut, *touch.layer));

  for (size_t i = 0; i < board.pads.size(); ++i) {
    if (pad_nodes[i] < 0) continue;
    for (const End &end : ends)
      if (on_layer(board.pads[i], end.layer) && inside(board.pads[i], end.point)) joins.join(end.node, pad_nodes[i]);
  }
  std::vector<std::pair<Point, int>> via_nodes;
  for (const Via &via : board.vias) {
    if (via.net != net) continue;
    const int id = joins.add();
    via_nodes.push_back({via.center, id});
    for (const End &end : ends)
      if (distance(end.point, via.center) <= via.size / 2) joins.join(end.node, id);
  }
  for (const ZonePolygon &zone : board.zones) {
    if (zone.net != net || !zone.filled || zone.points.size() < 3) continue;
    const int id = joins.add();
    for (const End &end : ends)
      if (end.layer == zone.layer && inside(zone.points, end.point)) joins.join(end.node, id);
    for (size_t i = 0; i < board.pads.size(); ++i)
      if (pad_nodes[i] >= 0 && on_layer(board.pads[i], zone.layer) && inside(zone.points, board.pads[i].center))
        joins.join(pad_nodes[i], id);
    for (const auto &[center, via] : via_nodes)
      if (inside(zone.points, center)) joins.join(via, id);
  }

  NetFlow flow;
  for (const Piece &piece : pieces) flow.edges.push_back(piece.edge);
  flow.unit.assign(flow.edges.size() * pins.size(), 0.0);
  if (pins.empty() || pieces.empty()) return flow;

  // The copper that carries the pins' current must be one connected piece.
  Joins connected;
  connected.parent.resize(joins.parent.size());
  std::iota(connected.parent.begin(), connected.parent.end(), 0);
  for (const Piece &piece : pieces) connected.join(joins.find(piece.a), joins.find(piece.b));
  const int component = connected.find(joins.find(pad_nodes[pins[0]]));
  for (size_t pin : pins)
    if (connected.find(joins.find(pad_nodes[pin])) != component)
      throw std::runtime_error("The pins of this net are not joined by copper");

  // Node numbers in that piece; the first pin is the reference (potential 0), so a column is the
  // current 1 A entering at its pin and leaving at the first pin. Pin currents that sum to zero
  // superpose without any current left at the reference.
  const int reference = joins.find(pad_nodes[pins[0]]);
  std::map<int, size_t> index;
  for (const Piece &piece : pieces) {
    for (int end : {joins.find(piece.a), joins.find(piece.b)})
      if (end != reference && connected.find(end) == component && !index.count(end)) {
        const size_t next = index.size();
        index[end] = next;
      }
  }
  const size_t n = index.size();
  if (n + 1 > kMaxNodes) throw std::runtime_error("This net has too much separate copper to solve");
  const size_t columns = pins.size();
  std::vector<double> matrix(n * (n + columns), 0.0);
  const auto cell = [&](size_t row, size_t column) -> double & { return matrix[row * (n + columns) + column]; };
  for (const Piece &piece : pieces) {
    const int a = joins.find(piece.a), b = joins.find(piece.b);
    if (a == b || connected.find(a) != component) continue;
    const auto ia = index.find(a), ib = index.find(b);
    if (ia != index.end()) cell(ia->second, ia->second) += piece.conductance;
    if (ib != index.end()) cell(ib->second, ib->second) += piece.conductance;
    if (ia != index.end() && ib != index.end()) {
      cell(ia->second, ib->second) -= piece.conductance;
      cell(ib->second, ia->second) -= piece.conductance;
    }
  }
  for (size_t p = 0; p < columns; ++p) {
    const auto found = index.find(joins.find(pad_nodes[pins[p]]));
    if (found != index.end()) cell(found->second, n + p) += 1;
  }
  for (size_t pivot = 0; pivot < n; ++pivot) {
    size_t best = pivot;
    for (size_t row = pivot + 1; row < n; ++row)
      if (std::abs(cell(row, pivot)) > std::abs(cell(best, pivot))) best = row;
    if (best != pivot)
      for (size_t column = pivot; column < n + columns; ++column) std::swap(cell(best, column), cell(pivot, column));
    const double divisor = cell(pivot, pivot);
    if (std::abs(divisor) < 1e-12) throw std::runtime_error("This net's copper could not be solved");
    for (size_t row = 0; row < n; ++row) {
      if (row == pivot || cell(row, pivot) == 0) continue;
      const double factor = cell(row, pivot) / divisor;
      for (size_t column = pivot; column < n + columns; ++column) cell(row, column) -= factor * cell(pivot, column);
    }
  }
  const auto potential = [&](int node_id, size_t p) {
    const auto found = index.find(node_id);
    return found == index.end() ? 0.0 : cell(found->second, n + p) / cell(found->second, found->second);
  };
  for (size_t e = 0; e < pieces.size(); ++e) {
    const int a = joins.find(pieces[e].a), b = joins.find(pieces[e].b);
    if (connected.find(a) != component) continue;
    for (size_t p = 0; p < columns; ++p)
      flow.unit[e * columns + p] = pieces[e].conductance * (potential(a, p) - potential(b, p));
  }
  return flow;
}
} // namespace corvcad

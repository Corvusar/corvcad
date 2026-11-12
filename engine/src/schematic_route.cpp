#include "corvcad/schematic_route.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace corvcad {
namespace {
constexpr double route_tolerance = 1e-6;

bool same(Point a, Point b) {
  return std::abs(a.x - b.x) < route_tolerance && std::abs(a.y - b.y) < route_tolerance;
}

bool on_segment(Point a, Point b, Point p) {
  return p.x >= std::min(a.x, b.x) - route_tolerance && p.x <= std::max(a.x, b.x) + route_tolerance &&
         p.y >= std::min(a.y, b.y) - route_tolerance && p.y <= std::max(a.y, b.y) + route_tolerance &&
         (std::abs(a.x - b.x) < route_tolerance ? std::abs(p.x - a.x) < route_tolerance
                                                : std::abs(p.y - a.y) < route_tolerance);
}

bool enters_box(Point a, Point b, const std::array<double, 4> &box) {
  const double lx = std::min(a.x, b.x), hx = std::max(a.x, b.x);
  const double ly = std::min(a.y, b.y), hy = std::max(a.y, b.y);
  return hx > box[0] + route_tolerance && lx < box[2] - route_tolerance &&
         hy > box[1] + route_tolerance && ly < box[3] - route_tolerance;
}

std::vector<Point> simplified(const std::vector<Point> &path) {
  std::vector<Point> result;
  for (const auto &p : path) {
    if (!result.empty() && same(result.back(), p)) continue;
    if (result.size() >= 2) {
      const Point a = result[result.size() - 2], b = result.back();
      const bool collinear = (std::abs(a.x - b.x) < route_tolerance && std::abs(b.x - p.x) < route_tolerance) ||
                             (std::abs(a.y - b.y) < route_tolerance && std::abs(b.y - p.y) < route_tolerance);
      if (collinear) { result.back() = p; continue; }
    }
    result.push_back(p);
  }
  return result;
}

bool route_is_clear(const std::vector<Point> &path, const RouteObstacles &obstacles) {
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const Point a = path[i], b = path[i + 1];
    for (const auto &p : obstacles.points)
      if (on_segment(a, b, p) && !same(p, path.front()) && !same(p, path.back())) return false;
    for (const auto &box : obstacles.boxes)
      if (enters_box(a, b, box)) return false;
  }
  return true;
}

double route_length(const std::vector<Point> &path) {
  double total = 0;
  for (size_t i = 0; i + 1 < path.size(); ++i)
    total += std::abs(path[i + 1].x - path[i].x) + std::abs(path[i + 1].y - path[i].y);
  return total;
}
} // namespace

void open_pin_escape(RouteObstacles &symbol, Point tip, Point outward) {
  const bool horizontal = std::abs(outward.x) > route_tolerance && std::abs(outward.y) < route_tolerance;
  const bool vertical = std::abs(outward.y) > route_tolerance && std::abs(outward.x) < route_tolerance;
  if (!horizontal && !vertical) return;
  constexpr double gap = 0.01;
  std::vector<std::array<double, 4>> boxes;
  for (const auto &box : symbol.boxes) {
    if (tip.x < box[0] || tip.x > box[2] || tip.y < box[1] || tip.y > box[3]) {
      boxes.push_back(box);
      continue;
    }
    const double left = horizontal && outward.x < 0 ? box[0] : std::max(box[0], tip.x - gap);
    const double right = horizontal && outward.x > 0 ? box[2] : std::min(box[2], tip.x + gap);
    const double top = vertical && outward.y < 0 ? box[1] : std::max(box[1], tip.y - gap);
    const double bottom = vertical && outward.y > 0 ? box[3] : std::min(box[3], tip.y + gap);
    auto add = [&](double x0, double y0, double x1, double y1) {
      if (x1 - x0 > route_tolerance && y1 - y0 > route_tolerance) boxes.push_back({x0, y0, x1, y1});
    };
    add(box[0], box[1], left, box[3]);
    add(right, box[1], box[2], box[3]);
    add(left, box[1], right, top);
    add(left, bottom, right, box[3]);
  }
  symbol.boxes = std::move(boxes);
}

std::vector<Point> plan_orthogonal_route(const RouteObstacles &obstacles, double x0, double y0,
                                         double x1, double y1) {
  if (std::hypot(x1 - x0, y1 - y0) < 0.01)
    throw std::runtime_error("Wire needs two distinct points");
  const Point a{x0, y0}, b{x1, y1};
  std::vector<std::vector<Point>> candidates{{a, {x1, y0}, b}, {a, {x0, y1}, b}};
  // Detours on the 2.54 mm grid: vertical-horizontal-vertical and the transpose.
  const double grid = 2.54;
  for (int k = 1; k <= 12; ++k) {
    for (const double y : {std::min(y0, y1) - k * grid, std::max(y0, y1) + k * grid})
      candidates.push_back({a, {x0, y}, {x1, y}, b});
    for (const double x : {std::min(x0, x1) - k * grid, std::max(x0, x1) + k * grid})
      candidates.push_back({a, {x, y0}, {x, y1}, b});
  }
  std::vector<Point> best;
  double best_length = 0;
  for (const auto &candidate : candidates) {
    const auto path = simplified(candidate);
    if (!route_is_clear(path, obstacles)) continue;
    const double length = route_length(path);
    if (best.empty() || length < best_length - route_tolerance ||
        (std::abs(length - best_length) < route_tolerance && path.size() < best.size())) {
      best = path;
      best_length = length;
    }
  }
  if (best.empty())
    throw std::runtime_error("No clear wire route: it would cross a pin or part. Route it in shorter steps.");
  return best;
}
} // namespace corvcad

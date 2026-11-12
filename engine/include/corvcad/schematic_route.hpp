#pragma once
#include "corvcad/engine.hpp"
#include <array>
#include <vector>

namespace corvcad {
// Things a touch-drawn wire must not cross, in millimetres. KiCad joins a wire to any pin
// end, wire end, junction or label on its interior, so those are points the route may only
// start or end on. Boxes are part bodies and pin lines (min x, min y, max x, max y, open).
struct RouteObstacles {
  std::vector<Point> points;
  std::vector<std::array<double, 4>> boxes;
};

// Open a narrow outward corridor from a pin tip buried in its symbol's drawing bounds
// (for example an LED's light arrows). Call on this symbol's boxes only, before merging
// other symbols' obstacles. The pin remains a point obstacle, so unrelated wires cannot
// pass through it. outward must follow the pin axis, from its body root toward its tip.
void open_pin_escape(RouteObstacles &symbol, Point tip, Point outward);

// Shortest clear orthogonal route on the 2.54 mm grid: one bend, or a three-segment detour
// up to 12 grid steps out. Throws when the points coincide or no candidate is clear. Both
// schematic backends use it, each with obstacles from its own model.
std::vector<Point> plan_orthogonal_route(const RouteObstacles &obstacles, double x0, double y0,
                                         double x1, double y1);
} // namespace corvcad

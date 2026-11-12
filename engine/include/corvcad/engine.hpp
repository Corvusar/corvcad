#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace corvcad {
// UTF-8 std::string <-> filesystem path. C++20's char8_t makes fs::u8path deprecated
// and path::u8string() return std::u8string; keep the engine's strings as plain UTF-8.
inline std::filesystem::path utf8_path(const std::string &text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string utf8_string(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}
inline std::string generic_utf8_string(const std::filesystem::path &path) {
  const std::u8string text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}

struct Point { double x = 0, y = 0; };
struct Hole { Point center; double diameter = 0; std::string reference; };
// A footprint. `model` is its first 3D model path as written ("" without one), with KiCad's
// offset (mm), rotation (degrees) and scale; `extent` is its pads' box in footprint coordinates
// (unrotated, y down): min x, min y, max x, max y.
struct Placement { Point center; double rotation = 0; std::string reference, value, library;
                   bool back = false; std::string model;
                   std::array<double, 3> offset{}, rotate{}, scale{1, 1, 1};
                   std::array<double, 4> extent{}; };
struct Net { int code = 0; std::string name; };
struct Track { Point start, end; double width = 0; int net = 0; std::string layer, uuid; };
struct Via { Point center; double size = 0, drill = 0; int net = 0; std::string uuid; };
struct Pad { Point center; double size_x = 0, size_y = 0, rotation = 0; int net = 0;
             std::string reference, number, type, shape, layers; };
// One zone outline, or one filled polygon of a zone (stored fractured: holes are joined to the
// outline, so an even-odd fill draws it).
// A zone's properties as KiCad writes them (defaults when absent): connection 0 solid, 1 thermal,
// 2 none, 3 thermal for through-hole pads only.
struct ZoneSettings {
  int priority = 0, connection = 1;
  double clearance = 0.5, min_thickness = 0.25, thermal_gap = 0.5, spoke_width = 0.5;
  bool hatched = false;
  bool teardrop = false; // (attr (teardrop ...)): KiCad makes these from its teardrop settings
};
struct ZonePolygon { int net = 0; std::string layer; bool filled = false; std::vector<Point> points; std::string uuid;
                     ZoneSettings settings; };
// A top-level board text (gr_text).
struct BoardText { Point position; double size = 0, rotation = 0; std::string text, layer, uuid; };
struct Board {
  std::vector<Point> outline;
  std::vector<ZonePolygon> zones;
  // Top-level gr_line drawings off Edge.Cuts (silkscreen, fab, user layers).
  std::vector<Track> graphics;
  std::vector<BoardText> texts;
  std::vector<Hole> mounting_holes;
  std::vector<Placement> placements;
  std::vector<Net> nets;
  std::vector<Track> tracks;
  std::vector<Via> vias;
  std::vector<Pad> pads;
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};
struct Settings {
  double margin = 3.0, wall = 2.0, floor = 2.0;
  double board_clearance = 2.0, standoff_height = 4.0;
  double standoff_diameter = 6.0, lid_thickness = 2.0, lid_gap = 0.25;
};
struct Triangle { std::array<float, 3> a, b, c; };
struct Model { std::vector<Triangle> enclosure, board, standoffs, floor, walls, rim, lid; };

// One piece of track in a net's copper, a to b.
struct FlowEdge { Point a, b; double width = 0; std::string layer; };
// `unit` has edges x pins entries: the current along each edge (positive a to b) for 1 A entering
// the net at each pin and leaving at the first. Pin currents summing to zero superpose.
struct NetFlow { std::vector<FlowEdge> edges; std::vector<double> unit; };

Board parse_kicad_board(const std::string &source);
// Solves a net's tracks as resistors between the given pins (indices into board.pads, on that
// net). Throws when the pins aren't joined by copper or the net is too large.
NetFlow net_flow(const Board &board, int net, const std::vector<size_t> &pins);
// Rewrite only the selected footprint's top-level (at ...) expression.
std::string move_footprint(const std::string &source, const std::string &reference,
                           double x, double y, double rotation);
std::string delete_footprint(const std::string &source, const std::string &reference);
// Removes the top-level segment, track arc, via, zone, gr_line or gr_text with this UUID.
std::string delete_board_copper(const std::string &source, const std::string &uuid);
// Moves the top-level gr_line, gr_text or zone with this UUID by (dx, dy) mm, editing only its
// coordinates. A moved zone loses its stale filled polygons.
std::string move_board_drawing(const std::string &source, const std::string &uuid, double dx, double dy);
// Sets a gr_text's string and height (0.5-10 mm, stroke 15% of it), keeping everything else.
std::string set_board_text(const std::string &source, const std::string &uuid, const std::string &text,
                           double size);
// The source without its top-level Edge.Cuts graphics (gr_line/arc/rect/poly/circle); not
// a valid board until an outline is added again.
std::string remove_board_outline(const std::string &source);
std::string rename_footprint(const std::string &source, const std::string &reference,
                             const std::string &new_reference);
// Turns a footprint 90 degrees counter-clockwise; KiCad stores its pad and text angles
// absolute, so they turn with it.
std::string rotate_footprint(const std::string &source, const std::string &reference);
// Sets each pad's (net "NAME") on the footprints named in `nets` (reference -> pad number ->
// net name) in KiCad 10's named-net form. Numbered pads missing from a footprint's map lose
// their net; other footprints and all other text are untouched.
std::string set_pad_nets(const std::string &source,
                         const std::map<std::string, std::map<std::string, std::string>> &nets);
void validate(const Board &board, const Settings &settings);
Model make_model(const Board &board, const Settings &settings);
void write_stl(const Model &model, const std::string &path);
void write_lid_stl(const Model &model, const std::string &path);
// Available only in builds configured with CORVCAD_WITH_OCCT=ON.
void write_step(const Board &board, const Settings &settings, const std::string &path);
bool step_available();
}

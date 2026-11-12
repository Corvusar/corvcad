#pragma once

#include "corvcad/engine.hpp"
#include <string>
#include <vector>

namespace corvcad {
// A reader for Gerber (RS-274X with X2 attributes) and Excellon drill files, for viewing
// fabrication outputs. Shapes come out in millimetres with the files' y axis (up).
// Supported: FS/MO, standard apertures (C, R, O, P) and aperture macros (primitives 1, 4, 5,
// 7, 20, 21 with $ variables and expressions), linear and circular interpolation (G75; G74
// sampled the short way), regions (G36/G37), LP polarity, step and repeat (SR), the image
// offset (OF); Excellon
// METRIC/INCH tools, decimal or integer coordinates, G85 and routed (M15/M16) slots.
// Unsupported commands are listed in `warnings`, never silently guessed.

struct GerberPolygon { std::vector<Point> points; bool dark = true; };
// One object: a flash or region (filled polygons; a clear polygon erases within the layer) or
// a draw (a stroked path of `width`, round ends).
struct GerberItem {
  bool dark = true;
  std::vector<GerberPolygon> polygons;
  std::vector<Point> path;
  double width = 0;
};
struct GerberLayer {
  std::string name;
  // X2 file function ("Copper,L1,Top", "Soldermask,Top", "Profile,NP", "Plated,1,2,PTH"...).
  std::string function;
  bool drill = false;
  std::vector<GerberItem> items;
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0; // extents of shapes and paths (no stroke width)
  int flashes = 0, draws = 0, regions = 0, holes = 0, slots = 0;
  std::vector<std::string> warnings;
};

// Gerber or Excellon, chosen by content (Excellon starts with M48 or has no Gerber commands).
GerberLayer parse_gerber_file(const std::string &text, const std::string &name);
GerberLayer parse_gerber(const std::string &text, const std::string &name);
GerberLayer parse_excellon(const std::string &text, const std::string &name);
// For the app: the layer as JSON {name, function, drill, bounds:[x0,y0,x1,y1], counts:{...},
// warnings:[...], items:[{dark, width, path:[x,y...], polygons:[{dark, points:[x,y...]}]}]}.
std::string gerber_json(const GerberLayer &layer);
}

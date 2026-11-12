#pragma once

#include "corvcad/eda.hpp"
#include "corvcad/engine.hpp"
#include <string>
#include <vector>

namespace corvcad {
// Orthogonal route between two schematic points that does not pass over a pin end,
// wire end, junction or label (KiCad would connect them) or through a CorvCAD
// two-pin body. Prefers the shortest path, then fewest bends. Throws if none fits.
std::vector<Point> route_schematic_wire(const std::string &source, double x0, double y0,
                                        double x1, double y1);
// The same on an already parsed sheet, for live previews (no text parse per call).
std::vector<Point> route_schematic_wire(const Schematic &sheet, double x0, double y0,
                                        double x1, double y1);
// Source-preserving additions to a limited KiCad 10 project. Existing nodes are
// kept byte-for-byte; new nodes are inserted before the document's closing ')'.
// `instance_path` is the KiCad sheet-instance path written to the symbol's instances
// block ("/<root uuid>[/<sheet uuid>...]"); empty means the root sheet of this file.
// `taken_references` adds designators used elsewhere in the project (other sheets).
std::string add_schematic_two_pin(const std::string &source, const std::string &project,
                                  double x, double y, const std::string &instance_path = "",
                                  const std::vector<std::string> &taken_references = {});
std::string add_schematic_wire(const std::string &source, double x0, double y0,
                               double x1, double y1);
std::string add_schematic_orthogonal_wire(const std::string &source, double x0,
                                          double y0, double x1, double y1);
std::string add_schematic_label(const std::string &source, const std::string &name,
                                double x, double y);
std::string add_board_two_pad(const std::string &source, const std::string &reference,
                              const std::string &value, double x, double y);
std::string add_board_mounting_hole(const std::string &source, double x, double y);
// Copper in KiCad 10's segment/via form with a named net (none when `net` is empty).
// Tracks run on "F.Cu" or "B.Cu"; vias join both. Both ends must lie inside the outline.
std::string add_board_track(const std::string &source, double x0, double y0, double x1, double y1,
                            const std::string &layer, const std::string &net, double width = 0.25);
std::string add_board_via(const std::string &source, double x, double y, const std::string &net,
                          double size = 0.6, double drill = 0.3);
// Replaces every top-level Edge.Cuts graphic with one gr_poly through `points` (a simple
// polygon of 3-256 corners).
std::string set_board_outline(const std::string &source, const std::vector<Point> &points);
// An unfilled copper zone with KiCad's default settings on "F.Cu" or "B.Cu" (no net when
// `net` is empty); KiCad's zone filler fills it.
std::string add_board_zone(const std::string &source, const std::vector<Point> &points,
                           const std::string &layer, const std::string &net);
// A gr_line or gr_text on F.SilkS, B.SilkS, F.Fab or B.Fab; back-side text is mirrored.
std::string add_board_line(const std::string &source, double x0, double y0, double x1, double y1,
                           const std::string &layer, double width = 0.15);
std::string add_board_text(const std::string &source, const std::string &text, double x, double y,
                           const std::string &layer, double size = 1);

// A subset of KiCad's Update PCB from Schematic for a single-sheet project. It assigns every
// schematic symbol's pin nets (schematic_nets) to the pads of the board footprint with the
// same reference, and adds missing CorvCAD:TwoPad footprints at free spots inside the
// outline. Symbols whose footprint is missing and is any other library ID are listed in
// `skipped`, as are references whose board footprint ID differs from the schematic's.
// Board-only footprints (mounting holes) are untouched. Throws for hierarchical schematics.
struct BoardUpdate { std::string board; std::vector<std::string> added, skipped; };
BoardUpdate update_board_from_schematic(const std::string &board, const std::string &schematic);

// Project-local libraries for the authored two-pin part and mounting hole.
// Creates missing files only. Existing files are never replaced.
void ensure_authoring_libraries(const std::string &project_directory);
}

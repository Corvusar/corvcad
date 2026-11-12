#pragma once

#include "corvcad/engine.hpp"
#include <array>
#include <string>
#include <vector>

namespace corvcad {

// One body shape of a placed symbol for drawing: rectangles, circles and arcs come as
// polylines (arcs and circles sampled), bezier curves as their control points.
struct SchGraphic {
  std::vector<Point> points;
  bool closed = false;
  int fill = 0; // KiCad's fill type: 0 none, 1 outline (stroke colour), 2 background
};
// A placed field's text anchor in sheet millimetres, as the sheet stores it. Vertical when the
// field angle and the symbol rotation together turn it, as KiCad draws it.
struct SchField { Point position; bool visible = false, vertical = false; };
struct SchSymbol {
  std::string uuid, reference, value, footprint, library_id;
  Point position;
  double rotation = 0;
  SchField reference_field, value_field;
  // The definition's pin_names offset and hide, and pin_numbers hide (KiCad's defaults).
  double pin_name_offset = 0.508;
  bool pin_names_visible = true, pin_numbers_visible = true;
  // From the sheet's embedded lib_symbols for the placed unit, in sheet millimetres:
  // pin connection points, and the box around body graphics and pin lines (min x, min y,
  // max x, max y). Empty when the definition is missing.
  std::vector<Point> pins;
  std::vector<Point> pin_ends; // the body end of each pin line, parallel to pins
  std::vector<bool> pin_visible; // false for hidden pins, parallel to pins
  std::vector<SchGraphic> graphics;
  std::vector<std::string> pin_numbers, pin_names; // parallel to pins
  std::vector<bool> pin_global_power; // power input, hidden or on a power symbol: a named net
  std::array<double, 4> body{};
  bool has_body = false;
  bool power = false; // definition marked (power): its Value names a global net
  int unit = 1, unit_count = 1; // placed unit; units in the definition (U1A, U1B when > 1)
};
struct SchWire { Point start, end; std::string uuid; };
struct SchJunction { Point position; std::string uuid; };
struct SchLabel { Point position; std::string text, uuid, kind; };
struct SchNoConnect { Point position; std::string uuid; };
struct SchSheetPin { Point position; std::string name, uuid, shape; };
struct SchSheet { std::string uuid, name, file; Point position, size; std::vector<SchSheetPin> pins; };
struct Schematic {
  std::string uuid;
  int version = 0;
  std::vector<std::string> unknown_top_level;
  std::vector<SchSymbol> symbols;
  std::vector<SchWire> wires;
  std::vector<SchWire> buses;
  std::vector<SchWire> bus_entries; // wire-to-bus entries, from (at) to (at)+(size)
  std::vector<SchJunction> junctions;
  std::vector<SchLabel> labels;
  std::vector<SchNoConnect> no_connects;
  std::vector<SchLabel> texts;      // free text notes (kind "text"); they join no net
  std::vector<SchWire> note_lines;  // graphic polylines as segments; a polyline's segments share its UUID
  std::vector<SchSheet> sheets;
  // KiCad's page: the paper size in millimetres (A4 landscape when missing) and the title block.
  Point paper{297, 210};
  std::string title, date, revision, company;
};

// Reads one .kicad_sch sheet. This is a format-compatible subset, not KiCad ERC.
// Unknown expressions remain in the source.
Schematic parse_kicad_schematic(const std::string &source);

struct SchNetPin { size_t symbol; std::string number; Point position; };
struct SchNet {
  std::string name; // KiCad's style: "/LABEL", "GLOBAL", "Net-(R1-Pad2)", "unconnected-(R3-Pad1)"
  std::vector<SchNetPin> pins;
  std::vector<size_t> wires, labels; // indices into Schematic::wires / labels
};
// Connectivity of one sheet, following the rules KiCad 10.0.6 applies (checked with
// kicad-cli): items meet at shared points; a label joins a wire anywhere along it; a wire end
// or pin on another wire's interior joins it only through a junction; labels with the same
// text join; power symbols join every same-named net. Not modelled: buses, hierarchy (sheet
// pins and multi-sheet names), hidden power pins, no-connect flags. `sheet_path` prefixes
// local label names ("/" for the root sheet).
std::vector<SchNet> schematic_nets(const Schematic &schematic, const std::string &sheet_path = "/");

// Move one placed symbol and its visible/hidden property anchors by the same
// delta. Identify the symbol by KiCad UUID, not reference (which may change).
// Both operations preserve every unrelated byte of the source file.
std::string move_schematic_symbol(const std::string &source, const std::string &uuid,
                                  double x, double y);
// Turns a placed symbol 90 degrees counter-clockwise about its origin, with its fields.
std::string rotate_schematic_symbol(const std::string &source, const std::string &uuid);
std::string set_schematic_property(const std::string &source, const std::string &uuid,
                                   const std::string &property, const std::string &value);
// Source-preserving edits for touch actions. UUIDs select top-level placed items.
std::string delete_schematic_item(const std::string &source, const std::string &uuid);
std::string rename_schematic_symbol(const std::string &source, const std::string &uuid,
                                    const std::string &reference);
std::string rename_schematic_label(const std::string &source, const std::string &uuid,
                                   const std::string &name);
std::string move_schematic_label(const std::string &source, const std::string &uuid,
                                 double x, double y);

}

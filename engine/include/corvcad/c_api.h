#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cc_board cc_board;
typedef struct cc_schematic cc_schematic;
typedef struct { double x, y, diameter; char reference[64]; } cc_hole;
// A footprint; `model` is its first 3D model path as written, with KiCad's offset (mm), rotate
// (degrees) and scale; `extent` its pads' box in footprint coordinates (min x, min y, max x, max y).
typedef struct { double x, y, rotation; char reference[64], value[64]; int back; char model[256];
                 double offset[3], rotate[3], scale[3], extent[4]; } cc_placement;
typedef struct { int code; char name[128]; } cc_net;
typedef struct { double x0, y0, x1, y1, width; int net; char layer[32], uuid[64]; } cc_track;
typedef struct { double x, y, size, drill; int net; char uuid[64]; } cc_via;
/* A zone outline (filled 0) or one of its filled polygons (filled 1), on one layer, with the
   zone's properties in millimetres: connection 0 solid, 1 thermal, 2 none, 3 thermal for
   through-hole pads only. */
typedef struct { int net, filled; char layer[32]; size_t point_count; char uuid[64];
                 int priority, connection, hatched, teardrop; double clearance, min_thickness, thermal_gap, spoke_width; } cc_zone;
/* A top-level board text: position, height and angle (degrees) in millimetres. */
typedef struct { double x, y, size, rotation; char layer[32], uuid[64], text[128]; } cc_board_text;
typedef struct { double x, y, size_x, size_y, rotation; int net;
                 char reference[64], number[32], type[32], shape[32], layers[128]; } cc_pad;
typedef struct { double margin, wall, floor, board_clearance, standoff_height,
                 standoff_diameter, lid_thickness, lid_gap; } cc_settings;
typedef struct { float ax, ay, az, bx, by, bz, cx, cy, cz; } cc_triangle;
typedef struct { double x, y, rotation; char uuid[64], reference[64], value[128],
                 footprint[192], library_id[192]; } cc_sch_symbol;
typedef struct { double x0, y0, x1, y1; char uuid[64]; } cc_sch_wire;
typedef struct { double x, y; char uuid[64]; } cc_sch_junction;
// kind: "label", "global_label" or "hierarchical_label".
typedef struct { double x, y; char uuid[64], text[128], kind[24]; } cc_sch_label;
// A child-sheet box on this sheet: top-left corner and size in millimetres.
typedef struct { double x, y, width, height; char uuid[64], name[128], file[256]; } cc_sch_sheet;

// Creates parent/name/{name.kicad_pro,name.kicad_sch,name.kicad_pcb}.
// Returns 0 with an error message if the name already exists or input is invalid.
int cc_create_project(const char *parent, const char *name, double board_width,
                      double board_height, char *error, size_t error_size);
// Renames parent/name to parent/new_name with its <name>.* files, as KiCad's Save Project As in
// place (the project file and board name the renamed files). 0 with an error message if the
// new name is invalid or taken.
int cc_rename_project(const char *parent, const char *name, const char *new_name, char *error,
                      size_t error_size);
int cc_prepare_project_libraries(const char *folder, char *error, size_t error_size);
// Gerber or Excellon text (corvcad/gerber.hpp) as the viewer's JSON: shapes in millimetres, y up.
char *cc_gerber_json(const char *text, const char *name, char *error, size_t error_size);
// PCB calculator (corvcad/calculator.hpp, KiCad pcb_calculator's formulas). Each returns 1, or 0
// with an error message. Track: give the current (width 0) or the width (current 0).
typedef struct { double current_a, width_mm, area_mm2, resistance_ohm, voltage_drop_v, power_w; } cc_track_calc;
int cc_calc_track(double current_a, double width_mm, double thickness_um, double rise_c, double length_mm, int internal,
                  cc_track_calc *out, char *error, size_t error_size);
typedef struct {
  double hole_mm, plating_um, length_mm, pad_mm, clearance_mm, impedance_ohm, current_a, resistivity, permittivity,
      rise_c, rise_time_ns;
} cc_via_calc_input;
typedef struct {
  double resistance_ohm, voltage_drop_v, power_w, ampacity_a, thermal_c_per_w, capacitance_pf, rise_degradation_ps,
      inductance_nh, reactance_ohm;
} cc_via_calc;
int cc_calc_via(cc_via_calc_input input, cc_via_calc *out, char *error, size_t error_size);
// IPC-2221 spacing in millimetres: B1, B2, B3, B4, A5, A6, A7.
int cc_calc_spacing(double volts, double out[7], char *error, size_t error_size);
// Regulator: Vout from R1 and R2, or (solve_r2) R2 from Vout and R1. Iadj 0 for the standard type.
int cc_calc_regulator(double vref, double r1_ohm, double r2_ohm, double vout, double iadj_a, int solve_r2, double *result,
                      char *error, size_t error_size);
// Nearest value of E6/E12/E24/E48/E96 and its colour bands, comma-separated ("brown,black,red,gold").
int cc_calc_resistor(double ohms, int series, double *value, char *bands, size_t bands_size, char *error,
                     size_t error_size);
// Project libraries (corvcad/library.hpp): the libraries of the project's sym-lib-table and
// fp-lib-table with their symbols and footprints as JSON. `footprint` selects footprints (1) or
// symbols (0). An item is edited as its KiCad text: a template from a tab-separated spec
// (cc_library_generate) or the library's own (cc_library_item_text), changed one edit at a time
// (cc_library_edit, the edits in library.hpp), shown through cc_library_item_json (a derived
// symbol passes its base's text) and saved with cc_save_library_item_text. The int calls write
// the library files and tables themselves and return 1, or 0 with an error message.
char *cc_project_libraries_json(const char *folder, char *error, size_t error_size);
char *cc_library_generate(int footprint, const char *spec, char *error, size_t error_size);
char *cc_library_item_text(const char *folder, int footprint, const char *library, const char *name, char *error,
                           size_t error_size);
char *cc_library_edit(int footprint, const char *text, const char *op, char *error, size_t error_size);
char *cc_library_item_json(int footprint, const char *text, const char *base, char *error, size_t error_size);
int cc_save_library_item_text(const char *folder, int footprint, const char *library, const char *original,
                              const char *text, int replace, char *error, size_t error_size);
int cc_import_project_library(const char *folder, const char *source, char *error, size_t error_size);
int cc_import_library_footprint(const char *folder, const char *library, const char *source, char *error,
                                size_t error_size);
int cc_create_project_library(const char *folder, int footprint, const char *name, char *error, size_t error_size);
int cc_remove_project_library(const char *folder, int footprint, const char *name, char *error, size_t error_size);
int cc_delete_library_item(const char *folder, int footprint, const char *library, const char *name, char *error,
                           size_t error_size);
// Returned UTF-8 text must be released with cc_free_text. New nodes are added
// without altering existing KiCad expressions. The caller saves the text.
char *cc_add_schematic_two_pin(const char *source, const char *project,
                               double x, double y, char *error, size_t error_size);
char *cc_add_schematic_wire(const char *source, double x0, double y0,
                            double x1, double y1, char *error, size_t error_size);
// Plans the route cc_add_schematic_orthogonal_wire would add. Writes up to
// `capacity` points as x,y pairs into `xy`; returns the point count, 0 on error.
size_t cc_route_schematic_wire(const char *source, double x0, double y0, double x1, double y1,
                               double *xy, size_t capacity, char *error, size_t error_size);
char *cc_add_schematic_orthogonal_wire(const char *source, double x0, double y0,
                                       double x1, double y1, char *error, size_t error_size);
char *cc_add_schematic_label(const char *source, const char *name,
                             double x, double y, char *error, size_t error_size);
char *cc_add_board_two_pad(const char *source, const char *reference,
                           const char *value, double x, double y,
                           char *error, size_t error_size);
char *cc_add_board_mounting_hole(const char *source, double x, double y,
                                  char *error, size_t error_size);
char *cc_delete_schematic_item(const char *source, const char *uuid,
                                char *error, size_t error_size);
char *cc_rename_schematic_symbol(const char *source, const char *uuid,
                                  const char *reference, char *error, size_t error_size);
char *cc_rename_schematic_label(const char *source, const char *uuid,
                                 const char *name, char *error, size_t error_size);
char *cc_move_schematic_label(const char *source, const char *uuid,
                               double x, double y, char *error, size_t error_size);
char *cc_delete_board_footprint(const char *source, const char *reference,
                                 char *error, size_t error_size);
char *cc_rename_board_footprint(const char *source, const char *reference,
                                 const char *new_reference, char *error, size_t error_size);
// Turns a footprint 90 degrees counter-clockwise; returns the new board text.
char *cc_rotate_board_footprint(const char *source, const char *reference,
                                char *error, size_t error_size);
// Copper. `net` is a net name, "" for none; layer is "F.Cu" or "B.Cu". Width, via size and
// drill are millimetres, 0 for the defaults (0.25 mm tracks; 0.6/0.3 mm vias). Delete takes the
// segment's or via's UUID. All return the new board text.
char *cc_add_board_track(const char *source, double x0, double y0, double x1, double y1,
                         const char *layer, const char *net, double width, char *error, size_t error_size);
char *cc_add_board_via(const char *source, double x, double y, const char *net, double size, double drill,
                       char *error, size_t error_size);
// Deletes the track, via, zone, line or text with this UUID.
char *cc_delete_board_copper(const char *source, const char *uuid, char *error, size_t error_size);
// Moves the drawn line, text or zone with this UUID by (dx, dy) mm (a moved zone is unfilled
// until KiCad's filler runs); sets a drawn text's string and height (0.5-10 mm).
char *cc_move_board_drawing(const char *source, const char *uuid, double dx, double dy, char *error,
                            size_t error_size);
char *cc_set_board_text(const char *source, const char *uuid, const char *text, double size, char *error,
                        size_t error_size);
// Drawing. Shapes are `count` corners as x,y pairs in `xy` (a simple polygon, 3-256 corners).
// The outline replaces every Edge.Cuts graphic. A zone is unfilled, on "F.Cu" or "B.Cu", `net`
// "" for none. Lines and text go on "F.SilkS", "B.SilkS", "F.Fab" or "B.Fab"; width and text
// height are millimetres, 0 for the defaults (0.15 mm, 1 mm).
char *cc_set_board_outline(const char *source, const double *xy, size_t count, char *error, size_t error_size);
char *cc_add_board_zone(const char *source, const double *xy, size_t count, const char *layer, const char *net,
                        char *error, size_t error_size);
char *cc_add_board_line(const char *source, double x0, double y0, double x1, double y1, const char *layer,
                        double width, char *error, size_t error_size);
char *cc_add_board_text(const char *source, const char *text, double x, double y, const char *layer, double size,
                        char *error, size_t error_size);
// Update PCB from schematic, the subset in corvcad::update_board_from_schematic: pad nets from
// a single-sheet schematic plus missing two-pad footprints. Returns the new board text;
// `report` gets e.g. "Added R4. Not updated: U1." or "" when every symbol was handled.
char *cc_update_board_from_schematic(const char *board, const char *schematic, char *report,
                                     size_t report_size, char *error, size_t error_size);
void cc_free_text(char *text);

cc_schematic *cc_schematic_parse(const char *utf8, char *error, size_t error_size);
void cc_schematic_free(cc_schematic *schematic);
size_t cc_sch_symbol_count(const cc_schematic *schematic);
cc_sch_symbol cc_sch_symbol_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_wire_count(const cc_schematic *schematic);
cc_sch_wire cc_sch_wire_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_junction_count(const cc_schematic *schematic);
cc_sch_junction cc_sch_junction_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_label_count(const cc_schematic *schematic);
cc_sch_label cc_sch_label_at(const cc_schematic *schematic, size_t index);
// No-connect flags share the junction record (position and UUID).
// Sheet text notes (kind "text") and graphic note lines (one segment per polyline edge).
size_t cc_sch_text_count(const cc_schematic *schematic);
cc_sch_label cc_sch_text_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_note_line_count(const cc_schematic *schematic);
cc_sch_wire cc_sch_note_line_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_no_connect_count(const cc_schematic *schematic);
cc_sch_junction cc_sch_no_connect_at(const cc_schematic *schematic, size_t index);
// cc_route_schematic_wire on this parsed sheet: no text parse per call, for live previews.
size_t cc_sch_route(const cc_schematic *schematic, double x0, double y0, double x1, double y1,
                    double *xy, size_t capacity, char *error, size_t error_size);
// The sheet's page: paper size in millimetres (A4 landscape when missing) and title block.
typedef struct { double width, height; char title[128], date[32], revision[32], company[128]; } cc_sch_page;
cc_sch_page cc_sch_page_info(const cc_schematic *schematic);
size_t cc_sch_sheet_count(const cc_schematic *schematic);
cc_sch_sheet cc_sch_sheet_at(const cc_schematic *schematic, size_t index);
// A sheet box's pins: text is the pin name, kind its shape ("input", "output", "bidirectional"...).
size_t cc_sch_sheet_pin_count(const cc_schematic *schematic, size_t sheet);
cc_sch_label cc_sch_sheet_pin_at(const cc_schematic *schematic, size_t sheet, size_t index);
// Bus segments and wire-to-bus entries (from the entry's position to position + size).
size_t cc_sch_bus_count(const cc_schematic *schematic);
cc_sch_wire cc_sch_bus_at(const cc_schematic *schematic, size_t index);
size_t cc_sch_bus_entry_count(const cc_schematic *schematic);
cc_sch_wire cc_sch_bus_entry_at(const cc_schematic *schematic, size_t index);
// Connectivity of this sheet (corvcad::schematic_nets; root-sheet names). A pin is a symbol
// index into cc_sch_symbol_at and its pin number and connection point.
typedef struct { char name[128]; size_t pin_count; } cc_sch_net;
typedef struct { size_t symbol; double x, y; char number[32]; } cc_sch_pin;
/* A placed symbol's drawing from the sheet's lib_symbols, for its unit, in sheet millimetres:
   body shapes as polylines (fill 0 none, 1 stroke colour, 2 background), and pin lines from
   the connection point (x, y) to the body (end_x, end_y). None when the definition is missing. */
typedef struct { int closed, fill; size_t point_count; } cc_sch_graphic;
typedef struct { double x, y, end_x, end_y; int visible; char number[32], name[64]; } cc_sch_symbol_pin;
size_t cc_sch_symbol_graphic_count(const cc_schematic *schematic, size_t symbol);
cc_sch_graphic cc_sch_symbol_graphic_at(const cc_schematic *schematic, size_t symbol, size_t index);
void cc_sch_symbol_graphic_point(const cc_schematic *schematic, size_t symbol, size_t graphic, size_t index,
                                 double *x, double *y);
/* Its text: Reference and Value anchors as the sheet places them (visible 0 when hidden or
   missing; vertical when drawn turned), and the definition's pin name offset (0: names over
   the pin line) and whether pin names and numbers show. */
typedef struct { double reference_x, reference_y, value_x, value_y, pin_name_offset;
                 int reference_visible, reference_vertical, value_visible, value_vertical,
                     pin_names_visible, pin_numbers_visible; } cc_sch_symbol_text;
cc_sch_symbol_text cc_sch_symbol_text_at(const cc_schematic *schematic, size_t symbol);
size_t cc_sch_symbol_pin_count(const cc_schematic *schematic, size_t symbol);
cc_sch_symbol_pin cc_sch_symbol_pin_at(const cc_schematic *schematic, size_t symbol, size_t index);
size_t cc_sch_net_count(const cc_schematic *schematic);
cc_sch_net cc_sch_net_at(const cc_schematic *schematic, size_t index);
cc_sch_pin cc_sch_net_pin_at(const cc_schematic *schematic, size_t net, size_t index);
// Net index of a wire or label, or -1.
long cc_sch_wire_net(const cc_schematic *schematic, size_t wire);
long cc_sch_label_net(const cc_schematic *schematic, size_t label);
int cc_move_schematic_symbol_file(const char *utf8, const char *uuid, double x, double y,
                                   const char *path, char *error, size_t error_size);
int cc_set_schematic_property_file(const char *utf8, const char *uuid, const char *property,
                                    const char *value, const char *path,
                                    char *error, size_t error_size);

cc_board *cc_board_parse(const char *utf8, char *error, size_t error_size);
void cc_board_free(cc_board *board);
// KiCad general/thickness in millimetres, or 0 when absent. No invented default.
double cc_board_thickness(const cc_board *board);
void cc_board_bounds(const cc_board *board, double *min_x, double *min_y, double *max_x, double *max_y);
size_t cc_outline_count(const cc_board *board);
void cc_outline_point(const cc_board *board, size_t index, double *x, double *y);
size_t cc_hole_count(const cc_board *board);
cc_hole cc_hole_at(const cc_board *board, size_t index);
size_t cc_placement_count(const cc_board *board);
cc_placement cc_placement_at(const cc_board *board, size_t index);
size_t cc_net_count(const cc_board *board);
cc_net cc_net_at(const cc_board *board, size_t index);
size_t cc_track_count(const cc_board *board);
cc_track cc_track_at(const cc_board *board, size_t index);
size_t cc_via_count(const cc_board *board);
cc_via cc_via_at(const cc_board *board, size_t index);
size_t cc_zone_count(const cc_board *board);
cc_zone cc_zone_at(const cc_board *board, size_t index);
void cc_zone_point(const cc_board *board, size_t zone, size_t index, double *x, double *y);
// Top-level gr_line drawings off Edge.Cuts (net 0) and board texts.
size_t cc_graphic_count(const cc_board *board);
cc_track cc_graphic_at(const cc_board *board, size_t index);
size_t cc_text_count(const cc_board *board);
cc_board_text cc_text_at(const cc_board *board, size_t index);
size_t cc_pad_count(const cc_board *board);
cc_pad cc_pad_at(const cc_board *board, size_t index);
// A net's tracks solved as resistors between `pins` (indices of cc_pad_at on that net; tracks'
// conductance is width / length, vias, pads and filled zones join without resistance).
// Each edge has one current per pin: for 1 A entering at that pin and leaving at the first
// (positive x0,y0 to x1,y1). NULL with `error` set when the pins aren't joined by copper.
typedef struct cc_net_flow cc_net_flow;
typedef struct { double x0, y0, x1, y1, width; char layer[32]; } cc_flow_edge;
cc_net_flow *cc_net_flow_make(const cc_board *board, int net, const size_t *pins, size_t pin_count,
                              char *error, size_t error_size);
void cc_net_flow_free(cc_net_flow *flow);
size_t cc_net_flow_edge_count(const cc_net_flow *flow);
cc_flow_edge cc_net_flow_edge_at(const cc_net_flow *flow, size_t index);
double cc_net_flow_unit(const cc_net_flow *flow, size_t edge, size_t pin);
int cc_move_footprint_file(const char *utf8, const char *reference, double x, double y,
                           double rotation, const char *path, char *error, size_t error_size);
cc_settings cc_default_settings(void);
// kind: 0 tray, 1 board preview, 2 standoffs, 3 floor, 4 walls,
// 5 top rim, 6 separate lid. Returns required triangle count.
size_t cc_model_triangles(const cc_board *board, cc_settings settings, int kind,
                          cc_triangle *output, size_t capacity, char *error, size_t error_size);
int cc_export_stl(const cc_board *board, cc_settings settings, const char *path,
                  char *error, size_t error_size);
int cc_export_lid_stl(const cc_board *board, cc_settings settings, const char *path,
                      char *error, size_t error_size);
int cc_step_available(void);
int cc_export_step(const cc_board *board, cc_settings settings, const char *path,
                   char *error, size_t error_size);
#ifdef __cplusplus
}
#endif

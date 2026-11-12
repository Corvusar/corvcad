#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// KiCad's own board model (port gate 5): DRC and zone fill on a .kicad_pcb, loaded through
// kicad-cli's loader. The last board loaded stays open between calls and is reloaded when its
// file changes, unless the file now holds the text the last call returned. Calls run on one
// thread at a time. The NATIVE_KICAD edition implements these in
// integrations/kicad_core/session/kicad_board.cpp; the compatible edition (board_native_c_api.cpp)
// reports no backend and fails every call. Returned text is freed with cc_free_text.

// "kicad-10.0.6-native", or NULL when KiCad's board code is not linked.
const char *cc_board_native_backend(void);
// DRC as `kicad-cli pcb drc --format json --units mm` (errors and warnings, no schematic
// parity), optionally after refilling every zone (`--refill-zones`). Returns DRC_REPORT's JSON.
char *cc_board_native_drc(const char *board_path, int refill_zones, char *error, size_t capacity);
// Refills every zone as `kicad-cli pcb drc --refill-zones --save-board` does and returns the
// board file text; nothing is written.
char *cc_board_native_fill_zones(const char *board_path, char *error, size_t capacity);
// KiCad's Update PCB from Schematic (BOARD_NETLIST_UPDATER) with a KiCad netlist, for example
// cc_sch_session_netlist's: adds missing footprints from the footprint libraries, sets pad
// nets, values and fields. Footprints are matched by reference; nothing is deleted, and a
// footprint whose library ID changed is reported, not exchanged. Returns the board text
// (nothing is written) and puts KiCad's report, one line per action, in `report`.
// KiCad's ratsnest: one line per unrouted connection, "x0 y0 x1 y1 net name" in millimetres
// (the net name may contain spaces). Empty text when everything is routed.
char *cc_board_native_ratsnest(const char *board_path, char *error, size_t capacity);
// KiCad's effective netclass for every net of the board: one line per net,
// "track_width via_diameter via_drill clearance net name" in millimetres (the net name may
// contain spaces; the line with an empty name is the Default netclass).
char *cc_board_native_net_rules(const char *board_path, char *error, size_t capacity);
char *cc_board_native_update_from_netlist(const char *board_path, const char *netlist, char *report,
                                          size_t report_capacity, char *error, size_t capacity);
// Fabrication outputs written into `directory` (created; start from an empty one), as
// kicad-cli 10.0.6 writes them: Gerbers of the board's plot settings with the Gerber job file
// (`pcb export gerbers --board-plot-params`, `--check-zones` when refill_zones), Excellon drill
// files in mm, PTH and NPTH apart (`pcb export drill --format excellon --drill-origin absolute
// --excellon-units mm --excellon-zeros-format decimal --excellon-separate-th`), and the
// placement of both sides as <board>-pos.csv (`pcb export pos --format csv --units mm --side
// both`). A refill is not saved to the board. Returns the written file names, one per line.
char *cc_board_native_fab_outputs(const char *board_path, const char *directory, int refill_zones, char *error,
                                  size_t capacity);
// KiCad's Plot with the board's plot settings and layers (as the Gerbers above) into `directory`:
// format 0 a PDF, one page per layer, <board>.pdf; format 1 one SVG per layer,
// <board>-<layer>.svg. Returns the written file names, one per line.
char *cc_board_native_plot(const char *board_path, const char *directory, int format, char *error, size_t capacity);
// Board Setup as tab-separated lines, millimetres:
//   layers <copper layer count>
//   rule <name> <value>   min_clearance, min_track_width, min_annular_width, min_via_diameter,
//                         min_through_hole, copper_edge_clearance, hole_clearance, hole_to_hole
//   class <name> <clearance> <track> <via diameter> <via drill> <pair width> <pair gap>
//                         the Default netclass first
//   assign <pattern> <netclass>   netclass pattern assignments (KiCad wildcards or regex)
char *cc_board_native_board_setup(const char *board_path, char *error, size_t capacity);
// Applies a Board Setup in that format, as KiCad's Board Setup dialog does: the listed rules,
// every netclass and assignment replaced (the Default netclass's sizes set), the copper layer
// count (removing layers is refused while tracks or zones use them). Returns the board text and
// puts the project file text in `*project_text` (rules and netclasses live in the .kicad_pro);
// both are freed with cc_free_text and nothing is written.
char *cc_board_native_apply_board_setup(const char *board_path, const char *setup, char **project_text, char *error,
                                        size_t capacity);

// KiCad's interactive router (PNS) on the open board, one route or drag at a time, driven as
// the board editor's router tool drives it. Millimetres. Mode: 0 highlight collisions,
// 1 shove, 2 walk around. Starting on another board ends this one.
//
// Starts at (x, y). Kind 0, a track (Route Single Track): from the pad, via or track end there
// (within 1 mm), with its net and netclass width; `layer` ("F.Cu") when the start is on it or
// on no single layer. Kind 1, a differential pair (Route Differential Pair): from a pad or
// track of a pair's net, with the pair's width and gap. Kinds 2-4 tune an existing track
// there (Tune Length, Tune Differential Pair Length, Tune Skew) to `target` millimetres of
// length (2, 3) or skew (4): move along the track to extend the meanders, fix to commit them.
// 1 or 0 (error).
int cc_board_native_route_start(const char *board_path, double x, double y, const char *layer, int mode,
                                int kind, double target, char *error, size_t capacity);
// Drags the track or via at (x, y) (within 1 mm), as the editor's drag: move to drag, fix to
// drop (1, done: call finish), finish to save. 1 or 0 (error).
int cc_board_native_drag_start(const char *board_path, double x, double y, int mode, char *error,
                               size_t capacity);
// Moves the route's end toward (x, y) and returns the router's preview, one item per line:
//   "end x y layer"                     where the route ends now, on that layer
//   "fail reason"                       why the router could not follow (optional)
//   "track layer width x0 y0 x1 y1 ..." the routed head or a shoved track, as a polyline
//   "via x y diameter"
//   "hide uuid"                         a board track or via the preview replaces
//   "tuning length status"              tuning: the tuned length or skew; short, long or tuned
char *cc_board_native_route_move(double x, double y, char *error, size_t capacity);
// Fixes the route up to (x, y), as a click: 1 when it reached a pad, via or track of its net
// (call finish), 0 when it continues from there, -1 when it cannot be fixed there (error).
int cc_board_native_route_fix(double x, double y, char *error, size_t capacity);
// Toggles a via at the route's end; the next fix places it and continues on the other layer of
// F.Cu/B.Cu. Returns 1 when a via will be placed.
int cc_board_native_route_toggle_via(void);
// Ends the route: commits everything fixed through KiCad's commit and returns the board text
// (nothing is written), or NULL when nothing was fixed.
char *cc_board_native_route_finish(char *error, size_t capacity);
// Abandons the route; the board is unchanged.
void cc_board_native_route_cancel(void);

// After the project's library tables or library files changed (cc_import_project_library and
// the other library calls): KiCad rebuilds the open project's library tables and reloads its
// project libraries on next use, for the schematic session and the board alike. A no-op in the
// compatible edition.
void cc_board_native_reload_libraries(void);
// Board edits with KiCad's model (NATIVE_KICAD; otherwise unsupported), one tab-separated
// request, the board's new text back (free with cc_free_text):
//   "flip\t<reference>"                       a footprint to the other side (left-right)
//   "track\t<uuid>\t<width mm>\t<layer>"      a track or arc's width and copper layer
//   "dimension\t<layer>\t<x0>\t<y0>\t<x1>\t<y1>\t<height>"  an aligned dimension, millimetres
//   "place\t<Library:Footprint>\t<reference>\t<x>\t<y>"  a library footprint placed on F.Cu
//   "swap\t<reference>\t<Library:Footprint>"  KiCad's Change Footprint: reference, nets (by pad),
//    position, side and orientation kept, the rest from the library
//   "zone\t<uuid>\t<net or empty>\t<priority>\t<clearance>\t<min width>\t<solid|thermal|tht_thermal|none>\t
//    <thermal gap>\t<spoke width>\t<solid|hatched>"  a copper zone's properties; it is unfilled
//    (refill it with cc_board_native_fill_zones)
char *cc_board_native_edit(const char *board_path, const char *request, char *error, size_t capacity);
// KiCad's board import (NATIVE_KICAD): a non-KiCad board (Altium, CircuitMaker, CircuitStudio,
// Eagle, EasyEDA, EasyEDA Pro, CADSTAR, PADS, P-CAD, Fabmaster, SolidWorks PCB) saved as a KiCad
// board at `output`, in a project whose .kicad_pro exists; its footprints go to the project's
// "<project>-import-fps" library. `format` is "auto" or one of kicad-cli pcb import's names plus
// circuitmaker, circuitstudio, easyeda, easyedapro. Returns
// "<format>\t<footprints>\t<tracks>\t<vias>\t<zones>\t<importer messages>".
char *cc_board_native_import(const char *input, const char *format, const char *output, char *error, size_t capacity);

#ifdef __cplusplus
}
#endif

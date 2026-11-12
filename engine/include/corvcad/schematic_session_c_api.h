#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_sch_session cc_sch_session;
typedef struct cc_sch_transaction cc_sch_transaction;
enum cc_sch_error_code { CC_SCH_OK = 0, CC_SCH_INVALID = 1, CC_SCH_NOT_FOUND = 2,
                         CC_SCH_CONFLICT = 3, CC_SCH_UNSUPPORTED = 4, CC_SCH_IO = 5 };
enum cc_sch_capability { CC_SCH_TRANSACTIONS = 1, CC_SCH_HIERARCHY = 2,
                         CC_SCH_SOURCE_PRESERVING = 4, CC_SCH_NATIVE_KICAD = 8,
                         CC_SCH_ERC = 16, CC_SCH_NETLIST = 32, CC_SCH_LIBRARY = 64,
                         CC_SCH_KICAD_TOOLS = 128, CC_SCH_PLOT = 256, CC_SCH_SIMULATE = 512 };
typedef struct { int code; char *message; } cc_sch_result;

// Result structs must be zero-initialized and cleared when no longer needed. All returned
// UTF-8 strings are owned by the caller and released with cc_free_text.
void cc_sch_result_clear(cc_sch_result *result);
int cc_sch_api_version(void); // Currently 11 (10 plus KiCad's simulator at the end).
cc_sch_session *cc_sch_session_open(const char *project_or_schematic, cc_sch_result *result);
void cc_sch_session_free(cc_sch_session *session);
const char *cc_sch_session_backend(const cc_sch_session *session);
unsigned cc_sch_session_capabilities(const cc_sch_session *session);
// A JSON snapshot contains sheet instance paths, UUIDs and supported items.
// It deliberately reports the compatible backend and no KiCad ERC capability.
char *cc_sch_session_snapshot_json(const cc_sch_session *session, cc_sch_result *result);
cc_sch_transaction *cc_sch_transaction_begin(cc_sch_session *session, cc_sch_result *result);
int cc_sch_transaction_move_symbol(cc_sch_transaction *transaction, const char *sheet_path,
                                   const char *uuid, double x, double y, cc_sch_result *result);
int cc_sch_transaction_set_property(cc_sch_transaction *transaction, const char *sheet_path,
                                    const char *uuid, const char *property, const char *value,
                                    cc_sch_result *result);
int cc_sch_transaction_rename_symbol(cc_sch_transaction *transaction, const char *sheet_path,
                                     const char *uuid, const char *reference, cc_sch_result *result);
int cc_sch_transaction_delete_item(cc_sch_transaction *transaction, const char *sheet_path,
                                   const char *uuid, cc_sch_result *result);
int cc_sch_transaction_add_wire(cc_sch_transaction *transaction, const char *sheet_path,
                                double x0, double y0, double x1, double y1, cc_sch_result *result);
int cc_sch_transaction_add_label(cc_sch_transaction *transaction, const char *sheet_path,
                                 const char *name, double x, double y, cc_sch_result *result);
// Version 2: touch authoring. add_two_pin places CorvCAD's two-pin part with a
// project-unique reference; add_routed_wire writes the pin-avoiding orthogonal route.
int cc_sch_transaction_add_two_pin(cc_sch_transaction *transaction, const char *sheet_path,
                                   double x, double y, cc_sch_result *result);
int cc_sch_transaction_add_routed_wire(cc_sch_transaction *transaction, const char *sheet_path,
                                       double x0, double y0, double x1, double y1,
                                       cc_sch_result *result);
int cc_sch_transaction_move_label(cc_sch_transaction *transaction, const char *sheet_path,
                                  const char *uuid, double x, double y, cc_sch_result *result);
int cc_sch_transaction_rename_label(cc_sch_transaction *transaction, const char *sheet_path,
                                    const char *uuid, const char *name, cc_sch_result *result);
int cc_sch_transaction_commit(cc_sch_transaction *transaction, cc_sch_result *result);
void cc_sch_transaction_free(cc_sch_transaction *transaction); // aborts an open transaction
int cc_sch_session_can_undo(const cc_sch_session *session);
int cc_sch_session_can_redo(const cc_sch_session *session);
// Current text of one sheet instance's file (free with cc_free_text).
char *cc_sch_session_sheet_source(const cc_sch_session *session, const char *sheet_path,
                                  cc_sch_result *result);
// Planned add_routed_wire route as x,y pairs (up to `capacity` points); returns the
// point count, 0 when no clear route exists.
size_t cc_sch_session_route(const cc_sch_session *session, const char *sheet_path,
                            double x0, double y0, double x1, double y1,
                            double *xy, size_t capacity, cc_sch_result *result);
int cc_sch_session_undo(cc_sch_session *session, cc_sch_result *result);
int cc_sch_session_redo(cc_sch_session *session, cc_sch_result *result);
// File-version and structural checks only; this is not KiCad ERC.
int cc_sch_session_validate_structure(cc_sch_session *session, cc_sch_result *result);
int cc_sch_session_save(cc_sch_session *session, cc_sch_result *result);
int cc_sch_session_save_copy(cc_sch_session *session, const char *new_directory,
                             cc_sch_result *result);
// Version 3: KiCad's own checks on the current (edited) state, available only with the
// CC_SCH_NETLIST and CC_SCH_ERC capabilities; otherwise CC_SCH_UNSUPPORTED and NULL.
// The netlist is KiCad's s-expression format; the ERC report is the JSON that
// `kicad-cli sch erc --format json` writes (errors and warnings, millimetres).
char *cc_sch_session_netlist(cc_sch_session *session, cc_sch_result *result);
char *cc_sch_session_erc_json(cc_sch_session *session, cc_sch_result *result);
// Version 4: turns a placed symbol 90 degrees counter-clockwise about its origin, as KiCad's
// rotate does, fields included. Attached wires stay where they are.
int cc_sch_transaction_rotate_symbol(cc_sch_transaction *transaction, const char *sheet_path,
                                     const char *uuid, cc_sch_result *result);
// Version 5: places unit 1 of any library symbol ("Library:Name", from the project's or the
// bundled libraries) with add_two_pin's placement rules and the first free reference for the
// symbol's own prefix. Only with CC_SCH_LIBRARY; otherwise CC_SCH_UNSUPPORTED.
int cc_sch_transaction_add_library_symbol(cc_sch_transaction *transaction, const char *sheet_path,
                                          const char *lib_id, double x, double y, cc_sch_result *result);
// Version 6: KiCad's schematic editing tools, only with CC_SCH_KICAD_TOOLS; otherwise
// CC_SCH_UNSUPPORTED. Millimetres.
// Annotate (the whole hierarchy, sorted by X, first free number): keeps existing references,
// or renumbers every symbol when `reset`.
int cc_sch_transaction_annotate(cc_sch_transaction *transaction, int reset, cc_sch_result *result);
// A bus segment, and a wire-to-bus entry (2.54 mm) from (x, y) on a bus toward quadrant 1
// up-right, 2 down-right, 3 down-left or 4 up-left.
int cc_sch_transaction_add_bus(cc_sch_transaction *transaction, const char *sheet_path, double x0, double y0,
                               double x1, double y1, cc_sch_result *result);
int cc_sch_transaction_add_bus_entry(cc_sch_transaction *transaction, const char *sheet_path, double x, double y,
                                     int quadrant, cc_sch_result *result);
// A label of kind 0 local, 1 global or 2 hierarchical; bus names such as D[0..7] work in each.
int cc_sch_transaction_add_scoped_label(cc_sch_transaction *transaction, const char *sheet_path, const char *name,
                                        double x, double y, int kind, cc_sch_result *result);
// A hierarchical sheet box (top left x, y) with a new file <name>.kicad_sch in the project
// folder, written by the next save; the name must be new and usable as a file name.
int cc_sch_transaction_add_sheet(cc_sch_transaction *transaction, const char *sheet_path, const char *name, double x,
                                 double y, double width, double height, cc_sch_result *result);
// Sheet pins for the hierarchical labels of the sheet (by its UUID on `sheet_path`) that have
// none, on its left edge, as KiCad's Import Sheet Pin.
int cc_sch_transaction_add_sheet_pins(cc_sch_transaction *transaction, const char *sheet_path, const char *sheet_uuid,
                                      cc_sch_result *result);
// The BOM as `kicad-cli sch export bom --group-by Value,Footprint --ref-range-delimiter -`
// writes it: "Refs","Value","Footprint","Qty","DNP", grouped by value and footprint.
char *cc_sch_session_bom_csv(cc_sch_session *session, cc_sch_result *result);
// Version 7: KiCad's schematic plot as `kicad-cli sch export pdf|svg` writes it with its
// defaults (all sheets, drawing sheet, default theme). Format 0 writes one PDF file at
// `output`; format 1 writes one SVG per sheet into the existing directory `output`. Returns 1,
// or 0 with CC_SCH_UNSUPPORTED without CC_SCH_PLOT.
int cc_sch_session_plot(cc_sch_session *session, int format, const char *output, cc_sch_result *result);
// Version 8, with CC_SCH_KICAD_TOOLS (otherwise CC_SCH_UNSUPPORTED). Millimetres.
// A no-connect flag at (x, y), normally on a pin end.
int cc_sch_transaction_add_no_connect(cc_sch_transaction *transaction, const char *sheet_path, double x, double y,
                                      cc_sch_result *result);
// Mirrors a placed symbol about its origin as KiCad's editor does: axis 0 top to bottom (Mirror
// Vertically, `mirror x`), 1 left to right (Mirror Horizontally, `mirror y`).
int cc_sch_transaction_mirror_symbol(cc_sch_transaction *transaction, const char *sheet_path, const char *uuid,
                                     int axis, cc_sch_result *result);
// A free text note anchored at (x, y), and a graphic note line; neither joins any net.
int cc_sch_transaction_add_text(cc_sch_transaction *transaction, const char *sheet_path, const char *text, double x,
                                double y, cc_sch_result *result);
int cc_sch_transaction_add_note_line(cc_sch_transaction *transaction, const char *sheet_path, double x0, double y0,
                                     double x1, double y1, cc_sch_result *result);
// Version 9, with CC_SCH_KICAD_TOOLS. A bus-to-bus entry, quadrants as add_bus_entry (KiCad saves it
// as a 2.54 mm bus segment, so it reads back as a bus); a sheet
// box resized with its top-left corner kept (at least 5.08 mm and what its pins need). With
// KiCad's tools cc_sch_transaction_delete_item also deletes a sheet from the hierarchy (its file
// stays in the project folder, as in KiCad).
int cc_sch_transaction_add_bus_bus_entry(cc_sch_transaction *transaction, const char *sheet_path, double x, double y,
                                         int quadrant, cc_sch_result *result);
int cc_sch_transaction_resize_sheet(cc_sch_transaction *transaction, const char *sheet_path, const char *sheet_uuid,
                                    double width, double height, cc_sch_result *result);
// Version 10, the NATIVE_KICAD backend only (otherwise CC_SCH_UNSUPPORTED and NULL). KiCad's import
// of a non-KiCad schematic (Altium, Eagle, EasyEDA, EasyEDA Pro, CADSTAR, PADS, LTspice, gEDA;
// `format` "auto" or one of those names in lower case), saved as KiCad files with the root at
// `output` in a project whose .kicad_pro exists; no session may be open. Footprint fields point
// at the project's "<project>-import-fps" library, which cc_board_native_import fills. Returns
// "<format>\t<sheets>\t<symbols>\t<importer messages>".
char *cc_sch_import(const char *input, const char *format, const char *output, cc_sch_result *result);
// Version 11, with CC_SCH_SIMULATE (the NATIVE_KICAD backend built with ngspice; otherwise
// CC_SCH_UNSUPPORTED and NULL). KiCad's simulator runs one analysis on the current state, as its
// Simulator window does: the project's ngspice settings (LTspice and PSpice compatibility when
// the project has no simulator workbook), KiCad's SPICE netlist with its default options, ngspice
// 46 with its XSPICE code models. `command` is the analysis, such as ".tran 1u 10m" (several lines
// allowed), optionally with a Simulator tab's option lines as KiCad's workbook (.wbk) stores them
// (".kicad adjustpaths", ".save all", ".probe alli", ".probe allp", ".kicad esavenone"): with any
// of those, the netlist options are the ones listed, otherwise a new tab's defaults (which save
// all currents and power; ngspice refuses that in circuits with digital nodes). No analysis line
// uses the schematic's own simulation directive. Blocks until the run ends; one simulation at a
// time in the process (CC_SCH_CONFLICT).
// `max_points` > 0 keeps every n-th point and the last, about that many; 0 keeps all. Returns
// "<analysis>\t<plot>\t<points>\t<kept>\n", then one line per vector, sorted by name:
// "<name>\t<real|complex>\t<v1> <v2> ...", each complex value written as <real>,<imaginary>.
// A run without results is CC_SCH_INVALID, with ngspice's output in the message.
char *cc_sch_session_simulate(cc_sch_session *session, const char *command, size_t max_points,
                              cc_sch_result *result);
// The SPICE netlist cc_sch_session_simulate would hand ngspice for `command` (KiCad's exporter,
// as its Simulator shows it). Same capability and `command` rules.
char *cc_sch_session_spice_netlist(cc_sch_session *session, const char *command, cc_sch_result *result);
// Stops the simulation in progress, from any thread (cc_sch_session_simulate then returns the
// points so far). 1 if one was running; always 0 without CC_SCH_SIMULATE.
int cc_sch_simulate_stop(void);

#ifdef __cplusplus
}
#endif

// Compatible edition: KiCad's board code is not linked. The NATIVE_KICAD edition replaces this
// file with integrations/kicad_core/session/kicad_board.cpp.
#include "corvcad/board_native_c_api.h"
#include <cstdio>

namespace {
char *unsupported(char *error, size_t capacity) {
    if (error && capacity) std::snprintf(error, capacity, "KiCad's board code is not in this edition");
    return nullptr;
}
} // namespace

const char *cc_board_native_backend(void) {return nullptr;}
char *cc_board_native_drc(const char *, int, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_fill_zones(const char *, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_ratsnest(const char *, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_net_rules(const char *, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_update_from_netlist(const char *, const char *, char *report, size_t report_capacity,
                                          char *error, size_t capacity) {
    if (report && report_capacity) *report = 0;
    return unsupported(error, capacity);
}
char *cc_board_native_fab_outputs(const char *, const char *, int, char *error, size_t capacity) {
    return unsupported(error, capacity);
}
char *cc_board_native_plot(const char *, const char *, int, char *error, size_t capacity) {
    return unsupported(error, capacity);
}
char *cc_board_native_board_setup(const char *, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_edit(const char *, const char *, char *error, size_t capacity) {return unsupported(error, capacity);}
char *cc_board_native_import(const char *, const char *, const char *, char *error, size_t capacity) {
  return unsupported(error, capacity);
}
char *cc_board_native_apply_board_setup(const char *, const char *, char **project_text, char *error,
                                        size_t capacity) {
    if (project_text) *project_text = nullptr;
    return unsupported(error, capacity);
}
int cc_board_native_route_start(const char *, double, double, const char *, int, int, double, char *error,
                                size_t capacity) {
    unsupported(error, capacity);
    return 0;
}
int cc_board_native_drag_start(const char *, double, double, int, char *error, size_t capacity) {
    unsupported(error, capacity);
    return 0;
}
char *cc_board_native_route_move(double, double, char *error, size_t capacity) {return unsupported(error, capacity);}
int cc_board_native_route_fix(double, double, char *error, size_t capacity) {
    unsupported(error, capacity);
    return -1;
}
int cc_board_native_route_toggle_via(void) {return 0;}
char *cc_board_native_route_finish(char *error, size_t capacity) {return unsupported(error, capacity);}
void cc_board_native_route_cancel(void) {}
void cc_board_native_reload_libraries(void) {}

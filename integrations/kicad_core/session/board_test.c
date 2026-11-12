/* SPDX-License-Identifier: MIT */
// Drives the native board checks through engine/include/corvcad/board_native_c_api.h alone.
// Linked with the schematic session and both measured closures, as the NATIVE_KICAD app links
// them; session/board_check.sh compares its output with kicad-cli.
//   board_test <in.kicad_pcb> <out-dir>
// Writes drc.json, drc-refilled.json and filled.kicad_pcb, and prints timings as JSON.
#include <corvcad/board_native_c_api.h>
#include <corvcad/schematic_session_c_api.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int write_text(const char *dir, const char *name, char *text, const char *error)
{
    char path[4096];
    FILE *out;
    if (!text) {
        fprintf(stderr, "%s: %s\n", name, error);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    out = fopen(path, "wb");
    if (!out || fputs(text, out) < 0 || fclose(out)) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    free(text); // cc_free_text is std::free; the board test links no engine
    return 0;
}

static char *read_text(const char *path)
{
    FILE *in = fopen(path, "rb");
    char *text;
    long size;
    if (!in || fseek(in, 0, SEEK_END) || (size = ftell(in)) < 0 || fseek(in, 0, SEEK_SET)) return NULL;
    text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, in) != (size_t)size) return NULL;
    text[size] = 0;
    fclose(in);
    return text;
}

// board_test update <in.kicad_pcb> <netlist> <out-dir>: Update PCB from Schematic; writes
// updated.kicad_pcb and report.txt.
static int update(const char *board, const char *netlist_path, const char *dir)
{
    char error[512] = "", report[16384] = "";
    char *netlist = read_text(netlist_path);
    int failed;
    if (!netlist) {
        fprintf(stderr, "cannot read %s\n", netlist_path);
        return 1;
    }
    failed = write_text(dir, "updated.kicad_pcb",
                        cc_board_native_update_from_netlist(board, netlist, report, sizeof report, error, sizeof error),
                        error);
    free(netlist);
    printf("%s", report);
    return failed | write_text(dir, "report.txt", strdup(report), "");
}

static int count(const char *text, const char *needle)
{
    int n = 0;
    for (const char *at = text ? strstr(text, needle) : NULL; at; at = strstr(at + 1, needle)) ++n;
    return n;
}

// board_test symbol <project> <Library:Name>: places the library symbol on the root sheet at the
// first free grid spot through the session (API 5) and checks that KiCad's netlist lists a part
// from that library. Nothing is saved.
static int place_symbol(const char *project, const char *lib_id)
{
    cc_sch_result result = {0, NULL};
    cc_sch_session *session = cc_sch_session_open(project, &result);
    char root[256] = "", lib[128] = "", part[128] = "", want[512];
    char *json, *netlist;
    const char *path, *colon = strchr(lib_id, ':');
    int placed = 0, before, after, parts;
    if (!session || !colon) {
        fprintf(stderr, "open: %s\n", result.message ? result.message : "bad Library:Name");
        return 1;
    }
    json = cc_sch_session_snapshot_json(session, &result);
    path = json ? strstr(json, "\"path\":\"") : NULL;
    if (!path) return 1;
    path += 8;
    memcpy(root, path, strcspn(path, "\""));
    free(json);
    netlist = cc_sch_session_netlist(session, &result);
    // KiCad writes one field per line: count "(comp" (less "(components") and "(part "Name")".
    snprintf(want, sizeof want, "(part \"%s\")", colon + 1);
    before = count(netlist, "(comp") - count(netlist, "(components");
    parts = count(netlist, want);
    free(netlist);
    cc_sch_transaction *t = cc_sch_transaction_begin(session, &result);
    for (int row = 0; row < 6 && !placed; ++row)
        for (int col = 0; col < 18 && !placed; ++col)
            placed = cc_sch_transaction_add_library_symbol(t, root, lib_id, 25.4 + col * 12.7, 182.88 - row * 12.7, &result);
    if (!placed || !cc_sch_transaction_commit(t, &result)) {
        fprintf(stderr, "place %s: %s\n", lib_id, result.message ? result.message : "no free spot");
        return 1;
    }
    cc_sch_transaction_free(t);
    netlist = cc_sch_session_netlist(session, &result);
    snprintf(lib, sizeof lib, "%.*s", (int)(colon - lib_id), lib_id);
    snprintf(part, sizeof part, "%s", colon + 1);
    after = count(netlist, "(comp") - count(netlist, "(components");
    placed = after == before + 1 && count(netlist, want) > parts;
    printf("placed %s: KiCad's netlist has %d components (was %d), %d from it (was %d): %s\n",
           lib_id, after, before, count(netlist, want), parts, placed ? "ok" : "WRONG");
    free(netlist);
    cc_sch_session_free(session);
    return !placed;
}

// Places `lib_id` at the first free grid spot of the root sheet in one transaction; 1 when placed.
static int place_at_free_spot(cc_sch_session *session, const char *root, const char *lib_id)
{
    cc_sch_result result = {0, NULL};
    cc_sch_transaction *t = cc_sch_transaction_begin(session, &result);
    int placed = 0;
    for (int row = 0; row < 6 && !placed; ++row)
        for (int col = 0; col < 18 && !placed; ++col)
            placed = cc_sch_transaction_add_library_symbol(t, root, lib_id, 25.4 + col * 12.7, 182.88 - row * 12.7, &result);
    placed = placed && cc_sch_transaction_commit(t, &result);
    cc_sch_transaction_free(t);
    cc_sch_result_clear(&result);
    return placed;
}

// board_test library <copy of a project's .kicad_pro> <tables-dir>: the project's tables lack the
// generated CorvLib library (engine/tests/test_library.cpp); <tables-dir> holds the tables with
// it, as the app's library import writes them. Placing CorvLib:Chip fails; after the tables are
// copied in and cc_board_native_reload_libraries, Chip and Conn place, KiCad's netlist carries
// their CorvLib footprints, Update PCB adds both footprints from the project's CorvLib.pretty
// with all their pads, and DRC runs on the result.
static int library_reload(const char *project, const char *tables)
{
    cc_sch_result result = {0, NULL};
    cc_sch_session *session = cc_sch_session_open(project, &result);
    char root[256] = "", folder[4096], path[4096], board[4096], error[512] = "", report[16384] = "";
    const char *at;
    char *json, *netlist, *updated, *drc;
    int before, after, ok;
    if (!session) {
        fprintf(stderr, "open: %s\n", result.message ? result.message : "failed");
        return 1;
    }
    json = cc_sch_session_snapshot_json(session, &result);
    at = json ? strstr(json, "\"path\":\"") : NULL;
    if (!at) return 1;
    memcpy(root, at + 8, strcspn(at + 8, "\""));
    free(json);
    before = place_at_free_spot(session, root, "CorvLib:Chip");
    snprintf(folder, sizeof folder, "%.*s", (int)(strrchr(project, '/') - project), project);
    for (int i = 0; i < 2; ++i) {
        const char *name = i ? "fp-lib-table" : "sym-lib-table";
        snprintf(path, sizeof path, "%s/%s", tables, name);
        char *text = read_text(path);
        if (!text || write_text(folder, name, text, "unreadable table")) return 1;
    }
    cc_board_native_reload_libraries();
    after = place_at_free_spot(session, root, "CorvLib:Chip") && place_at_free_spot(session, root, "CorvLib:Conn");
    printf("library reload: CorvLib:Chip %s before the tables changed, %s after reload\n",
           before ? "PLACED" : "not found", after ? "Chip and Conn placed" : "NOT PLACED");
    netlist = cc_sch_session_netlist(session, &result);
    ok = !before && after && netlist && strstr(netlist, "\"CorvLib:Chip_SOIC\"") && strstr(netlist, "\"CorvLib:Header_1x04\"");
    printf("  netlist footprints: %s\n", ok ? "CorvLib:Chip_SOIC and CorvLib:Header_1x04" : "MISSING");
    snprintf(board, sizeof board, "%.*s.kicad_pcb", (int)(strrchr(project, '.') - project), project);
    updated = netlist ? cc_board_native_update_from_netlist(board, netlist, report, sizeof report, error, sizeof error) : NULL;
    free(netlist);
    if (!updated) {
        fprintf(stderr, "update: %s\n", error);
        return 1;
    }
    // Pads of the added footprints: 8 for the SOIC, 4 for the header.
    const char *soic = strstr(updated, "(footprint \"CorvLib:Chip_SOIC\""), *header = strstr(updated, "(footprint \"CorvLib:Header_1x04\"");
    int soic_pads = 0, header_pads = 0;
    for (const char *p = soic ? strstr(soic, "(pad ") : NULL; p && (!header || header < soic || p < header); p = strstr(p + 1, "(pad ")) ++soic_pads;
    for (const char *p = header ? strstr(header, "(pad ") : NULL; p && (!soic || soic < header || p < soic); p = strstr(p + 1, "(pad ")) ++header_pads;
    printf("  Update PCB: %s; Chip_SOIC %d pads, Header_1x04 %d pads\n", strtok(report, "\n") ? report : "", soic_pads,
           header_pads);
    ok = ok && soic_pads == 8 && header_pads == 4;
    ok = !write_text(folder, strrchr(board, '/') + 1, updated, "") && ok;
    drc = cc_board_native_drc(board, 0, error, sizeof error);
    printf("  DRC of the updated board: %s\n", drc ? "ran" : error);
    ok = ok && drc;
    free(drc);
    cc_sch_session_free(session);
    printf("library: %s\n", ok ? "ok" : "WRONG");
    return !ok;
}

// The sheet instance path whose sheet is named `name` in a session snapshot, or 0.
static int sheet_path_named(const char *json, const char *name, char *path, size_t capacity)
{
    char key[160];
    const char *at, *start = NULL;
    snprintf(key, sizeof key, "\"name\":\"%s\"", name);
    at = json ? strstr(json, key) : NULL;
    if (!at) return 0;
    for (const char *p = json; (p = strstr(p, "\"path\":\"")) && p < at; p += 8) start = p + 8;
    if (!start) return 0;
    snprintf(path, capacity, "%.*s", (int)strcspn(start, "\""), start);
    return 1;
}

// Distinct references across a session snapshot's sheets (units of a part share one), and how
// many of them are unannotated ("R?").
static int distinct_references(const char *json, int *unannotated)
{
    static char seen[4096][64];
    int n = 0;
    *unannotated = 0;
    for (const char *at = json ? strstr(json, "\"reference\":\"") : NULL; at; at = strstr(at + 1, "\"reference\":\"")) {
        char ref[64];
        int i, known = 0;
        snprintf(ref, sizeof ref, "%.*s", (int)strcspn(at + 13, "\""), at + 13);
        for (i = 0; i < n && !known; ++i) known = !strcmp(seen[i], ref);
        if (known || n == 4096) continue;
        snprintf(seen[n++], sizeof seen[0], "%s", ref);
        *unannotated += ref[0] && ref[strlen(ref) - 1] == '?';
    }
    return n;
}

// Library-table findings ("does not include the symbol library ...") in an ERC report.
static int library_findings(const char *project)
{
    cc_sch_result r = {0, NULL};
    cc_sch_session *s = cc_sch_session_open(project, &r);
    char *erc = s ? cc_sch_session_erc_json(s, &r) : NULL;
    int count = erc ? 0 : -1;
    for (const char *at = erc; at && (at = strstr(at, "does not include the")); ++at) ++count;
    if (!erc) fprintf(stderr, "ERC on %s: %s\n", project, r.message ? r.message : "failed");
    free(erc);
    cc_sch_session_free(s);
    cc_sch_result_clear(&r);
    return count;
}

// DRC on a project's board: 1 when it ran.
static int board_drc(const char *project)
{
    char board[4096], error[512], *dot, *report;
    snprintf(board, sizeof board, "%s", project);
    if ((dot = strrchr(board, '.'))) snprintf(dot, sizeof board - (size_t)(dot - board), ".kicad_pcb");
    report = cc_board_native_drc(board, 0, error, sizeof error);
    if (!report) fprintf(stderr, "DRC on %s: %s\n", board, error);
    free(report);
    return report != NULL;
}

// board_test projects <a.kicad_pro> <b.kicad_pro>: two projects in one process, as the app opens
// them one after the other. KiCad resolves libraries through its active project, so each must
// see its own tables: board_check.py projects runs both orders and compares B's library-table
// findings after A with B's own when first. Here: A's board is closed safely when B becomes
// active, a board still loads while another project's session is open, and A sees its own
// tables again afterwards.
static int project_switch(const char *a, const char *b)
{
    int first, second, ok;
    cc_sch_result r = {0, NULL};
    first = library_findings(a);
    ok = board_drc(a);
    second = library_findings(b); // closes A's cached board, makes B active
    cc_sch_session *held = cc_sch_session_open(b, &r);
    ok = ok && held && board_drc(a); // A loads beside the open session's project
    cc_sch_session_free(held);
    ok = ok && board_drc(b) && library_findings(a) == first;
    printf("projects: library-table findings %d (A), %d (B after A); boards across the switch %s\n",
           first, second, ok ? "ok" : "FAILED");
    cc_sch_result_clear(&r);
    return !(ok && first >= 0 && second >= 0);
}

// One committed and saved edit; prints the failure.
static int edit_ok(cc_sch_session *session, cc_sch_transaction *t, int ok, const char *what)
{
    cc_sch_result result = {0, NULL};
    ok = ok && cc_sch_transaction_commit(t, &result) && cc_sch_session_save(session, &result);
    if (!ok) fprintf(stderr, "%s: %s\n", what, result.message ? result.message : "failed");
    cc_sch_transaction_free(t);
    return ok;
}

// A check that names itself on stderr when it fails.
#define EXPECT(condition) ((condition) ? 1 : (fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #condition), 0))

// The number after `"key"`, skipping the colon and white space (KiCad pretty-prints its JSON).
static int json_number(const char *from, const char *key, double *value)
{
    char quoted[32];
    snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char *at = strstr(from, quoted);
    if (!at) return 0;
    at += strlen(quoted);
    while (*at == ' ' || *at == ':' || *at == '\n' || *at == '\r' || *at == '\t') ++at;
    return sscanf(at, "%lf", value) == 1;
}

// The first pin KiCad's ERC (its JSON report) finds unconnected on the root sheet ("path": "/"):
// the position of that violation's first item.
static int root_unconnected_pin(const char *erc, double *x, double *y)
{
    for (const char *type = erc; type && (type = strstr(type, "\"pin_not_connected\"")); ++type) {
        const char *sheet = NULL, *items = NULL, *p;
        for (p = erc; (p = strstr(p, "\"path\"")) && p < type; ++p) sheet = p;
        for (p = erc; (p = strstr(p, "\"items\"")) && p < type; ++p) items = p;
        if (!sheet || !items) continue;
        sheet += 6;
        while (*sheet == ' ' || *sheet == ':') ++sheet;
        const char *pos = strstr(items, "\"pos\"");
        if (strncmp(sheet, "\"/\"", 3) == 0 && pos && pos < type && json_number(pos, "x", x) && json_number(pos, "y", y))
            return 1;
    }
    return 0;
}

// Whether the placed symbol `uuid` in a sheet's source carries a (mirror ...) token: between the
// last (lib_id before its (uuid "...") and that uuid.
static int mirrored(const char *source, const char *uuid)
{
    char key[160];
    snprintf(key, sizeof key, "(uuid \"%s\")", uuid);
    const char *at = source ? strstr(source, key) : NULL, *lib = NULL, *p;
    if (!at) return -1;
    for (p = source; (p = strstr(p, "(lib_id")) && p < at; ++p) lib = p;
    if (!lib) return -1;
    p = strstr(lib, "(mirror ");
    return p && p < at;
}
// A placed symbol's UUID on sheet `path` from the session snapshot ({"id":"<path>:<uuid>",...
// "reference":...}).
static int symbol_on(const char *json, const char *path, char *uuid, size_t size)
{
    char id[600];
    snprintf(id, sizeof id, "{\"id\":\"%s:", path);
    for (const char *p = json; p && (p = strstr(p, id)); ++p) {
        const char *end = strchr(p, '}'), *ref = strstr(p, "\"reference\":");
        if (ref && end && ref < end) {
            p += strlen(id);
            snprintf(uuid, size, "%.*s", (int)strcspn(p, "\""), p);
            return 1;
        }
    }
    return 0;
}

// Session API 8 on the root sheet of a reopened project: a no-connect flag on a pin KiCad's ERC
// finds unconnected, a symbol mirrored left to right, a text note and a note line. After a save
// and a reopen the flag, mirror and notes are in the file and ERC no longer reports that pin.
// Writes the session's ERC and netlist (erc.json, netlist.net) beside the project copy for a
// kicad-cli comparison.
static int schematic_notes(const char *project, const char *dir, const char *root)
{
    cc_sch_result r = {0, NULL};
    cc_sch_session *s = cc_sch_session_open(project, &r);
    char *erc = s ? cc_sch_session_erc_json(s, &r) : NULL, *json, *source, symbol[128] = "", at[64];
    double x = 0, y = 0;
    int ok = s && erc, flagged;
    if (!ok) return 1;
    flagged = root_unconnected_pin(erc, &x, &y);
    free(erc);
    json = cc_sch_session_snapshot_json(s, &r);
    ok &= json && symbol_on(json, root, symbol, sizeof symbol);
    free(json);
    source = cc_sch_session_sheet_source(s, root, &r);
    int was_mirrored = mirrored(source, symbol);
    free(source);
    cc_sch_transaction *t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t,
                  (!flagged || cc_sch_transaction_add_no_connect(t, root, x, y, &r)) &&
                  cc_sch_transaction_mirror_symbol(t, root, symbol, 1, &r) &&
                  cc_sch_transaction_add_text(t, root, "CorvCAD note", 12.7, 12.7, &r) &&
                  cc_sch_transaction_add_note_line(t, root, 12.7, 15.24, 38.1, 15.24, &r),
                  "notes");
    t = cc_sch_transaction_begin(s, &r);
    ok &= !cc_sch_transaction_mirror_symbol(t, root, symbol, 2, &r) && r.code == CC_SCH_INVALID; // bad axis
    cc_sch_transaction_free(t);
    cc_sch_session_free(s);

    s = cc_sch_session_open(project, &r);
    source = s ? cc_sch_session_sheet_source(s, root, &r) : NULL;
    // KiCad toggles a symbol's mirror, and may write a turned symbol's as (mirror x).
    ok &= source && strstr(source, "(text \"CorvCAD note\"") && strstr(source, "(polyline") &&
          was_mirrored >= 0 && mirrored(source, symbol) == !was_mirrored;
    ok &= source && (!flagged || strstr(source, "(no_connect") != NULL);
    free(source);
    erc = s ? cc_sch_session_erc_json(s, &r) : NULL;
    double x2 = 0, y2 = 0;
    // The flagged pin is no longer reported (another unconnected pin may be).
    int still = erc && root_unconnected_pin(erc, &x2, &y2) && x2 == x && y2 == y;
    ok &= erc && !(flagged && still);
    free(erc);
    if (s) cc_sch_session_free(s);

    // API 9: the CorvChild sheet (from schematic_tools) resized, a bus-to-bus entry on its bus,
    // then the sheet deleted (undone and redone once); after a reopen it is gone from the
    // hierarchy and its file is still in the project folder, as KiCad leaves it.
    char child[512] = "", file[4096];
    const char *sheet_uuid = "";
    s = cc_sch_session_open(project, &r);
    json = s ? cc_sch_session_snapshot_json(s, &r) : NULL;
    int api9 = EXPECT(json && sheet_path_named(json, "CorvChild", child, sizeof child));
    free(json);
    if (api9) {
        sheet_uuid = strrchr(child, '/') + 1;
        t = cc_sch_transaction_begin(s, &r);
        api9 &= edit_ok(s, t, cc_sch_transaction_resize_sheet(t, root, sheet_uuid, 40.64, 25.4, &r) &&
                              cc_sch_transaction_add_bus_bus_entry(t, child, 88.9, 76.2, 1, &r),
                        "resize and bus-to-bus entry");
        t = cc_sch_transaction_begin(s, &r);
        api9 &= EXPECT(!cc_sch_transaction_resize_sheet(t, root, sheet_uuid, 1.0, 1.0, &r) && r.code == CC_SCH_INVALID);
        cc_sch_transaction_free(t);
        source = cc_sch_session_sheet_source(s, root, &r);
        api9 &= EXPECT(source && strstr(source, "(size 40.64 25.4)") != NULL);
        free(source);
        source = cc_sch_session_sheet_source(s, child, &r);
        // KiCad saves a bus-to-bus entry as a bus segment (SCH_IO_KICAD_SEXPR::saveBusEntry).
        api9 &= EXPECT(source && strstr(source, "(xy 88.9 76.2) (xy 91.44 73.66)") != NULL);
        free(source);
        t = cc_sch_transaction_begin(s, &r);
        api9 &= edit_ok(s, t, cc_sch_transaction_delete_item(t, root, sheet_uuid, &r), "delete sheet");
        json = cc_sch_session_snapshot_json(s, &r);
        api9 &= EXPECT(json && !sheet_path_named(json, "CorvChild", file, sizeof file));
        free(json);
        api9 &= EXPECT(cc_sch_session_undo(s, &r));
        json = cc_sch_session_snapshot_json(s, &r);
        api9 &= EXPECT(json && sheet_path_named(json, "CorvChild", file, sizeof file)); // undo restores it
        free(json);
        api9 &= EXPECT(cc_sch_session_redo(s, &r) && cc_sch_session_save(s, &r));
        cc_sch_session_free(s);
        s = cc_sch_session_open(project, &r);
        json = s ? cc_sch_session_snapshot_json(s, &r) : NULL;
        api9 &= EXPECT(json && !sheet_path_named(json, "CorvChild", file, sizeof file));
        free(json);
        snprintf(file, sizeof file, "%.*s/CorvChild.kicad_sch", (int)(strrchr(project, '/') - project), project);
        FILE *kept = fopen(file, "r");
        api9 &= EXPECT(kept != NULL);
        if (kept) fclose(kept);
    }
    ok &= api9;
    ok &= s && !write_text(dir, "erc.json", cc_sch_session_erc_json(s, &r), "erc") &&
          !write_text(dir, "netlist.net", cc_sch_session_netlist(s, &r), "netlist");
    snprintf(at, sizeof at, flagged ? "%.2f, %.2f" : "none", x, y);
    printf("schematic notes on %s: no-connect at %s, mirror, text and line read back; sheet resized, bus-to-bus entry, "
           "sheet deleted (undo, redo, reopen; file kept): %s\n", project, at, ok ? "ok" : "WRONG");
    if (s) cc_sch_session_free(s);
    return !ok;
}

// board_test schematic <copy of a project's .kicad_pro> <out-dir>: KiCad's schematic tools
// (API 6) on a copy: writes bom.csv and the plot (API 7), annotates, adds a hierarchical sheet with a hierarchical
// label, a bus, a bus entry and a bus label, imports the sheet pin, re-annotates everything,
// then reopens the project and checks the hierarchy, the pin, the component count and ERC.
static int schematic_tools(const char *project, const char *dir)
{
    cc_sch_result r = {0, NULL};
    cc_sch_session *s = cc_sch_session_open(project, &r);
    char root[256] = "", child[512] = "", *json, *text, *source;
    const char *path, *uuid;
    int ok = 1, before, after, unannotated = 0;
    if (!s) {
        fprintf(stderr, "open: %s\n", r.message ? r.message : "failed");
        return 1;
    }
    json = cc_sch_session_snapshot_json(s, &r);
    path = json ? strstr(json, "\"path\":\"") : NULL;
    if (!path) return 1;
    snprintf(root, sizeof root, "%.*s", (int)strcspn(path + 8, "\""), path + 8);
    free(json);
    if (write_text(dir, "bom.csv", cc_sch_session_bom_csv(s, &r), r.message ? r.message : "bom")) return 1;
    // API 7: the unedited schematic plotted to plot/schematic.pdf and one SVG per sheet in plot/.
    char plot[4096], pdf[4200];
    snprintf(plot, sizeof plot, "%s/plot", dir);
    snprintf(pdf, sizeof pdf, "%s/schematic.pdf", plot);
    mkdir(plot, 0755);
    if (!cc_sch_session_plot(s, 0, pdf, &r) || !cc_sch_session_plot(s, 1, plot, &r)) {
        fprintf(stderr, "plot: %s\n", r.message ? r.message : "failed");
        return 1;
    }
    json = cc_sch_session_snapshot_json(s, &r);
    before = distinct_references(json, &unannotated);
    free(json);

    cc_sch_transaction *t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t, cc_sch_transaction_annotate(t, 0, &r), "annotate");
    t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t, cc_sch_transaction_add_sheet(t, root, "CorvChild", 25.4, 25.4, 30.48, 20.32, &r), "add sheet");
    json = cc_sch_session_snapshot_json(s, &r);
    if (!sheet_path_named(json, "CorvChild", child, sizeof child)) {
        fprintf(stderr, "the new sheet is not in the hierarchy\n");
        return 1;
    }
    free(json);
    t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t,
                  cc_sch_transaction_add_scoped_label(t, child, "SIG", 50.8, 50.8, 2, &r) &&
                  cc_sch_transaction_add_bus(t, child, 50.8, 76.2, 101.6, 76.2, &r) &&
                  cc_sch_transaction_add_bus_entry(t, child, 63.5, 76.2, 2, &r) &&
                  cc_sch_transaction_add_scoped_label(t, child, "D[0..3]", 76.2, 76.2, 0, &r),
                  "child items");
    uuid = strrchr(child, '/') + 1;
    t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t, cc_sch_transaction_add_sheet_pins(t, root, uuid, &r), "sheet pins");
    t = cc_sch_transaction_begin(s, &r);
    ok &= edit_ok(s, t, cc_sch_transaction_annotate(t, 1, &r), "re-annotate");
    cc_sch_session_free(s);

    // A fresh session reads everything back from the files.
    s = cc_sch_session_open(project, &r);
    json = s ? cc_sch_session_snapshot_json(s, &r) : NULL;
    ok &= sheet_path_named(json, "CorvChild", child, sizeof child);
    ok &= json && strstr(json, "\"kind\":\"hierarchical_label\"") && strstr(json, "D[0..3]") && strstr(json, "\"buses\":[{");
    source = s ? cc_sch_session_sheet_source(s, root, &r) : NULL;
    ok &= source && strstr(source, "(pin \"SIG\"") != NULL;
    text = s ? cc_sch_session_sheet_source(s, child, &r) : NULL;
    ok &= text && strstr(text, "(bus_entry") != NULL;
    after = distinct_references(json, &unannotated);
    ok &= after == before && unannotated == 0; // every reference annotated, units kept together
    free(source);
    free(text);
    free(json);
    char *erc = s ? cc_sch_session_erc_json(s, &r) : NULL;
    ok &= erc != NULL;
    free(erc);
    printf("schematic tools on %s: %d references (was %d), %d unannotated, sheet, pin, bus and entry read back: %s\n", project,
           after, before, unannotated, ok ? "ok" : "WRONG");
    if (s) cc_sch_session_free(s);
    ok &= !schematic_notes(project, dir, root); // API 8, on the edited project reopened
    return !ok;
}

int corvcad_board_native_loads(void); // kicad_board.cpp, not in the public header

// board_test session <in.kicad_pcb> <out-dir>: the board stays open between calls, and is kept
// when the file is written back with an edit's text; any other change reloads it. Works on a
// copy, session.kicad_pcb.
static int session(const char *board, const char *dir)
{
    char copy[4096], error[512] = "";
    int loads = corvcad_board_native_loads();
    snprintf(copy, sizeof copy, "%s/session.kicad_pcb", dir);
    if (write_text(dir, "session.kicad_pcb", read_text(board), "cannot read the board")) return 1;
    free(cc_board_native_net_rules(copy, error, sizeof error));  // load
    free(cc_board_native_ratsnest(copy, error, sizeof error));   // open board
    free(cc_board_native_drc(copy, 0, error, sizeof error));     // open board; a refill after DRC, as in the app
    if (write_text(dir, "session.kicad_pcb", cc_board_native_fill_zones(copy, error, sizeof error), error)) return 1;
    free(cc_board_native_ratsnest(copy, error, sizeof error));   // written back: open board
    if (write_text(dir, "session.kicad_pcb", read_text(board), "cannot read the board")) return 1;
    free(cc_board_native_net_rules(copy, error, sizeof error));  // changed: load
    loads = corvcad_board_native_loads() - loads;
    printf("board session: %d loads for 6 calls (expected 2): %s %s\n", loads, loads == 2 ? "ok" : "WRONG", error);
    return loads != 2 || *error;
}

// The routed copy keeps the input board's file name, so KiCad loads the project's rules beside
// it when <out-dir> is a copy of the project (board_check.py route).
static char routed_name[512] = "routed.kicad_pcb";

// board_test route <in.kicad_pcb> <out-dir>: routes every ratsnest edge of a copy (routed_name),
// with KiCad's router as a finger would (start on the edge's first pad, move to the second, tap
// there), walking around first and shoving when that fails, and writes each route back as the
// app does. One edge (of the first five) goes out on B.Cu, through a via half way, and ends on
// F.Cu.
// board_check.py route compares the DRC of the result with the board before.
static int via_route(const char *copy, const char *dir, double x0, double y0, double x1, double y1)
{
    char error[512] = "", *text = NULL;
    double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    const char *step = "start";
    int fixed = 0;
    if (cc_board_native_route_start(copy, x0, y0, "B.Cu", 2, 0, 0, error, sizeof error)) {
        free(cc_board_native_route_move(mx, my, error, sizeof error));
        step = "toggle the via";
        if (cc_board_native_route_toggle_via()) {
            step = "fix at the via";
            if (cc_board_native_route_fix(mx, my, error, sizeof error) == 0) {
                // As a finger would: tap the end again while the route stops short of it.
                step = "reach the end";
                for (int tap = 0; tap < 3 && fixed == 0; ++tap) {
                    free(cc_board_native_route_move(x1, y1, error, sizeof error));
                    fixed = cc_board_native_route_fix(x1, y1, error, sizeof error);
                }
                if (fixed == 1) {
                    step = "finish with a via";
                    text = cc_board_native_route_finish(error, sizeof error);
                    if (!text || !strstr(text, "(via")) fixed = 0;
                    if (text && write_text(dir, routed_name, text, error)) fixed = 0;
                }
            }
        }
    }
    if (fixed != 1) fprintf(stderr, "via route %.2f %.2f -> %.2f %.2f: could not %s %s\n", x0, y0, x1, y1, step, error);
    cc_board_native_route_cancel();
    return fixed == 1;
}

// The first track segment of a board text: start and end (mm).
static int first_segment(const char *text, double s[4])
{
    const char *segment = text ? strstr(text, "(segment") : NULL, *a, *b;
    if (!segment || !(a = strstr(segment, "(start ")) || !(b = strstr(segment, "(end "))) return 0;
    return sscanf(a, "(start %lf %lf", &s[0], &s[1]) == 2 && sscanf(b, "(end %lf %lf", &s[2], &s[3]) == 2;
}

// Unrouted ratsnest edges of a board (lines of cc_board_native_ratsnest).
static int unrouted(const char *path)
{
    char error[512] = "", *edges = cc_board_native_ratsnest(path, error, sizeof error);
    int n = count(edges, "\n");
    free(edges);
    return n;
}

// After routing: drag the first segment's middle aside (shove), then tune it to 100 mm (the
// meanders must fit); each is written back. Returns how many of the two worked.
static int drag_and_tune(const char *copy, const char *dir)
{
    char error[512] = "", *text = read_text(copy), *after;
    double s[4];
    int dragged = 0, tuned = 0, before = count(text, "(segment") + count(text, "(arc");
    if (!first_segment(text, s)) {
        free(text);
        fprintf(stderr, "drag: no segment\n");
        return 0;
    }
    free(text);
    double mx = (s[0] + s[2]) / 2, my = (s[1] + s[3]) / 2;
    printf("drag: the segment %.3f %.3f -> %.3f %.3f at its middle\n", s[0], s[1], s[2], s[3]);
    const double offsets[] = {1.27, -1.27, 0.635, -0.635};
    for (int i = 0; i < 4 && !dragged; ++i) {
        if (!cc_board_native_drag_start(copy, mx, my, 1, error, sizeof error)) break;
        free(cc_board_native_route_move(mx, my + offsets[i], error, sizeof error));
        if (cc_board_native_route_fix(mx, my + offsets[i], error, sizeof error) == 1) {
            after = cc_board_native_route_finish(error, sizeof error);
            dragged = after && !write_text(dir, routed_name, after, error);
        } else {
            cc_board_native_route_cancel();
        }
    }
    printf("drag: the first track's middle %s %s (%d unrouted)\n", dragged ? "moved" : "NOT moved", dragged ? "" : error,
           unrouted(copy));
    *error = 0;
    text = read_text(copy);
    printf("tune: start\n");
    if (first_segment(text, s) &&
        cc_board_native_route_start(copy, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2, "F.Cu", 2, 2, 100, error, sizeof error)) {
        char *preview = cc_board_native_route_move(s[2], s[3], error, sizeof error);
        const char *status = preview ? strstr(preview, "tuning ") : NULL;
        printf("tune: %.*s\n", status ? (int)strcspn(status, "\n") : 4, status ? status : "none");
        free(preview);
        if (cc_board_native_route_fix(s[2], s[3], error, sizeof error) == 1) {
            after = cc_board_native_route_finish(error, sizeof error);
            tuned = after && count(after, "(segment") + count(after, "(arc") > before &&
                    !write_text(dir, routed_name, after, error);
        }
    }
    cc_board_native_route_cancel();
    free(text);
    printf("tune: meanders %s %s (%d unrouted)\n", tuned ? "added" : "NOT added", tuned ? "" : error, unrouted(copy));
    return dragged + tuned;
}

// board_test pair <in.kicad_pcb> <out-dir> <net>: routes the ratsnest edge of a differential
// pair's net (with its partner) as a finger would, on routed.kicad_pcb.
static int pair(const char *board, const char *dir, const char *net)
{
    char copy[4096], error[512] = "", *edges, *line, *next;
    int done = 0, reached = 0;
    snprintf(copy, sizeof copy, "%s/%s", dir, routed_name);
    if (write_text(dir, routed_name, read_text(board), "cannot read the board")) return 1;
    edges = cc_board_native_ratsnest(copy, error, sizeof error);
    for (line = edges; line && *line && !done; line = next) {
        double x0, y0, x1, y1;
        char name[256] = "";
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "%lf %lf %lf %lf %255[^\n]", &x0, &y0, &x1, &y1, name) != 5 || strcmp(name, net)) continue;
        if (!cc_board_native_route_start(copy, x0, y0, "F.Cu", 1, 1, 0, error, sizeof error)) break;
        int fixed = 0;
        for (int tap = 0; tap < 3 && fixed == 0; ++tap) {
            free(cc_board_native_route_move(x1, y1, error, sizeof error));
            fixed = cc_board_native_route_fix(x1, y1, error, sizeof error);
        }
        // A pair that stops short of the far pads is still committed (and DRC-checked) as fixed.
        reached = fixed == 1;
        char *text = fixed >= 0 ? cc_board_native_route_finish(error, sizeof error) : NULL;
        done = text && strstr(text, "(segment") && !write_text(dir, routed_name, text, error);
        cc_board_native_route_cancel();
    }
    free(edges);
    printf("route: %d of 1 ratsnest edges routed, the differential pair of %s %s %s\n", reached, net,
           done ? (reached ? "placed to the far pads" : "placed part way") : "NOT placed", done ? "" : error);
    fflush(stdout);
    return !done;
}

static int route(const char *board, const char *dir)
{
    char copy[4096], error[512] = "", *edges, *line, *next, *preview = NULL;
    int total = 0, routed = 0, previewTracks = 0, viaRouted = -1;
    snprintf(copy, sizeof copy, "%s/%s", dir, routed_name);
    if (write_text(dir, routed_name, read_text(board), "cannot read the board")) return 1;
    edges = cc_board_native_ratsnest(copy, error, sizeof error);
    if (!edges) {
        fprintf(stderr, "ratsnest: %s\n", error);
        return 1;
    }
    for (line = edges; line && *line; line = next) {
        double x0, y0, x1, y1;
        int done = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "%lf %lf %lf %lf", &x0, &y0, &x1, &y1) != 4) continue;
        ++total;
        if (viaRouted < 1 && total <= 5) done = viaRouted = via_route(copy, dir, x0, y0, x1, y1);
        for (int mode = 2; mode >= 1 && !done; --mode) { // walk around, then shove
            if (!cc_board_native_route_start(copy, x0, y0, "F.Cu", mode, 0, 0, error, sizeof error)) {
                fprintf(stderr, "start %s: %s\n", line, error);
                break;
            }
            preview = cc_board_native_route_move(x1, y1, error, sizeof error);
            previewTracks += preview && strstr(preview, "\ntrack ") ? 1 : 0;
            free(preview);
            if (cc_board_native_route_fix(x1, y1, error, sizeof error) == 1) {
                char *text = cc_board_native_route_finish(error, sizeof error);
                done = text && !write_text(dir, routed_name, text, error);
            } else {
                cc_board_native_route_cancel();
            }
        }
        routed += done;
    }
    free(edges);
    printf("route: %d of %d ratsnest edges routed, %s; %d moves previewed a track\n", routed, total,
           viaRouted == 1 ? "one from B.Cu to F.Cu through a via" : "NONE through a via", previewTracks);
    int extras = drag_and_tune(copy, dir);
    fflush(stdout);
    return routed == 0 || previewTracks == 0 || viaRouted != 1 || extras != 2;
}

// board_test spike <in.kicad_pcb> <out-dir>: free-space routes on a board without nets, driven
// exactly as BoardRouting.swift drives the router for a finger (start; moves; a tap fixes, and a
// fix that continues is followed by a move to the same point; Finish), on the 0.5 mm grid. After
// each route, the board's tracks are checked: an acute corner between two tracks that share an
// end, a track end that stops within 1 mm of another track without touching it, or a track
// shorter than 0.05 mm is a spike. Every track is printed.
typedef struct { double x0, y0, x1, y1; } SEGMENT;

static int segments(const char *text, SEGMENT *out, int capacity)
{
    int n = 0;
    for (const char *s = text; s && (s = strstr(s, "(segment")) && n < capacity; ++s) {
        const char *a = strstr(s, "(start "), *b = strstr(s, "(end ");
        if (a && b && sscanf(a, "(start %lf %lf", &out[n].x0, &out[n].y0) == 2 &&
            sscanf(b, "(end %lf %lf", &out[n].x1, &out[n].y1) == 2)
            ++n;
    }
    return n;
}

static double gap(double px, double py, const SEGMENT *s)
{
    double dx = s->x1 - s->x0, dy = s->y1 - s->y0, l = dx * dx + dy * dy;
    double t = l > 0 ? ((px - s->x0) * dx + (py - s->y0) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypot(px - s->x0 - t * dx, py - s->y0 - t * dy);
}

static double gap(double px, double py, const SEGMENT *s);

// Connected tracks: an end of one lies on the other (a short leg's far end is near its neighbour).
static int linked(const SEGMENT *a, const SEGMENT *b)
{
    return gap(a->x0, a->y0, b) < 0.001 || gap(a->x1, a->y1, b) < 0.001 || gap(b->x0, b->y0, a) < 0.001 ||
           gap(b->x1, b->y1, a) < 0.001;
}

static int spikes(const char *text, const char *label)
{
    SEGMENT s[512];
    int n = segments(text, s, 512), found = 0;
    for (int i = 0; i < n; ++i) {
        printf("  %s track %.3f %.3f -> %.3f %.3f\n", label, s[i].x0, s[i].y0, s[i].x1, s[i].y1);
        if (hypot(s[i].x1 - s[i].x0, s[i].y1 - s[i].y0) < 0.05) { printf("  SPIKE: a %.3f mm track\n", hypot(s[i].x1 - s[i].x0, s[i].y1 - s[i].y0)); ++found; }
        for (int e = 0; e < 2; ++e) {
            double px = e ? s[i].x1 : s[i].x0, py = e ? s[i].y1 : s[i].y0, ox = e ? s[i].x0 : s[i].x1, oy = e ? s[i].y0 : s[i].y1;
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                const double d = gap(px, py, &s[j]);
                const int atStart = hypot(s[j].x0 - px, s[j].y0 - py) < 0.001, atEnd = hypot(s[j].x1 - px, s[j].y1 - py) < 0.001;
                if ((atStart || atEnd) && j > i) {
                    // Two tracks sharing this end: the angle between them, seen from it.
                    const double qx = atStart ? s[j].x1 : s[j].x0, qy = atStart ? s[j].y1 : s[j].y0;
                    const double ax = ox - px, ay = oy - py, bx = qx - px, by = qy - py;
                    const double cosine = (ax * bx + ay * by) / (hypot(ax, ay) * hypot(bx, by) + 1e-12);
                    // Acute, but not a track continued along an existing leg (0 deg, KiCad allows it).
                    if (cosine > 0.01 && cosine < 0.9998) {
                        printf("  SPIKE: acute corner (%.0f deg) at %.3f %.3f\n", acos(cosine) * 180 / 3.14159265358979, px, py);
                        ++found;
                    }
                } else if (d >= 0.001 && d < 1.0 && !linked(&s[i], &s[j])) {
                    printf("  SPIKE: track end %.3f %.3f stops %.3f mm from another track\n", px, py, d);
                    ++found;
                }
            }
        }
    }
    return found;
}

// One finger session: start at points[0], move through the rest, tap (fix) at each index in taps,
// then Finish. Returns the board text after Finish (NULL when nothing was placed).
// PCBOverview.routerPoint: a committed track end within reach of the finger (22 pt, about 1.5 mm
// at the iPad's fitted zoom on an 80 mm board), else the 0.5 mm grid.
static void app_point(const SEGMENT *s, int n, double *x, double *y)
{
    double best = 1.5, bx = 0, by = 0;
    for (int i = 0; i < n; ++i)
        for (int e = 0; e < 2; ++e) {
            double ex = e ? s[i].x1 : s[i].x0, ey = e ? s[i].y1 : s[i].y0, d = hypot(ex - *x, ey - *y);
            if (d <= best) { best = d; bx = ex; by = ey; }
        }
    if (best < 1.5) { *x = bx; *y = by; return; }
    *x = round(*x * 2) / 2;
    *y = round(*y * 2) / 2;
}

static char *finger(const char *copy, const char *dir, const double *raw, int count, const int *taps, int tapCount)
{
    char error[512] = "", *text = read_text(copy);
    SEGMENT committed[512];
    double points[64];
    const int n = segments(text, committed, 512);
    free(text);
    text = NULL;
    for (int i = 0; i < count && i < 32; ++i) {
        points[2 * i] = raw[2 * i];
        points[2 * i + 1] = raw[2 * i + 1];
        app_point(committed, n, &points[2 * i], &points[2 * i + 1]);
    }
    if (!cc_board_native_route_start(copy, points[0], points[1], "F.Cu", 2, 0, 0, error, sizeof error)) {
        printf("  start: %s\n", error);
        return NULL;
    }
    int t = 0, done = 0;
    for (int i = 1; i < count && !done; ++i) {
        double x = points[2 * i], y = points[2 * i + 1];
        free(cc_board_native_route_move(x, y, error, sizeof error));
        if (t < tapCount && taps[t] == i) {
            ++t;
            int fixed = cc_board_native_route_fix(x, y, error, sizeof error);
            if (fixed == 1) done = 1;                                             // the app finishes
            else if (fixed == 0) free(cc_board_native_route_move(x, y, error, sizeof error));
            else printf("  fix at %.2f %.2f: %s\n", x, y, error);
        }
    }
    text = cc_board_native_route_finish(error, sizeof error);
    if (!text) printf("  finish: %s\n", error);
    else text = write_text(dir, routed_name, text, error) ? NULL : read_text(copy); // write_text frees it
    cc_board_native_route_cancel();
    return text;
}

static int spike(const char *board, const char *dir)
{
    char copy[4096];
    snprintf(copy, sizeof copy, "%s/%s", dir, routed_name);
    if (write_text(dir, routed_name, read_text(board), "cannot read the board")) return 1;
    // A: drag diagonally with finger jitter, tap at the end, tap again 1.5 mm further on, Finish.
    const double a[] = {30, 30, 30.5, 30.5, 31.5, 31.5, 33, 33, 35, 35.5, 37.5, 37.5, 39.5, 37.5, 40, 38, 40.5, 39.5};
    const int aTaps[] = {7, 8};
    // B: a new route starting 0.5 mm from A's end, going down.
    const double b[] = {40.5, 38, 40.5, 39, 40.5, 41, 40.5, 42};
    const int bTaps[] = {3};
    // C: the finger first wobbles back up-left, then drags down.
    const double c[] = {50, 30, 49.5, 29.5, 49, 29, 49.5, 30, 50, 32, 50.5, 35, 51, 38};
    const int cTaps[] = {6};
    // D: a route ending 0.7 mm short of A's end (the router may snap onto it).
    const double d[] = {45, 30, 44, 33, 42, 36, 40.5, 37.5};
    const int dTaps[] = {3};
    // E: tap, then the finger goes back toward the start before the next tap.
    const double e[] = {30, 45, 36, 45, 36, 45, 34, 45.5, 36, 48};
    const int eTaps[] = {2, 4};
    struct { const char *name; const double *p; int n; const int *taps; int tapCount; } runs[] = {
        {"A", a, 9, aTaps, 2}, {"B", b, 4, bTaps, 1}, {"C", c, 7, cTaps, 1}, {"D", d, 4, dTaps, 1}, {"E", e, 5, eTaps, 2}};
    int found = 0;
    for (size_t r = 0; r < sizeof runs / sizeof runs[0]; ++r) {
        printf("spike %s:\n", runs[r].name);
        char *text = finger(copy, dir, runs[r].p, runs[r].n, runs[r].taps, runs[r].tapCount);
        int here = text ? spikes(text, runs[r].name) : 0;
        printf("spike %s: %d found\n", runs[r].name, here);
        found += here;
        free(text);
    }
    printf("spike: %d found in all\n", found);
    return found != 0;
}

// board_test edit <copy.kicad_pcb> <out-dir> <request>...: KiCad's board edits
// (cc_board_native_edit), each written back to the copy as the app does, then the session's DRC
// of the result in <out-dir>/drc.json (board_check.py edit and edit-parity).
static int board_edit(const char *board, const char *dir, int count, char **requests)
{
    char error[512] = "";
    for (int i = 0; i < count; ++i) {
        char *text = cc_board_native_edit(board, requests[i], error, sizeof error);
        FILE *out = text ? fopen(board, "wb") : NULL;
        if (!out || fputs(text, out) < 0 || fclose(out)) {
            fprintf(stderr, "edit %d: %s\n", i + 1, text ? "cannot write the board" : error);
            free(text);
            return 1;
        }
        free(text);
    }
    char *drc = cc_board_native_drc(board, 0, error, sizeof error);
    printf("board edits on %s: %d applied\n", board, count);
    return write_text(dir, "drc.json", drc, error);
}

static void *stop_soon(void *unused)
{
    int stopped = 0;
    (void)unused;
    // The netlist comes first: keep trying until a run is going, for at most 60 s.
    for (int tries = 0; tries < 600 && !stopped; ++tries) {
        usleep(100000);
        if (tries >= 3) stopped = cc_sch_simulate_stop();
    }
    printf("stop: %d\n", stopped);
    return NULL;
}

int main(int argc, char **argv)
{
    char error[512] = "";
    double start, drc_ms, refill_ms, fill_ms;
    int failed = 0;
    if (argc == 5 && !strcmp(argv[1], "update"))
        return update(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "session"))
        return session(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "library"))
        return library_reload(argv[2], argv[3]);
    setvbuf(stdout, NULL, _IONBF, 0); // progress shows even if a check crashes
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "route")) {
        const char *slash = strrchr(argv[2], '/');
        snprintf(routed_name, sizeof routed_name, "%s", slash ? slash + 1 : argv[2]);
        return argc == 4 ? route(argv[2], argv[3]) : pair(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && !strcmp(argv[1], "spike"))
        return spike(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "symbol"))
        return place_symbol(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "schematic"))
        return schematic_tools(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "projects"))
        return project_switch(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "edit"))
        return board_edit(argv[2], argv[3], argc - 4, argv + 4);
    // import-board|import-sch <input> <format> <output>: KiCad's importers (board_check.py import).
    if (argc == 5 && !strcmp(argv[1], "import-board")) {
        char *report = cc_board_native_import(argv[2], argv[3], argv[4], error, sizeof error);
        if (!report) {
            fprintf(stderr, "board import: %s\n", error);
            return 1;
        }
        printf("board import %s\n", report);
        free(report);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "import-sch")) {
        cc_sch_result result = {0, NULL};
        char *report = cc_sch_import(argv[2], argv[3], argv[4], &result);
        if (!report) {
            fprintf(stderr, "schematic import: %s\n", result.message ? result.message : "failed");
            return 1;
        }
        printf("schematic import %s\n", report);
        free(report);
        return 0;
    }
    // sim-stop <project> <long-command> <short-command>: the short run, the long run stopped once
    // it's going, and the short run again, in one process and session; prints each first line and
    // its last time point. A run after a stopped one must match the first.
    if (argc == 5 && !strcmp(argv[1], "sim-stop")) {
        cc_sch_result result = {0, NULL};
        cc_sch_session *sch = cc_sch_session_open(argv[2], &result);
        pthread_t stopper;
        if (!sch) return 1;
        for (int run = 0; run < 3; ++run) {
            if (run == 1) pthread_create(&stopper, NULL, stop_soon, NULL);
            char *text = cc_sch_session_simulate(sch, argv[run == 1 ? 3 : 4], 100, &result);
            if (run == 1) pthread_join(stopper, NULL);
            if (!text) {
                fprintf(stderr, "run %d: %s\n", run, result.message ? result.message : "failed");
                failed = 1;
                continue;
            }
            const char *line = strstr(text, "\ntime\t");
            const char *end = line ? strchr(line + 1, '\n') : NULL;
            const char *last = end;
            while (last && last > line && last[-1] != ' ') --last;
            printf("run %d: %.*s last time %.*s\n", run, (int)strcspn(text, "\n"), text,
                   last ? (int)(end - last) : 0, last ? last : "");
            free(text);
        }
        cc_sch_session_free(sch);
        return failed;
    }
    // simulate|sim-netlist <project-or-schematic> <out-file> [command [max-points]]: KiCad's
    // simulator through the session (cc_sch_session_simulate), or the SPICE netlist it would run
    // (cc_sch_session_spice_netlist); writes the text and prints its first line and the time
    // (ngspice/check.sh). An empty command uses the schematic's directive.
    if ((argc >= 4 && argc <= 6) && (!strcmp(argv[1], "simulate") || !strcmp(argv[1], "sim-netlist"))) {
        cc_sch_result result = {0, NULL};
        cc_sch_session *sch = cc_sch_session_open(argv[2], &result);
        if (!sch) {
            fprintf(stderr, "open: %s\n", result.message ? result.message : "failed");
            return 1;
        }
        if (!(cc_sch_session_capabilities(sch) & CC_SCH_SIMULATE)) {
            fprintf(stderr, "this build has no simulator\n");
            cc_sch_session_free(sch);
            return 1;
        }
        double began = now_ms();
        char *text = !strcmp(argv[1], "sim-netlist")
                         ? cc_sch_session_spice_netlist(sch, argc >= 5 ? argv[4] : NULL, &result)
                         : cc_sch_session_simulate(sch, argc >= 5 ? argv[4] : NULL,
                                                   argc == 6 ? (size_t)strtoul(argv[5], NULL, 10) : 0, &result);
        double took = now_ms() - began;
        cc_sch_session_free(sch);
        if (!text) {
            fprintf(stderr, "simulate: %s\n", result.message ? result.message : "failed");
            return 1;
        }
        FILE *out = fopen(argv[3], "w");
        if (!out || fputs(text, out) < 0 || fclose(out)) {
            fprintf(stderr, "cannot write %s\n", argv[3]);
            free(text);
            return 1;
        }
        printf("%.*s\t%.1f ms\n", (int)strcspn(text, "\n"), text, took);
        free(text);
        return 0;
    }
    // plot <in.kicad_pcb> <out-dir>: the PDF and the SVGs; prints the file names.
    if (argc == 4 && !strcmp(argv[1], "plot")) {
        int format;
        for (format = 0; format < 2; ++format) {
            char *files = cc_board_native_plot(argv[2], argv[3], format, error, sizeof error);
            if (!files) {
                fprintf(stderr, "plot %d: %s\n", format, error);
                return 1;
            }
            printf("%s", files);
            free(files);
        }
        return 0;
    }
    // setup <in.kicad_pcb>: prints Board Setup. setup <in.kicad_pcb> <setup-file>: applies it
    // and writes the board and its project file in place (run it on a copy).
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "setup")) {
        char *text, *project = NULL, *setup, path[4096], *dot;
        FILE *out;
        if (argc == 3) {
            text = cc_board_native_board_setup(argv[2], error, sizeof error);
            if (!text) {
                fprintf(stderr, "setup: %s\n", error);
                return 1;
            }
            printf("%s", text);
            free(text);
            return 0;
        }
        setup = read_text(argv[3]);
        if (!setup) {
            fprintf(stderr, "cannot read %s\n", argv[3]);
            return 1;
        }
        text = cc_board_native_apply_board_setup(argv[2], setup, &project, error, sizeof error);
        free(setup);
        if (!text) {
            fprintf(stderr, "apply setup: %s\n", error);
            return 1;
        }
        snprintf(path, sizeof path, "%s", argv[2]);
        dot = strrchr(path, '.');
        if (dot) strcpy(dot, ".kicad_pro");
        out = fopen(path, "wb");
        if (!out || fputs(project, out) < 0 || fclose(out)) return 1;
        out = fopen(argv[2], "wb");
        if (!out || fputs(text, out) < 0 || fclose(out)) return 1;
        free(project);
        free(text);
        return 0;
    }
    // fab <in.kicad_pcb> <out-dir>: the fabrication outputs; board_check.py fab compares them.
    if (argc == 4 && !strcmp(argv[1], "fab")) {
        char *files = cc_board_native_fab_outputs(argv[2], argv[3], 0, error, sizeof error);
        if (!files) {
            fprintf(stderr, "fab: %s\n", error);
            return 1;
        }
        printf("%s", files);
        free(files);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.kicad_pcb> <out-dir> | update <in.kicad_pcb> <netlist> <out-dir> | "
                        "session <in.kicad_pcb> <out-dir> | symbol <project> <Library:Name> | "
                        "fab <in.kicad_pcb> <out-dir>\n", argv[0]);
        return 2;
    }
    if (!cc_board_native_backend()) {
        fprintf(stderr, "no native board backend\n");
        return 3;
    }
    start = now_ms();
    failed |= write_text(argv[2], "drc.json", cc_board_native_drc(argv[1], 0, error, sizeof error), error);
    drc_ms = now_ms() - start;
    start = now_ms();
    failed |= write_text(argv[2], "drc-refilled.json", cc_board_native_drc(argv[1], 1, error, sizeof error), error);
    refill_ms = now_ms() - start;
    start = now_ms();
    failed |= write_text(argv[2], "filled.kicad_pcb", cc_board_native_fill_zones(argv[1], error, sizeof error), error);
    fill_ms = now_ms() - start;
    failed |= write_text(argv[2], "ratsnest.txt", cc_board_native_ratsnest(argv[1], error, sizeof error), error);
    failed |= write_text(argv[2], "netrules.txt", cc_board_native_net_rules(argv[1], error, sizeof error), error);
    printf("{\"backend\":\"%s\",\"drcMs\":%.1f,\"refillDrcMs\":%.1f,\"fillMs\":%.1f}\n",
           cc_board_native_backend(), drc_ms, refill_ms, fill_ms);
    fflush(stdout); // shown even if teardown crashes
    return failed;
}

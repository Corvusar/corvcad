/* SPDX-License-Identifier: MIT */
/* Drives a cc_sch_session backend through the C ABI only, so the same file links against
 * the compatible engine and the native KiCad backend (port gate 3).
 *
 *   abi_test <project.kicad_pro|.kicad_sch> <new-out-dir>
 *
 * Writes snapshots before.json, aborted.json, after.json, undone.json and redone.json, and
 * a saved copy in <out>/copy. With CC_SCH_NETLIST and CC_SCH_ERC it also writes KiCad's
 * netlist and ERC report after the undo (undone.net, undone-erc.json) and after the redo
 * (redone.net, redone-erc.json). check.sh compares them and runs kicad-cli on the copy. */
#include <corvcad/schematic_session_c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void cc_free_text(char *text);

static cc_sch_result result;
static const char *out;

static void check(int okay, const char *step) {
  if (okay) return;
  fprintf(stderr, "%s failed: %s\n", step, result.message ? result.message : "(no message)");
  exit(1);
}

/* Writes text (owned, freed here) to <out>/<name>. */
static void save(char *text, const char *name) {
  char path[4096];
  check(text != NULL, name);
  snprintf(path, sizeof path, "%s/%s", out, name);
  FILE *file = fopen(path, "wb");
  check(file != NULL, path);
  fputs(text, file);
  fclose(file);
  cc_free_text(text);
}

static void snapshot(cc_sch_session *session, const char *name) {
  save(cc_sch_session_snapshot_json(session, &result), name);
}

/* Version 3: KiCad's netlist and ERC of the current state, or the refusal without them. */
static void kicad_checks(cc_sch_session *session, const char *netlist, const char *erc) {
  if (!(cc_sch_session_capabilities(session) & CC_SCH_NETLIST)) {
    check(!cc_sch_session_netlist(session, &result) && result.code == CC_SCH_UNSUPPORTED,
          "netlist refused without CC_SCH_NETLIST");
    check(!cc_sch_session_erc_json(session, &result) && result.code == CC_SCH_UNSUPPORTED,
          "ERC refused without CC_SCH_ERC");
    return;
  }
  save(cc_sch_session_netlist(session, &result), netlist);
  save(cc_sch_session_erc_json(session, &result), erc);
}

/* Copies the string value after the first `key` at or after `from`. */
static const char *field(const char *from, const char *key, char *value, size_t capacity) {
  const char *start = strstr(from, key);
  check(start != NULL, key);
  start += strlen(key);
  size_t length = strcspn(start, "\"");
  check(length < capacity, key);
  memcpy(value, start, length);
  value[length] = 0;
  return start + length;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <project> <new-out-dir>\n", argv[0]);
    return 2;
  }
  out = argv[2];
  check(mkdir(out, 0755) == 0, "create out dir");
  cc_sch_session *session = cc_sch_session_open(argv[1], &result);
  check(session != NULL, "open");
  printf("backend %s, capabilities %u\n", cc_sch_session_backend(session),
         cc_sch_session_capabilities(session));
  snapshot(session, "before.json");

  /* The root sheet comes first. Edit its symbol with the smallest UUID: the backends list
   * symbols in different orders (file order, KiCad's spatial index). */
  char *json = cc_sch_session_snapshot_json(session, &result);
  char root[256], uuid[64] = "", candidate[64];
  double x = 0, y = 0;
  field(json, "\"path\":\"", root, sizeof root);
  const char *symbol = strstr(json, "\"symbols\":[{");
  check(symbol != NULL, "a symbol on the root sheet");
  const char *end = strstr(symbol, "],\"wires\":[");
  for (const char *item = strstr(symbol, "{\"id\":"); item && item < end; item = strstr(item + 1, "{\"id\":")) {
    const char *rest = field(item, "\"uuid\":\"", candidate, sizeof candidate);
    if (*uuid && strcmp(candidate, uuid) >= 0) continue;
    strcpy(uuid, candidate);
    rest = strstr(rest, "\"x\":");
    x = strtod(rest + 4, NULL);
    y = strtod(strstr(rest, "\"y\":") + 4, NULL);
  }
  cc_free_text(json);

  cc_sch_transaction *t = cc_sch_transaction_begin(session, &result);
  check(t != NULL, "begin");
  check(cc_sch_transaction_delete_item(t, root, uuid, &result), "delete");
  cc_sch_transaction_free(t); /* aborts */
  snapshot(session, "aborted.json");

  t = cc_sch_transaction_begin(session, &result);
  check(t != NULL, "begin");
  check(cc_sch_transaction_move_symbol(t, root, uuid, x + 2.54, y, &result), "move symbol");
  check(cc_sch_transaction_set_property(t, root, uuid, "Value", "corvcad", &result), "set value");
  check(cc_sch_transaction_rotate_symbol(t, root, uuid, &result), "rotate symbol"); /* version 4 */
  check(cc_sch_transaction_add_wire(t, root, 25.4, 25.4, 30.48, 25.4, &result), "add wire");
  check(cc_sch_transaction_add_label(t, root, "CORV_ABI", 25.4, 25.4, &result), "add label");
  /* Version 2 touch authoring: a two-pin part at the first free grid spot along the page's
   * bottom rows, and a routed wire from its pin 2 to a point up and to the right. */
  double px = 0, py = 0;
  for (int row = 0; row < 4 && !px; ++row)
    for (int col = 0; col < 18 && !px; ++col)
      if (cc_sch_transaction_add_two_pin(t, root, 25.4 + col * 12.7, 182.88 - row * 12.7, &result))
        px = 25.4 + col * 12.7, py = 182.88 - row * 12.7;
  check(px != 0, "add two-pin part");
  double xy[16];
  size_t points = cc_sch_session_route(session, root, px + 5.08, py, px + 12.7, py - 7.62, xy, 8, &result);
  check(points >= 2, "plan route");
  printf("two-pin at %g,%g; route", px, py);
  for (size_t i = 0; i < points && i < 8; ++i) printf(" %g,%g", xy[2 * i], xy[2 * i + 1]);
  printf("\n");
  check(cc_sch_transaction_add_routed_wire(t, root, px + 5.08, py, px + 12.7, py - 7.62, &result),
        "add routed wire");
  check(cc_sch_transaction_commit(t, &result), "commit");
  cc_sch_transaction_free(t);
  snapshot(session, "after.json");

  check(cc_sch_session_can_undo(session) && cc_sch_session_undo(session, &result), "undo");
  snapshot(session, "undone.json");
  kicad_checks(session, "undone.net", "undone-erc.json");
  check(cc_sch_session_can_redo(session) && cc_sch_session_redo(session, &result), "redo");
  snapshot(session, "redone.json");
  kicad_checks(session, "redone.net", "redone-erc.json");

  char copy[4096];
  snprintf(copy, sizeof copy, "%s/copy", out);
  check(cc_sch_session_save_copy(session, copy, &result), "save copy");
  cc_sch_session_free(session);
  cc_sch_result_clear(&result);
  puts("abi_test: ok");
  return 0;
}

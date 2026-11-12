/* SPDX-License-Identifier: MIT */
/* Runs a SPICE netlist through the static ngspice library's shared API (the calls KiCad's
 * NGSPICE wrapper makes) and writes each resulting plot as an ASCII raw file, so check.sh can
 * compare it with the ngspice executable's own raw file.
 *   ngspice_probe <netlist> <out-prefix> [command...]   -> <out-prefix>.<n>.raw per plot, n from 0
 * The commands run before the netlist is loaded (KiCad sets its compatibility mode there).
 * Exits 1 when ngspice asks to exit (its controlled_exit callback) or a command fails. */
#include <stdbool.h> /* sharedspice.h uses bool without including it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ngspice/sharedspice.h"

int corvcad_ngspice_codemodels(void); /* static_codemodels.c, linked into the library */

static int exited;

static int output(char *text, int id, void *user) { (void)id; (void)user; fprintf(stderr, "%s\n", text); return 0; }
static int status(char *text, int id, void *user) { (void)text; (void)id; (void)user; return 0; }
static int controlled_exit(int code, NG_BOOL unload, NG_BOOL quit, int id, void *user) {
  (void)unload; (void)quit; (void)id; (void)user;
  fprintf(stderr, "ngspice asked to exit (%d)\n", code);
  exited = 1;
  return 0;
}
static int running(NG_BOOL stopped, int id, void *user) { (void)stopped; (void)id; (void)user; return 0; }

static int command(const char *text) {
  int failed = ngSpice_Command((char *)text);
  if (failed || exited) fprintf(stderr, "command failed: %s\n", text);
  return failed || exited;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: ngspice_probe <netlist> <out-prefix> [command...]\n"); return 2; }
  char line[4096];
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (ngSpice_Init(output, status, controlled_exit, NULL, NULL, running, NULL)) return 1;
  /* Where desktop ngspice's spinit loads the code-model plug-ins. */
  fprintf(stderr, "code models: %d\n", corvcad_ngspice_codemodels());
  for (int i = 3; i < argc; ++i)
    if (command(argv[i])) return 1;
  snprintf(line, sizeof line, "source %s", argv[1]);
  /* "run" is synchronous here; KiCad's wrapper uses "bg_run" and waits on the running callback. */
  if (command(line) || command("run") || command("set filetype=ascii")) return 1;
  clock_gettime(CLOCK_MONOTONIC, &end);
  char **plots = ngSpice_AllPlots();
  int count = 0;
  /* Newest first, ending with the constants plot. */
  for (int i = 0; plots && plots[i]; ++i) {
    if (!strcmp(plots[i], "const")) continue;
    snprintf(line, sizeof line, "setplot %s", plots[i]);
    if (command(line)) return 1;
    snprintf(line, sizeof line, "write %s.%d.raw", argv[2], count++);
    if (command(line)) return 1;
  }
  printf("plots %d, %.1f ms\n", count,
         (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6);
  return count ? 0 : 1;
}

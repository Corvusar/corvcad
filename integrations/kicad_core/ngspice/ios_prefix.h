/* SPDX-License-Identifier: MIT */
/* Included first in every ngspice translation unit of ngspice_ios.sh's builds, at make time only
 * (in configure, its <stdlib.h> would break the function checks). */
#include <TargetConditionals.h>
#include <pwd.h>
#include <stdlib.h>

#if TARGET_OS_IPHONE
/* iOS has no system(). ngspice calls it only for desktop helpers an app can't run (its "shell"
 * command, gnuplot and similar), and on iOS those fail as if no shell were available. */
static inline int corvcad_no_system(const char *command) { (void)command; return -1; }
#define system corvcad_no_system

/* An iOS process has no passwd entry, and ngSpice_Init dereferences getpwuid(getuid()) to find
 * ~/.spiceinit. Without an entry, the home directory is $HOME (the app's container). */
static inline struct passwd *corvcad_getpwuid(uid_t uid) {
  static struct passwd home;
  struct passwd *pw = getpwuid(uid);
  if (pw) return pw;
  home.pw_name = (char *)"";
  home.pw_dir = getenv("HOME") ? getenv("HOME") : (char *)"/";
  return &home;
}
#define getpwuid corvcad_getpwuid
#endif

/* macOS and iOS: ngspice's memory check reads the free page count through host_statistics (only
 * frontend/get_avail_mem_size.c calls it); ngspice/apple_memory.c reports usable memory instead.
 * The rename also applies to the declaration in <mach/mach.h>, so the signature stays checked. */
#define host_statistics corvcad_host_statistics

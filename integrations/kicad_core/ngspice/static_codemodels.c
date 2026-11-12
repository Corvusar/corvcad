/* SPDX-License-Identifier: MIT */
/* CorvCAD: registers the XSPICE code models that ngspice_ios.sh links into libngspice.a.
 * Desktop ngspice loads each code-model library (.cm) at run time with dlopen and hands its
 * device and user-defined node tables to add_device and add_udn (load_opus in
 * spicelib/devices/dev.c); an iOS app can't load such plug-ins. Here the models' own objects
 * (cfunc.o, ifspec.o, udnfunc.o) are linked in instead, without each library's dlmain.c, whose
 * forwarding functions would duplicate the core's, and the tables go to the same two calls.
 * The headers are the ones cmpp -lst generates for each library in the build tree.
 * MIT licence (CorvCAD). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ngspice/config.h"
#include "ngspice/cpextern.h"
#include "ngspice/devdefs.h"
#include "ngspice/dstring.h"
#include "ngspice/dllitf.h" /* the code-model types; the plug-in interface itself goes unused */
#include "ngspice/evtudn.h"
#include "ngspice/inpdefs.h"
#include "ngspice/inertial.h"

#include "spice2poly/cmextrn.h"
#include "digital/cmextrn.h"
#include "analog/cmextrn.h"
#include "xtradev/cmextrn.h"
#include "xtraevt/cmextrn.h"
#include "table/cmextrn.h"
#include "tlines/cmextrn.h"
#include "spice2poly/udnextrn.h"
#include "digital/udnextrn.h"
#include "analog/udnextrn.h"
#include "xtradev/udnextrn.h"
#include "xtraevt/udnextrn.h"
#include "table/udnextrn.h"
#include "tlines/udnextrn.h"

/* The one forwarder the core has no function of the same name for: a plug-in's cm_getvar calls
 * the interface table's entry, which is cp_getvar (xspice/cm/cmexport.c). */
bool cm_getvar(char *name, enum cp_types type, void *retval, size_t rsize);
bool cm_getvar(char *name, enum cp_types type, void *retval, size_t rsize) {
    return cp_getvar(name, type, retval, rsize);
}

int add_device(int n, SPICEdev **devs, int flag);
int add_udn(int n, Evt_Udn_Info_t **udns);

static SPICEdev *devices[] = {
#include "spice2poly/cminfo.h"
#include "digital/cminfo.h"
#include "analog/cminfo.h"
#include "xtradev/cminfo.h"
#include "xtraevt/cminfo.h"
#include "table/cminfo.h"
#include "tlines/cminfo.h"
    NULL
};

static Evt_Udn_Info_t *udns[] = {
#include "spice2poly/udninfo.h"
#include "digital/udninfo.h"
#include "analog/udninfo.h"
#include "xtradev/udninfo.h"
#include "xtraevt/udninfo.h"
#include "table/udninfo.h"
#include "tlines/udninfo.h"
    NULL
};

/* Call once, after ngSpice_Init, where desktop ngspice's spinit runs its "codemodel" commands.
 * Returns the number of devices registered. */
__attribute__((visibility("default"))) int corvcad_ngspice_codemodels(void) {
    static int registered;
    int n = (int)(sizeof devices / sizeof *devices) - 1;
    if (!registered) {
        add_device(n, devices, 1);
        add_udn((int)(sizeof udns / sizeof *udns) - 1, udns);
        registered = 1;
    }
    return n;
}

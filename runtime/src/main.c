/* main.c — the wovm CLI. Exit-code contract the toolchain scripts against:
 *   0 = ran to completion
 *   1 = trap; one stderr line: "trap CODE in METHOD at line N: MESSAGE"
 *   2 = usage or load failure (loader's message on stderr)
 * Heap cap defaults to 64 MiB, overridable via WO_HEAP_MB. */
#include <stdio.h>
#include <stdlib.h>

#include "vm.h"

static wo_vm VM; /* 32K value stack: keep it off the C stack */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: wovm <file.wob>\n");
        return 2;
    }
    wo_module mod;
    char err[256];
    if (wo_load_file(&mod, argv[1], err, sizeof err) != 0) {
        fprintf(stderr, "wovm: %s\n", err);
        return 2;
    }
    if (mod.entry == WOB_NONE) {
        fprintf(stderr, "wovm: module has no entry method\n");
        wo_module_free(&mod);
        return 2;
    }
    size_t heap_mb = 64;
    const char *env = getenv("WO_HEAP_MB");
    if (env && env[0]) {
        char *end = NULL;
        unsigned long v = strtoul(env, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 1048576) heap_mb = v;
    }
    if (wo_vm_init(&VM, &mod, heap_mb << 20) != 0) {
        fprintf(stderr, "wovm: cannot allocate %zu MiB heap\n", heap_mb);
        wo_module_free(&mod);
        return 2;
    }
    uint64_t ret = 0;
    wo_err terr;
    int rc = wo_vm_call(&VM, mod.entry, NULL, 0, &ret, &terr);
    if (rc != 0)
        fprintf(stderr, "trap %u in %s at line %u: %s\n", (unsigned)terr.code,
                terr.method, (unsigned)terr.line, terr.msg);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return rc == 0 ? 0 : 1;
}

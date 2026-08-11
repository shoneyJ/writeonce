/* main.c — the wovm CLI. Exit-code contract the toolchain scripts against:
 *   0 = ran to completion
 *   1 = trap; one stderr line: "trap CODE in METHOD at line N: MESSAGE"
 *   2 = usage or load failure (loader's message on stderr)
 * Heap cap defaults to 64 MiB, overridable via WO_HEAP_MB. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gc.h"
#include "vm.h"

static wo_vm VM; /* 32K value stack: keep it off the C stack */

/* ---- self-exec detection (Task 6, plan 3) -------------------------------
 * `woc build` makes a single executable by copying wovm and appending the
 * .wob image plus a fixed-size trailer; docs/plan/oop-vm/00-wob-format.md's
 * "single-binary trailer" section is the normative layout (writer:
 * compiler/bin/main.ml) -- keep this reader in lock-step with it. Reading
 * this executable's own path via /proc/self/exe is Linux-only, matching
 * wob.h's own platform note. */
#define WO_TRAILER_MAGIC 0x31544257u /* "WBT1" read as LE u32 */
#define WO_TRAILER_SIZE 20u          /* payload_off u64, payload_len u64, magic u32 */

/* 1 = embedded image found and loaded into *mod (caller must ignore argv);
 * 0 = no trailer (plain wovm binary; caller falls back to argv[1] as
 * today); -1 = a trailer is present but corrupt (err filled; caller must
 * report and exit -- never guess or run something unintended). */
static int load_self_embedded(wo_module *mod, char *err, size_t errlen) {
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    if (size < WO_TRAILER_SIZE) {
        close(fd);
        return 0;
    }
    uint8_t tail[WO_TRAILER_SIZE];
    if (lseek(fd, (off_t)(size - WO_TRAILER_SIZE), SEEK_SET) < 0 ||
        read(fd, tail, WO_TRAILER_SIZE) != (ssize_t)WO_TRAILER_SIZE) {
        close(fd);
        return 0;
    }
    uint32_t magic;
    memcpy(&magic, tail + 16, 4);
    if (magic != WO_TRAILER_MAGIC) {
        close(fd);
        return 0; /* plain wovm binary, nothing embedded */
    }
    uint64_t payload_off, payload_len;
    memcpy(&payload_off, tail + 0, 8);
    memcpy(&payload_len, tail + 8, 8);
    /* payload must exactly fill everything between its offset and the
     * trailer -- no gap, no overlap. Bounding payload_off first makes the
     * subtraction below safe (no unsigned wraparound on a corrupt value). */
    if (payload_off > size - WO_TRAILER_SIZE) {
        close(fd);
        snprintf(err, errlen, "corrupt trailer (bad payload offset)");
        return -1;
    }
    if (payload_len != size - WO_TRAILER_SIZE - payload_off) {
        close(fd);
        snprintf(err, errlen, "corrupt trailer (bad payload length)");
        return -1;
    }
    void *p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(err, errlen, "cannot mmap self");
        return -1;
    }
    int rc = wo_load_buf(mod, (const uint8_t *)p + payload_off, (size_t)payload_len, err, errlen);
    munmap(p, size);
    return rc == 0 ? 1 : -1;
}

/* ---- gc pump -----------------------------------------------------------
 * Deliberately the simplest possible driver over the plan-1 collector's
 * already-budgeted step interface (wo_gc_step, gc.h): after the entry
 * method returns, drain the cycle-candidate buffer in bounded slices until
 * it is empty. This is post-exit-only pacing and nothing more — real
 * scheduler-integrated pacing (stepping between turns of live work while
 * the program keeps running) is sub-project 2's job; this milestone only
 * proves the budgeted-step interface end to end and makes it observable.
 * WO_GC_TRACE prints one stderr line per step (never stdout — the corpus
 * harness diffs stdout byte-for-byte) so a fixture can assert "collection
 * happened in bounded slices", not just "the leak is gone". */
static void gc_pump(wo_vm *vm) {
    size_t budget = 64; /* candidates per step: no prior art to size this
                            against (post-exit draining is new), so picked
                            to mirror WO_HEAP_MB's default 64 — small
                            enough that a deliberately oversized abandoned
                            structure visibly takes more than one step,
                            large enough that ordinary programs clear in
                            one or two */
    const char *benv = getenv("WO_GC_BUDGET");
    if (benv && benv[0]) {
        char *end = NULL;
        unsigned long v = strtoul(benv, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 1000000) budget = v;
    }
    int trace = getenv("WO_GC_TRACE") != NULL;
    size_t step = 0;
    while (vm->rt.cycbuf.len > 0) {
        size_t before = vm->rt.cycbuf.len;
        size_t freed = wo_gc_step(&vm->rt, budget);
        step++;
        if (trace)
            fprintf(stderr, "gc: step %zu budget=%zu freed=%zu visited=%zu remaining=%zu\n", step,
                    budget, freed, before - vm->rt.cycbuf.len, vm->rt.cycbuf.len);
    }
}

int main(int argc, char **argv) {
    wo_module mod;
    char err[256];
    int self_rc = load_self_embedded(&mod, err, sizeof err);
    if (self_rc < 0) {
        fprintf(stderr, "wovm: %s\n", err);
        return 2;
    }
    if (self_rc == 0) {
        /* no embedded image: today's contract, unchanged */
        if (argc != 2) {
            fprintf(stderr, "usage: wovm <file.wob>\n");
            return 2;
        }
        if (wo_load_file(&mod, argv[1], err, sizeof err) != 0) {
            fprintf(stderr, "wovm: %s\n", err);
            return 2;
        }
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
    gc_pump(&VM);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return rc == 0 ? 0 : 1;
}

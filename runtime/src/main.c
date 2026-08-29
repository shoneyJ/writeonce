/* main.c — the wovm CLI. Exit-code contract the toolchain scripts against:
 *   0 = ran to completion
 *   1 = trap; one stderr line: "trap CODE in METHOD at line N: MESSAGE"
 *   2 = usage or load failure (loader's message on stderr)
 * Heap cap defaults to 64 MiB, overridable via WO_HEAP_MB. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "cont.h"
#include "gc.h"
#include "table.h"
#include "vm.h"
#include "wal.h"

/* stamped by the Makefile from the repo-root VERSION file; the fallback keeps
 * a hand-compiled main.c building. `just dist` asserts it matches `woc`. */
#ifndef WO_VERSION
#define WO_VERSION "0.0.0-dev"
#endif

/* The shard array: index 0 is the primary (runs the entry, owns the
 * database); the arc's default is ONE VM PER CORE (WO_SHARDS overrides,
 * =1 is the serial escape hatch). Static: each wo_vm carries its 32K
 * value stack, kept off the C stack. */
#define WO_MAX_SHARDS 64u
static wo_vm SHARDS[WO_MAX_SHARDS];
#define VM (SHARDS[0])
static wo_db DB;  /* the per-shard engine (one shard until iteration 8) */
static wo_wal WAL;

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

/* ---- gc pump (iteration 7b) ---------------------------------------------
 * After the entry method returns the stack is empty, so a collection cycle
 * has no roots: everything still on the traced list is unreachable and one
 * cycle frees it all, in budgeted slices (WO_GC_BUDGET objects per slice —
 * rt owns the knobs now, read at init). WO_GC_TRACE prints one stderr line
 * per slice (never stdout — the corpus harness diffs stdout byte-for-byte)
 * so a fixture can assert "collection happened in bounded slices", not just
 * "the leak is gone". A mid-program cycle interrupted by exit is finished
 * here the same way. The zero-progress guard turns a would-be hang (a bug)
 * into a leak the ASan gate reports instead. */
static void gc_pump(wo_vm *vm) {
    wo_rt *rt = &vm->rt;
    size_t at_begin = rt->gc_traced_cnt;
    while (rt->gc_traced) {
        if (rt->gc_phase == WO_GC_IDLE) {
            at_begin = rt->gc_traced_cnt;
            wo_gc_begin(rt); /* no roots: the stack is empty at depth 0 */
        }
        wo_gc_slice(rt, rt->gc_budget);
        if (rt->gc_phase == WO_GC_IDLE && rt->gc_traced_cnt == at_begin) break;
    }
}

/* databasev2 4: one diagnostic line about group commit, opt-in via
 * WO_WAL_STATS. Off by default because it would otherwise pollute the output
 * of every durable program; a gate that wants the numbers asks for them. */
static void wal_stats_report(const wo_wal *w) {
    if (!w || !getenv("WO_WAL_STATS")) return;
    fprintf(stderr,
            "walstats batches=%llu records=%llu peak_batch=%llu peak_staged=%llu "
            "compactions=%llu compact_us_max=%llu compact_us_total=%llu compacted_bytes=%llu\n",
            (unsigned long long)w->stat_batches, (unsigned long long)w->stat_records,
            (unsigned long long)w->stat_peak_batch, (unsigned long long)w->stat_peak_staged,
            (unsigned long long)w->stat_compactions,
            (unsigned long long)w->stat_compact_us_max,
            (unsigned long long)w->stat_compact_us_total,
            (unsigned long long)w->compacted_bytes);
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
        /* no embedded image: the image path is argv[1], and program mode
           passes everything after it to the program itself. `--version` is
           handled ONLY here (plain wovm) so a built app never shadows its
           own `version` argument. */
        if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0)) {
            printf("wovm %s\n", WO_VERSION);
            return 0;
        }
        if (argc < 2) {
            fprintf(stderr, "usage: wovm <file.wob> [args...]\n");
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
    VM.shard_id = 0;
    VM.is_primary = 1;
    VM.rt.shard_id = 0;
    VM.wake_efd = eventfd(0, EFD_NONBLOCK);
    if (VM.wake_efd < 0 || wo_engine_primary_inbox(VM.wake_efd) != 0) {
        fprintf(stderr, "wovm: cannot set up the primary shard\n");
        wo_vm_destroy(&VM);
        wo_module_free(&mod);
        return 2;
    }
    wo_tls_set(&VM);
    /* iteration 24: a write to a peer-closed socket must be EPIPE (a
     * catchable WO_T_IO), never a process-killing SIGPIPE — every
     * serving program writes to sockets whose peers vanish. */
    signal(SIGPIPE, SIG_IGN);
    /* The database engine boots with the VM: every class IS a table.
     * Durability is opt-in — WO_DATA=<dir> opens <dir>/shard-0.wal,
     * replays it before the entry runs (boot-before-listeners doctrine),
     * and every insert commits before it acknowledges. Without WO_DATA
     * the engine runs RAM-only, which is what the corpus expects.
     * Arc stage 3 obligation: this whole block runs BEFORE the worker
     * shards spawn — replay completes before anything can serve, and the
     * engine's immutable class-table pointer is published to the worker
     * threads by the spawn itself. The engine and WAL stay the PRIMARY's
     * alone (rt.db/rt.wal are never set on a worker); workers reach them
     * through the DB actor's message path. */
    if (wo_db_init(&DB, mod.classes, mod.class_cnt, 0, 1) != 0) {
        fprintf(stderr, "wovm: cannot initialize the database engine\n");
        wo_vm_destroy(&VM);
        wo_module_free(&mod);
        return 2;
    }
    VM.rt.db = &DB;
    const char *data_dir = getenv("WO_DATA");
    if (data_dir && data_dir[0]) {
        char wal_path[512];
        snprintf(wal_path, sizeof wal_path, "%s/shard-0.wal", data_dir);
        uint32_t vol_cid = 0;
        int64_t replayed = wo_wal_replay_ex(wal_path, &DB, &vol_cid);
        if (replayed == -2) {
            /* databasev2 2: not corruption — this log was written when the
             * table was durable and the source now says `durable: false`.
             * Refusing beats converting, and beats resurrecting rows into a
             * table declared not to have any. Name the class so the fix is
             * obvious. */
            /* wo_str.data is NOT NUL-terminated (obj.h), so the name must be
             * printed with an explicit length — %s here would over-read. */
            const char *cname = "?";
            int cnlen = 1;
            if (vol_cid < mod.class_cnt) {
                uint32_t k = mod.classes[vol_cid].name;
                if (k < mod.const_cnt && mod.consts[k].s) {
                    cname = mod.consts[k].s->data;
                    cnlen = (int)mod.consts[k].s->len;
                }
            }
            fprintf(stderr,
                    "wovm: %s: holds records for `%.*s`, which this program declares "
                    "`@table(durable: false)` — refusing to start. Either restore "
                    "`durable: true` for that table, or remove the data directory.\n",
                    wal_path, cnlen, cname);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        if (replayed < 0) {
            fprintf(stderr, "wovm: %s: replay found corruption beyond a torn tail\n", wal_path);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        if (wo_wal_open(&WAL, wal_path, 1u << 20) != 0) {
            fprintf(stderr, "wovm: cannot open %s\n", wal_path);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        VM.rt.wal = &WAL;
    }
    /* iteration 24: the one mailbox cap; WO_MAILBOX shrinks it in soak
     * tests to force the fail-fast policy (0/garbage keeps the default) */
    {
        const char *me = getenv("WO_MAILBOX");
        if (me && me[0]) {
            unsigned long v = strtoul(me, NULL, 10);
            if (v >= 1 && v <= 0x7FFFFFFFul) wo_mailbox_cap = (uint32_t)v;
        }
    }
    /* databasev2 3: the checkpoint policy. WO_CHECKPOINT_BYTES is the floor
     * below which a log is too small to bother compacting; WO_CHECKPOINT_RATIO
     * is how many times the live set's own size counts as too much history.
     * Both exist mainly so the policy is TESTABLE — a gate sets a tiny floor
     * and forces compaction in a few writes rather than waiting for megabytes.
     * There is no time-based trigger, by design: our records are durable at
     * commit, so an idle log does not grow. */
    {
        const char *cb = getenv("WO_CHECKPOINT_BYTES");
        if (cb && cb[0]) {
            unsigned long long v = strtoull(cb, NULL, 10);
            if (v > 0) wo_wal_ckpt_floor = (uint64_t)v;
        }
        const char *cr = getenv("WO_CHECKPOINT_RATIO");
        if (cr && cr[0]) {
            unsigned long v = strtoul(cr, NULL, 10);
            if (v <= 0xFFFFFFFFul) wo_wal_ckpt_ratio = (uint32_t)v;
        }
    }
    /* the arc's stage 2: all cores by default (the brave landing), one
     * pinned worker vm per extra core; WO_SHARDS caps or forces it */
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        uint32_t nshards = cores > 0 ? (uint32_t)cores : 1;
        const char *se = getenv("WO_SHARDS");
        if (se && se[0]) {
            unsigned long v = strtoul(se, NULL, 10);
            if (v >= 1 && v <= WO_MAX_SHARDS) nshards = (uint32_t)v;
        }
        if (nshards > WO_MAX_SHARDS) nshards = WO_MAX_SHARDS;
        wo_eng.shards = SHARDS;
        if (wo_engine_start(&mod, heap_mb << 20, nshards) != 0) {
            fprintf(stderr, "wovm: cannot start %u shards\n", nshards);
            wo_engine_stop();
            if (VM.rt.wal) { wal_stats_report(&WAL); wo_wal_close(&WAL); }
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
    }

    /* Program mode: an entry that declares one parameter gets the program's
     * OWN arguments as a `multi Text` — not the program name, and not the
     * image path a plain `wovm image.wob args...` invocation carries. So
     * `args[0]` is the first real argument (the workload's own contract:
     * `args[0] == "watch"`, `args[1]` the log file). An entry with no
     * parameters is called exactly as before. */
    uint64_t argv_val = 0;
    uint32_t entry_argc = mod.methods[mod.entry].arg_cnt;
    if (entry_argc == 1) {
        wo_multi *args = wo_multi_new(&VM.rt, WO_K_TEXT);
        if (!args) {
            fprintf(stderr, "wovm: cannot allocate the argument list\n");
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        int first = self_rc == 0 ? 2 : 1; /* skip the image path when there is one */
        for (int i = first; i < argc; i++) {
            wo_str *s = wo_str_new(&VM.rt, argv[i], (uint32_t)strlen(argv[i]));
            if (!s || wo_multi_push(args, (uint64_t)(uintptr_t)s) != 0) {
                fprintf(stderr, "wovm: cannot allocate the argument list\n");
                wo_vm_destroy(&VM);
                wo_module_free(&mod);
                return 2;
            }
        }
        argv_val = (uint64_t)(uintptr_t)args;
    }
    uint64_t ret = 0;
    wo_err terr;
    int rc = wo_vm_call(&VM, mod.entry, entry_argc == 1 ? &argv_val : NULL, entry_argc, &ret,
                        &terr);
    if (rc < 0)
        fprintf(stderr, "trap %u in %s at line %u: %s\n", (unsigned)terr.code,
                terr.method, (unsigned)terr.line, terr.msg);
    /* the entry's return value IS the exit code (docs/plan/oop-vm/
     * 08-builtin-surface.md's "Program entry"): 0..255, a trap is 1. A stop
     * (rc == 1) is not a failure and not a trap — SIGTERM landing in a
     * blocking call is the operator asking for the shutdown the program
     * would have taken at its own next `env.stopping()` check, so it exits
     * with the status that check's `return 0` would have produced. */
    int exit_code = rc == 0 ? (int)((uint64_t)ret & 0xFF) : (rc > 0 ? 0 : 1);
    /* the entry only BORROWS its arguments -- a parameter is never a `take`,
     * and the drop tables never drop one -- so the runtime that built the
     * container is the one that releases it, elements included. Without this
     * the workload reported a leak on every path, in every mode, which is
     * exactly the noise a soak measurement cannot afford. It runs before the
     * heap is torn down, and after a trap too: the container outlives the
     * unwind. */
    if (argv_val) wo_drop_kind(&VM.rt, WO_K_MULTI, argv_val);
    wo_engine_stop(); /* join + destroy the worker shards before the primary */
    if (VM.rt.wal) { wal_stats_report(&WAL); wo_wal_close(&WAL); }
    wo_db_destroy(&DB);
    gc_pump(&VM);
    wo_vm_destroy(&VM);
    wo_module_free(&mod);
    return exit_code;
}

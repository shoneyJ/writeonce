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
/* databasev2 12: the COMPILED schema, names resolved out of the constant
 * pool — the database layer never sees consts, so this is where byte-pointer
 * names come from. Field metadata may be absent on hand-built images; the
 * compiler always emits it, and a name that is genuinely missing becomes the
 * empty string, which still round-trips (an unchanged schema compares equal
 * byte for byte). */
static int mig_build_schema(const wo_module *mod, wo_schema *sc) {
    memset(sc, 0, sizeof *sc);
    sc->class_cnt = mod->class_cnt;
    sc->classes = calloc(mod->class_cnt ? mod->class_cnt : 1, sizeof *sc->classes);
    if (!sc->classes) return -1;
    for (uint32_t c = 0; c < mod->class_cnt; c++) {
        const wo_classdesc *k = &mod->classes[c];
        wo_schema_class *o = &sc->classes[c];
        if (k->name < mod->const_cnt && mod->consts[k->name].s) {
            o->name = (const uint8_t *)mod->consts[k->name].s->data;
            o->name_len = mod->consts[k->name].s->len;
        }
        o->flags = k->flags & (WO_CLASSF_VOLATILE | WO_CLASSF_RESIDENT_KEYS);
        o->field_cnt = k->field_cnt;
        o->fields = calloc(k->field_cnt ? k->field_cnt : 1, sizeof *o->fields);
        if (!o->fields) return -1;
        for (uint32_t f = 0; f < k->field_cnt; f++) {
            wo_schema_field *fl = &o->fields[f];
            if (k->field_names && k->field_names[f] < mod->const_cnt &&
                mod->consts[k->field_names[f]].s) {
                fl->name = (const uint8_t *)mod->consts[k->field_names[f]].s->data;
                fl->name_len = mod->consts[k->field_names[f]].s->len;
            }
            fl->kind = k->kinds[f];
            fl->fclass = k->field_class ? k->field_class[f] : WO_SCHEMA_NONE;
            fl->felem = k->field_elem ? k->field_elem[f] : WO_SCHEMA_NONE;
        }
    }
    return 0;
}
static void mig_free_schema(wo_schema *sc) {
    if (!sc->classes) return;
    for (uint32_t c = 0; c < sc->class_cnt; c++) free(sc->classes[c].fields);
    free(sc->classes);
    sc->classes = NULL;
}

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
     * Durability is the default — WO_DATA=<dir> opens <dir>/shard-0.wal,
     * replays it before the entry runs (boot-before-listeners doctrine),
     * and every insert commits before it acknowledges. A program with any
     * durable @table (the default) refuses to start without WO_DATA;
     * WO_EPHEMERAL=1 opts into a RAM-only run (the corpus's mode), and
     * @table(durable: false) opts a table out. A class without @table is
     * storage for the engine but never a durable table (v8 WO_CLASSF_TABLE).
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
    DB.rt = &VM.rt; /* databasev2 2 (5c): the loop a borrow reads the WAL through */
    const char *data_dir = getenv("WO_DATA");
    const char *ephemeral = getenv("WO_EPHEMERAL");
    if (data_dir && data_dir[0] && ephemeral) {
        /* databasev2 2 (6a): the two knobs answer the same question with
         * opposite answers — refuse rather than pick one silently. */
        fprintf(stderr, "wovm: WO_EPHEMERAL=1 is incompatible with WO_DATA\n");
        wo_db_destroy(&DB);
        wo_vm_destroy(&VM);
        wo_module_free(&mod);
        return 2;
    }
    if (!data_dir || !data_dir[0]) {
        /* databasev2 3: the loader used to refuse `resident: keys` outright;
         * now it is accepted because UPDATE landed, but every row still
         * lives in the log, not RAM — refuse the same way the loader's own
         * durable:false+resident:keys refusal does, rather than let reads
         * silently misbehave with no WAL to fold from. */
        for (uint32_t i = 0; i < mod.class_cnt; i++) {
            /* v8: the storage bits are a @table's; the loader refuses them
             * on anything else, so the table check here is belt and braces */
            if (!(mod.classes[i].flags & WO_CLASSF_TABLE)) continue;
            if (!(mod.classes[i].flags & WO_CLASSF_RESIDENT_KEYS)) continue;
            const char *cname = "?";
            int cnlen = 1;
            uint32_t k = mod.classes[i].name;
            if (k < mod.const_cnt && mod.consts[k].s) {
                cname = mod.consts[k].s->data;
                cnlen = (int)mod.consts[k].s->len;
            }
            fprintf(stderr,
                    "wovm: `%.*s` is declared `resident: keys` — its rows live only "
                    "in the write-ahead log, so it cannot run without WO_DATA.\n",
                    cnlen, cname);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        /* databasev2 2 (6a): a durable table (the default) without WO_DATA
         * used to run RAM-only and drop every write at exit — the one
         * outcome `durable: true` promises against. Refuse by name unless
         * the developer opted into RAM-only explicitly. Startup-only:
         * db.c's `w && table_is_durable` guards are untouched, so the RAM
         * path under WO_EPHEMERAL=1 is byte-for-byte the old one. After the
         * keys loop on purpose: a keys-resident table has nowhere to read
         * from at all, which is the more specific refusal and is not
         * rescued by WO_EPHEMERAL. `durable: true` is a @table property (v8
         * WO_CLASSF_TABLE): a plain class, variant or predeclared record is
         * not a table, so a program with no durable table starts as it
         * always did and WO_EPHEMERAL is not consulted at all. */
        uint32_t first_durable = mod.class_cnt;
        for (uint32_t i = 0; i < mod.class_cnt; i++) {
            if (!(mod.classes[i].flags & WO_CLASSF_TABLE)) continue;
            if (mod.classes[i].flags & WO_CLASSF_VOLATILE) continue;
            first_durable = i;
            break;
        }
        if (first_durable < mod.class_cnt && ephemeral && strcmp(ephemeral, "1") != 0) {
            fprintf(stderr,
                    "wovm: WO_EPHEMERAL=%s is not accepted — the only accepted "
                    "value is WO_EPHEMERAL=1.\n",
                    ephemeral);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        if (first_durable < mod.class_cnt && ephemeral) {
            fprintf(stderr,
                    "wovm: WO_EPHEMERAL=1 — durable tables served from RAM, "
                    "nothing is written.\n");
        } else if (first_durable < mod.class_cnt) {
            const char *cname = "?";
            int cnlen = 1;
            uint32_t k = mod.classes[first_durable].name;
            if (k < mod.const_cnt && mod.consts[k].s) {
                cname = mod.consts[k].s->data;
                cnlen = (int)mod.consts[k].s->len;
            }
            fprintf(stderr,
                    "wovm: `%.*s` is a durable table (the default) and WO_DATA "
                    "is not set — set WO_DATA=<dir or file> to keep its rows, "
                    "WO_EPHEMERAL=1 to run RAM-only, or declare it "
                    "@table(durable: false).\n",
                    cnlen, cname);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
    }
    wo_schema compiled_schema;
    int have_schema = 0;
    if (data_dir && data_dir[0]) {
        char wal_path[512];
        /* databasev2 7: WO_DATA is a directory (→ <dir>/shard-0.wal, as it
         * always was) or THE log file, created if absent. Refuse rather than
         * guess: a missing parent is never created, and a path that is
         * neither a regular file nor a directory is not a store. */
        int prc = wo_wal_resolve_data_path(data_dir, wal_path, sizeof wal_path);
        if (prc != 0) {
            if (prc == WO_WAL_PATH_NO_PARENT)
                fprintf(stderr,
                        "wovm: WO_DATA=%s — its parent %s is not an existing "
                        "directory; create it first (wovm never runs mkdir -p).\n",
                        data_dir, wal_path);
            else if (prc == WO_WAL_PATH_NOT_A_FILE)
                fprintf(stderr,
                        "wovm: WO_DATA=%s exists but is neither a regular file "
                        "nor a directory.\n",
                        data_dir);
            else
                fprintf(stderr, "wovm: WO_DATA=%s is too long for a path.\n", data_dir);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        /* databasev2 12: the log's head record states the shape that wrote
         * it. Diff it against the compiled classes BEFORE replay: a matching
         * shape replays as-is, add/delete migrates the log in place through
         * a compaction-style rewrite, anything else refuses by name. A log
         * with no head record is a legacy log — nothing recorded, nothing
         * diffable; replay's own decode remains its only check, exactly as
         * before this iteration. */
        if (mig_build_schema(&mod, &compiled_schema) != 0) {
            fprintf(stderr, "wovm: out of memory building the schema\n");
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        have_schema = 1;
        uint8_t *head = NULL;
        uint32_t head_len = 0;
        int hrc = wo_wal_read_schema(wal_path, &head, &head_len);
        if (hrc == 0) {
            wo_schema *stored = wo_schema_decode(head, head_len);
            free(head);
            if (!stored) {
                fprintf(stderr, "wovm: %s: the schema record is malformed\n", wal_path);
                mig_free_schema(&compiled_schema);
                wo_db_destroy(&DB);
                wo_vm_destroy(&VM);
                wo_module_free(&mod);
                return 2;
            }
            wo_mig_plan plan;
            if (wo_schema_diff(stored, &compiled_schema, &plan) != 0) {
                fprintf(stderr, "wovm: out of memory diffing schemas\n");
                wo_schema_free(stored);
                mig_free_schema(&compiled_schema);
                wo_db_destroy(&DB);
                wo_vm_destroy(&VM);
                wo_module_free(&mod);
                return 2;
            }
            if (!plan.identity) {
                /* say what is about to change, per class, before doing it */
                for (uint32_t c = 0; c < plan.old_class_cnt; c++) {
                    if (!plan.classes[c].changed) continue;
                    const wo_schema_class *ok = &stored->classes[c];
                    fprintf(stderr, "wovm: %s: migrating `%.*s`:", wal_path,
                            (int)ok->name_len, (const char *)ok->name);
                    /* A poisoned class (a referenced/nested type changed, or a
                     * field's type is incompatible) has fmap == NULL and
                     * new_cid == NONE — the field-level diff does not apply.
                     * Dereferencing fmap here was a NULL read (SEGV); name the
                     * situation and let wo_wal_migrate below refuse cleanly. */
                    if (plan.classes[c].fmap) {
                        for (uint32_t f = 0; f < ok->field_cnt; f++)
                            if (plan.classes[c].fmap[f] == -1)
                                fprintf(stderr, " -%.*s", (int)ok->fields[f].name_len,
                                        (const char *)ok->fields[f].name);
                        const wo_schema_class *nk =
                            &compiled_schema.classes[plan.classes[c].new_cid];
                        for (uint32_t f = 0; f < nk->field_cnt; f++) {
                            int found = 0;
                            for (uint32_t g = 0; g < ok->field_cnt && !found; g++)
                                found = plan.classes[c].fmap[g] == (int32_t)f;
                            if (!found)
                                fprintf(stderr, " +%.*s", (int)nk->fields[f].name_len,
                                        (const char *)nk->fields[f].name);
                        }
                    } else {
                        fprintf(stderr, " (a referenced type changed — cannot migrate in place)");
                    }
                    fprintf(stderr, "\n");
                }
                char *merr = NULL;
                int mrc = wo_wal_migrate(wal_path, &DB, stored, &plan,
                                         &compiled_schema, 1u << 20, &merr);
                if (mrc != 0) {
                    if (mrc == -2 && merr)
                        fprintf(stderr, "wovm: %s: refusing to start — %s\n",
                                wal_path, merr);
                    else
                        fprintf(stderr,
                                "wovm: %s: migration found corruption beyond a "
                                "torn tail\n",
                                wal_path);
                    free(merr);
                    wo_mig_plan_free(&plan);
                    wo_schema_free(stored);
                    mig_free_schema(&compiled_schema);
                    wo_db_destroy(&DB);
                    wo_vm_destroy(&VM);
                    wo_module_free(&mod);
                    return 2;
                }
                fprintf(stderr, "wovm: %s: schema migrated\n", wal_path);
            }
            wo_mig_plan_free(&plan);
            wo_schema_free(stored);
        } else if (hrc < 0) {
            fprintf(stderr, "wovm: cannot read %s\n", wal_path);
            mig_free_schema(&compiled_schema);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
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
            if (have_schema) mig_free_schema(&compiled_schema);
            wo_db_destroy(&DB);
            wo_vm_destroy(&VM);
            wo_module_free(&mod);
            return 2;
        }
        /* databasev2 12: the live log carries the compiled shape from here
         * on — a fresh log gets it as its first record now, a legacy one at
         * its next compaction. */
        /* lazily written ahead of the first record (stage()), so a program
         * whose tables are all volatile keeps its documented zero WAL bytes */
        (void)wo_wal_set_schema(&WAL, &compiled_schema);
        VM.rt.wal = &WAL;
    }
    if (have_schema) mig_free_schema(&compiled_schema);
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

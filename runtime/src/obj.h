/* obj.h — per-shard arena allocator (spec section 4). Object model and
 * strings extend this header in the next task. */
#ifndef WO_OBJ_H
#define WO_OBJ_H

#include "wob.h"

/* Arena: one malloc'd region, bump allocation, 16-byte-granular size-class
 * free lists up to 1024 bytes (freed blocks chain through their first word).
 * Larger sizes use plain malloc/free — the caller always passes the size
 * back on free, so no size headers exist anywhere. Exhaustion returns NULL;
 * the VM maps that to WO_T_OOM, never an abort. */
#define WO_ARENA_MAX_CLASS 1024u
#define WO_ARENA_NCLASSES (WO_ARENA_MAX_CLASS / 16u) /* 64 lists: 16..1024 */

typedef struct wo_arena {
    uint8_t *base;   /* malloc'd region */
    size_t cap;      /* region capacity in bytes */
    size_t used;     /* bump offset */
    void *freelist[WO_ARENA_NCLASSES]; /* index = size/16 - 1 */
} wo_arena;

int wo_arena_init(wo_arena *a, size_t cap); /* 0 ok, -1 malloc failure */
void wo_arena_destroy(wo_arena *a);
void *wo_arena_alloc(wo_arena *a, size_t size); /* NULL = region OOM */
void wo_arena_free(wo_arena *a, void *p, size_t size);

/* Collector phase (iteration 7b, gc.c). IDLE -> MARK at the heap-goal
 * trigger; MARK -> SWEEP when the gray worklist drains; SWEEP -> IDLE when
 * the traced-list cursor reaches the end. The Yuasa deletion barrier is
 * active during MARK only. */
enum { WO_GC_IDLE = 0, WO_GC_MARK = 1, WO_GC_SWEEP = 2 };

/* Runtime context: what every module needs. One per shard (one total in
 * milestone 1): the arena, the loaded class table, the tracing collector's
 * state (gc.c), and the output stream builtin print writes to (tests point
 * it at a temp file to capture output). */
typedef struct wo_rt {
    uint16_t shard_id; /* arc T6: stamped into every allocation's header;
                          a drop whose header disagrees routes home */
    wo_arena arena;
    const wo_classdesc *classes;
    uint32_t class_cnt;
    /* ---- tracing collector (iteration 7b) ---- */
    wo_hdr *gc_traced;    /* per-shard traced list: every live traced object */
    size_t gc_traced_cnt; /* list length (trace/reporting only) */
    struct {
        wo_hdr **items; /* gray worklist: traced objects awaiting a scan */
        size_t len, cap;
        int oom; /* worklist realloc failed: finish the cycle freeing nothing */
    } gc_gray;
    int gc_phase;          /* WO_GC_IDLE / MARK / SWEEP */
    wo_hdr **gc_sweep;     /* SWEEP: link slot the cursor resumes at */
    size_t gc_alloc_bytes; /* traced bytes since the last cycle (trigger) */
    size_t gc_goal;        /* start a cycle past this many traced bytes */
    size_t gc_budget;      /* objects processed per slice (WO_GC_BUDGET) */
    int gc_trace;          /* WO_GC_TRACE: one stderr line per slice */
    size_t gc_step_no;     /* slices run so far (the trace's step counter) */
    uint8_t *gc_may;       /* per-class "may transitively hold a gcref" bit —
                              lets mark skip owned subtrees that cannot reach
                              a traced object. Computed at init; NULL =
                              conservative (traverse everything). */
    void *out; /* FILE*; kept void* so obj.h needn't pull in stdio */
    /* the database engine's handles (database/src), opaque here so the VM
       core needn't include engine headers: db = wo_db*, wal = wo_wal*.
       NULL = engine absent (test binaries) / durability off (no WO_DATA).
       Set by main.c at boot; db.c casts. */
    void *db;
    void *wal;
} wo_rt;

int wo_rt_init(wo_rt *rt, size_t heap_cap, const wo_classdesc *classes,
               uint32_t class_cnt); /* 0 ok, -1 alloc failure; out = stdout */
void wo_rt_destroy(wo_rt *rt);

/* New zeroed instance of a class-table class. A traced (inferred-gc) class
 * instance links itself onto the traced list, born white when the collector
 * is idle and black during a cycle (live-at-birth for that cycle). NULL =
 * OOM (VM traps WO_T_OOM). */
wo_hdr *wo_obj_new(wo_rt *rt, uint32_t class_id);

/* Strings: header + length + inline bytes, class id WO_CLS_STR. */
typedef struct wo_str {
    wo_hdr h;
    uint32_t len;
    char data[]; /* len bytes, no NUL */
} wo_str;

wo_str *wo_str_new(wo_rt *rt, const char *bytes, uint32_t len); /* NULL=OOM */
/* A Text of [len] UNINITIALIZED bytes for a caller that writes them itself
 * (the systems-stdlib `join`, which knows the total length up front and
 * would otherwise need one allocation per element). NULL = OOM. */
wo_str *wo_str_alloc(wo_rt *rt, uint32_t len);
wo_str *wo_str_concat(wo_rt *rt, const wo_str *a, const wo_str *b);
int wo_str_eq(const wo_str *a, const wo_str *b); /* content equality */
void wo_str_free(wo_rt *rt, wo_str *s);          /* no-op on WO_F_CONST */

/* iteration 19: Bytes. Byte-for-byte the wo_str object — same struct, same
 * allocator, same free path (gc.c handles both class ids) — differing only in
 * the header's class_id, which is what every Text builtin checks. There is no
 * wo_bytes_free: wo_str_free is it. NULL = OOM on both. */
wo_str *wo_bytes_alloc(wo_rt *rt, uint32_t len); /* UNINITIALIZED bytes */
wo_str *wo_bytes_new(wo_rt *rt, const char *bytes, uint32_t len);

#endif /* WO_OBJ_H */

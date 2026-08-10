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

/* Runtime context: what every module needs. One per shard (one total in
 * milestone 1): the arena, the loaded class table, the cycle-candidate
 * buffer (filled by gc.c), and the output stream builtin print writes to
 * (tests point it at a temp file to capture output). */
typedef struct wo_rt {
    wo_arena arena;
    const wo_classdesc *classes;
    uint32_t class_cnt;
    struct {
        wo_hdr **items;
        size_t len, cap;
    } cycbuf;
    void *out; /* FILE*; kept void* so obj.h needn't pull in stdio */
} wo_rt;

int wo_rt_init(wo_rt *rt, size_t heap_cap, const wo_classdesc *classes,
               uint32_t class_cnt); /* 0 ok, -1 alloc failure; out = stdout */
void wo_rt_destroy(wo_rt *rt);

/* New zeroed instance of a class-table class. @gc classes get the GC flag
 * and rc 1 (the creating reference). NULL = OOM (VM traps WO_T_OOM). */
wo_hdr *wo_obj_new(wo_rt *rt, uint32_t class_id);

/* Strings: header + length + inline bytes, class id WO_CLS_STR. */
typedef struct wo_str {
    wo_hdr h;
    uint32_t len;
    char data[]; /* len bytes, no NUL */
} wo_str;

wo_str *wo_str_new(wo_rt *rt, const char *bytes, uint32_t len); /* NULL=OOM */
wo_str *wo_str_concat(wo_rt *rt, const wo_str *a, const wo_str *b);
int wo_str_eq(const wo_str *a, const wo_str *b); /* content equality */
void wo_str_free(wo_rt *rt, wo_str *s);          /* no-op on WO_F_CONST */

#endif /* WO_OBJ_H */

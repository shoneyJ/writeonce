#include "obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h" /* wo_gc_track (traced allocations), wo_drop_obj (teardown) */

static size_t round16(size_t n) { return (n + 15u) & ~(size_t)15u; }

int wo_arena_init(wo_arena *a, size_t cap) {
    memset(a, 0, sizeof(*a));
    a->base = malloc(cap ? cap : 1);
    if (!a->base) return -1;
    a->cap = cap;
    return 0;
}

void wo_arena_destroy(wo_arena *a) {
    free(a->base);
    memset(a, 0, sizeof(*a));
}

void *wo_arena_alloc(wo_arena *a, size_t size) {
    size = round16(size ? size : 1);
    if (size > WO_ARENA_MAX_CLASS) return malloc(size);
    size_t cls = size / 16u - 1u;
    if (a->freelist[cls]) {
        void *p = a->freelist[cls];
        memcpy(&a->freelist[cls], p, sizeof(void *));
        return p;
    }
    if (a->used + size > a->cap) return NULL; /* region OOM -> trap upstream */
    void *p = a->base + a->used;
    a->used += size;
    return p;
}

void wo_arena_free(wo_arena *a, void *p, size_t size) {
    if (!p) return;
    size = round16(size ? size : 1);
    if (size > WO_ARENA_MAX_CLASS) {
        free(p);
        return;
    }
    size_t cls = size / 16u - 1u;
    memcpy(p, &a->freelist[cls], sizeof(void *));
    a->freelist[cls] = p;
}

/* Per-class "may transitively hold a gcref" fixpoint (iteration 7b): lets
 * mark skip owned subtrees that cannot reach a traced object. Field kinds
 * alone identify direct gcrefs; for owned/container fields the referenced
 * class comes from the v2 field_class metadata when present — absent or
 * unknown answers conservatively (may = 1), which is always safe. */
static void compute_gc_may(wo_rt *rt) {
    if (rt->class_cnt == 0) return;
    rt->gc_may = calloc(rt->class_cnt, 1);
    if (!rt->gc_may) return; /* NULL = conservative everywhere */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (uint32_t c = 0; c < rt->class_cnt; c++) {
            if (rt->gc_may[c]) continue;
            const wo_classdesc *cd = &rt->classes[c];
            int may = 0;
            for (uint32_t i = 0; i < cd->field_cnt && !may; i++) {
                uint8_t k = cd->kinds[i];
                if (k == WO_K_GCREF) {
                    may = 1;
                } else if (k == WO_K_OWNED || k == WO_K_MULTI || k == WO_K_MAP) {
                    if (k != WO_K_OWNED && cd->field_elem &&
                        cd->field_elem[i] == WO_K_GCREF) {
                        may = 1;
                    } else if (cd->field_class && cd->field_class[i] < rt->class_cnt) {
                        if (rt->gc_may[cd->field_class[i]]) may = 1;
                    } else {
                        may = 1; /* referenced class unknown: conservative */
                    }
                }
            }
            if (may) {
                rt->gc_may[c] = 1;
                changed = 1;
            }
        }
    }
}

int wo_rt_init(wo_rt *rt, size_t heap_cap, const wo_classdesc *classes,
               uint32_t class_cnt) {
    memset(rt, 0, sizeof(*rt));
    if (wo_arena_init(&rt->arena, heap_cap) != 0) return -1;
    rt->classes = classes;
    rt->class_cnt = class_cnt;
    rt->out = stdout;
    /* collector knobs (iteration 7b). WO_GC_BUDGET = objects per slice;
     * WO_GC_GOAL = traced bytes that trigger a cycle; WO_GC_TRACE = one
     * stderr line per slice. Defaults: budget 64 (mirrors WO_HEAP_MB's
     * default; small enough that an oversized abandoned structure visibly
     * takes more than one slice), goal 256 KiB. */
    rt->gc_budget = 64;
    rt->gc_goal = 256u << 10;
    const char *e;
    if ((e = getenv("WO_GC_BUDGET")) && e[0]) {
        char *end = NULL;
        unsigned long v = strtoul(e, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 1000000) rt->gc_budget = v;
    }
    if ((e = getenv("WO_GC_GOAL")) && e[0]) {
        char *end = NULL;
        unsigned long v = strtoul(e, &end, 10);
        if (end && *end == '\0' && v >= 1) rt->gc_goal = v;
    }
    rt->gc_trace = getenv("WO_GC_TRACE") != NULL;
    compute_gc_may(rt);
    /* Line-buffered, always: a long-running program (the driving workload's
     * `watch`/`run`/`mcp` modes) writes progress with `print`, and stdio's
     * default full buffering when stdout is a file or a pipe meant that output
     * sat in a buffer until exit — so a redirected service looked silent, and
     * a killed one lost its log entirely. Content is unchanged, so every
     * byte-exact fixture still compares equal. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}

void wo_rt_destroy(wo_rt *rt) {
    /* free whatever the collector still tracks — the post-exit pump should
     * have emptied the list, but a teardown after a trap or a test that
     * never pumped must still be leak-free. Sweep-order free is safe: a
     * traced object's gcref edges are no-ops in class_free (phase is idle),
     * and its owned interior has exactly one owner — this object. */
    while (rt->gc_traced) {
        wo_hdr *o = rt->gc_traced;
        rt->gc_traced = o->gclink;
        wo_drop_obj(rt, o);
    }
    free(rt->gc_gray.items);
    free(rt->gc_may);
    wo_arena_destroy(&rt->arena);
    memset(rt, 0, sizeof(*rt));
}

wo_hdr *wo_obj_new(wo_rt *rt, uint32_t class_id) {
    const wo_classdesc *c = &rt->classes[class_id];
    size_t sz = wo_obj_size(c);
    wo_hdr *o = wo_arena_alloc(&rt->arena, sz);
    if (!o) return NULL;
    memset(o, 0, sz); /* color: WHITE by construction (all-zero) */
    o->class_id = class_id;
    o->shard_id = rt->shard_id;
    if (c->flags & WO_CLASSF_GC) {
        o->flags = WO_F_GC;
        /* born black while a cycle runs: live-at-birth for that cycle */
        if (rt->gc_phase != WO_GC_IDLE) o->flags |= WO_COLOR_BLACK;
        wo_gc_track(rt, o, sz);
    }
    return o;
}

wo_str *wo_str_alloc(wo_rt *rt, uint32_t len) {
    wo_str *s = wo_arena_alloc(&rt->arena, sizeof(wo_str) + len);
    if (!s) return NULL;
    memset(&s->h, 0, sizeof(s->h));
    s->h.class_id = WO_CLS_STR;
    s->h.shard_id = rt->shard_id;
    s->len = len;
    return s;
}

wo_str *wo_str_new(wo_rt *rt, const char *bytes, uint32_t len) {
    wo_str *s = wo_str_alloc(rt, len);
    if (!s) return NULL;
    memcpy(s->data, bytes, len);
    return s;
}

wo_str *wo_str_concat(wo_rt *rt, const wo_str *a, const wo_str *b) {
    wo_str *s = wo_str_alloc(rt, a->len + b->len);
    if (!s) return NULL;
    memcpy(s->data, a->data, a->len);
    memcpy(s->data + a->len, b->data, b->len);
    return s;
}

int wo_str_eq(const wo_str *a, const wo_str *b) {
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

void wo_str_free(wo_rt *rt, wo_str *s) {
    if (!s || (s->h.flags & WO_F_CONST)) return; /* interned: outlives all */
    wo_arena_free(&rt->arena, s, sizeof(wo_str) + s->len);
}

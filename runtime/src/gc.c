#include "gc.h"

#include <stdlib.h>

#include "cont.h"

static void multi_free(wo_rt *rt, wo_multi *m) {
    for (uint32_t i = 0; i < m->len; i++)
        wo_drop_kind(rt, m->elem_kind, m->items[i]);
    free(m->items);
    wo_arena_free(&rt->arena, m, sizeof(wo_multi));
}

static void map_free(wo_rt *rt, wo_map *m) {
    for (uint32_t i = 0; i < m->len; i++) {
        wo_drop_kind(rt, m->key_kind, m->keys[i]);
        wo_drop_kind(rt, m->val_kind, m->vals[i]);
    }
    free(m->keys);
    free(m->vals);
    wo_arena_free(&rt->arena, m, sizeof(wo_map));
}

/* release a class object's contents then the object itself */
static void class_free(wo_rt *rt, wo_hdr *o) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    uint64_t *f = wo_fields(o);
    for (uint32_t i = 0; i < c->field_cnt; i++)
        wo_drop_kind(rt, c->kinds[i], f[i]);
    wo_arena_free(&rt->arena, o, wo_obj_size(c));
}

void wo_drop_obj(wo_rt *rt, wo_hdr *o) {
    if (!o) return;
    switch (o->class_id) {
    case WO_CLS_STR:
        wo_str_free(rt, (wo_str *)o);
        return;
    case WO_CLS_MULTI:
        multi_free(rt, (wo_multi *)o);
        return;
    case WO_CLS_MAP:
        map_free(rt, (wo_map *)o);
        return;
    default:
        class_free(rt, o);
        return;
    }
}

void wo_drop_kind(wo_rt *rt, uint8_t kind, uint64_t v) {
    if (!v) return; /* null values ignored for every kind */
    switch (kind) {
    case WO_K_SCALAR:
        return;
    case WO_K_OWNED:
    case WO_K_MULTI:
    case WO_K_MAP:
        wo_drop_obj(rt, (wo_hdr *)(uintptr_t)v);
        return;
    case WO_K_GCREF:
        wo_rc_dec(rt, (wo_hdr *)(uintptr_t)v);
        return;
    case WO_K_TEXT:
        wo_str_free(rt, (wo_str *)(uintptr_t)v);
        return;
    default:
        return; /* loader guarantees kinds; defensive no-op */
    }
}

void wo_rc_inc(wo_hdr *o) { o->rc++; }

/* ---- cycle-candidate buffer ---- */

/* only classes that can point at other @gc objects can close a cycle */
static int class_possibly_cyclic(const wo_rt *rt, const wo_hdr *o) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        uint8_t k = c->kinds[i];
        if (k == WO_K_GCREF || k == WO_K_MULTI || k == WO_K_MAP) return 1;
    }
    return 0;
}

static void buf_push(wo_rt *rt, wo_hdr *o) {
    if (rt->cycbuf.len == rt->cycbuf.cap) {
        size_t ncap = rt->cycbuf.cap ? rt->cycbuf.cap * 2 : 16;
        wo_hdr **ni = realloc(rt->cycbuf.items, ncap * sizeof(wo_hdr *));
        if (!ni) return; /* can't buffer: conservative leak, never corrupt */
        rt->cycbuf.items = ni;
        rt->cycbuf.cap = ncap;
    }
    o->flags |= WO_F_BUF;
    rt->cycbuf.items[rt->cycbuf.len++] = o;
}

/* swap-remove a specific object from the buffer (whites purged mid-step) */
static void buf_remove(wo_rt *rt, wo_hdr *o) {
    for (size_t i = 0; i < rt->cycbuf.len; i++) {
        if (rt->cycbuf.items[i] == o) {
            rt->cycbuf.items[i] = rt->cycbuf.items[--rt->cycbuf.len];
            return;
        }
    }
}

void wo_rc_dec(wo_rt *rt, wo_hdr *o) {
    o->rc--;
    if (o->rc == 0) {
        /* zombie guard: a buffered candidate's death belongs to the cycle
         * collector — it will notice rc 0 and free it in its own sweep */
        if (o->flags & WO_F_BUF) return;
        class_free(rt, o);
        return;
    }
    /* survived a decrement and can sit on a cycle: buffer as a candidate,
     * deduplicated by the flag (Bacon–Rajan possible-root heuristic) */
    if (!(o->flags & WO_F_BUF) && class_possibly_cyclic(rt, o))
        buf_push(rt, o);
}

/* ---- budgeted Bacon–Rajan trial deletion ---- */

static uint8_t color_of(const wo_hdr *o) { return o->flags & WO_F_COLOR; }
static void set_color(wo_hdr *o, uint8_t c) {
    o->flags = (uint8_t)((o->flags & ~WO_F_COLOR) | c);
}

/* visit every @gc edge out of a class object: gcref fields, plus gcref
 * elements inside multi/map fields */
typedef void (*child_fn)(wo_rt *rt, wo_hdr *child, void *ctx);
static void visit_children(wo_rt *rt, wo_hdr *o, child_fn fn, void *ctx) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    uint64_t *f = wo_fields(o);
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        uint8_t k = c->kinds[i];
        if (k == WO_K_GCREF) {
            if (f[i]) fn(rt, (wo_hdr *)(uintptr_t)f[i], ctx);
        } else if (k == WO_K_MULTI) {
            wo_multi *m = (wo_multi *)(uintptr_t)f[i];
            if (m && m->elem_kind == WO_K_GCREF)
                for (uint32_t j = 0; j < m->len; j++)
                    if (m->items[j]) fn(rt, (wo_hdr *)(uintptr_t)m->items[j], ctx);
        } else if (k == WO_K_MAP) {
            wo_map *mp = (wo_map *)(uintptr_t)f[i];
            if (!mp) continue;
            if (mp->key_kind == WO_K_GCREF)
                for (uint32_t j = 0; j < mp->len; j++)
                    if (mp->keys[j]) fn(rt, (wo_hdr *)(uintptr_t)mp->keys[j], ctx);
            if (mp->val_kind == WO_K_GCREF)
                for (uint32_t j = 0; j < mp->len; j++)
                    if (mp->vals[j]) fn(rt, (wo_hdr *)(uintptr_t)mp->vals[j], ctx);
        }
    }
}

static void mark_gray(wo_rt *rt, wo_hdr *o);
static void mark_gray_child(wo_rt *rt, wo_hdr *c, void *ctx) {
    (void)ctx;
    c->rc--; /* trial-delete this edge */
    mark_gray(rt, c);
}
static void mark_gray(wo_rt *rt, wo_hdr *o) {
    if (color_of(o) == WO_COLOR_GRAY) return;
    set_color(o, WO_COLOR_GRAY);
    visit_children(rt, o, mark_gray_child, NULL);
}

static void scan_black(wo_rt *rt, wo_hdr *o);
static void scan_black_child(wo_rt *rt, wo_hdr *c, void *ctx) {
    (void)ctx;
    c->rc++; /* restore the trial-deleted edge */
    if (color_of(c) != WO_COLOR_BLACK) scan_black(rt, c);
}
static void scan_black(wo_rt *rt, wo_hdr *o) {
    set_color(o, WO_COLOR_BLACK);
    visit_children(rt, o, scan_black_child, NULL);
}

static void scan(wo_rt *rt, wo_hdr *o, void *ctx);
static void scan_(wo_rt *rt, wo_hdr *o) {
    if (color_of(o) != WO_COLOR_GRAY) return;
    if (o->rc > 0) {
        scan_black(rt, o); /* externally held: restore the whole subgraph */
        return;
    }
    set_color(o, WO_COLOR_WHITE);
    visit_children(rt, o, scan, NULL);
}
static void scan(wo_rt *rt, wo_hdr *o, void *ctx) {
    (void)ctx;
    scan_(rt, o);
}

/* gather whites into a step-local list (post-marking, pre-free) */
typedef struct {
    wo_hdr **items;
    size_t len, cap;
    int oom;
} whites_t;

static void collect_white(wo_rt *rt, wo_hdr *o, void *ctx) {
    whites_t *w = ctx;
    if (color_of(o) != WO_COLOR_WHITE) return;
    set_color(o, WO_COLOR_BLACK); /* dedup: gathered exactly once */
    visit_children(rt, o, collect_white, ctx);
    if (w->len == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 16;
        wo_hdr **ni = realloc(w->items, ncap * sizeof(wo_hdr *));
        if (!ni) {
            w->oom = 1;
            return;
        }
        w->items = ni;
        w->cap = ncap;
    }
    w->items[w->len++] = o;
}

/* Free a dead white: release contents but SKIP every @gc edge — all edge
 * accounting was already settled by the gray/scan phases, and the pointed-
 * at whites die in this same sweep. Containers holding gcref elements free
 * only their backing. */
static void white_free(wo_rt *rt, wo_hdr *o) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    uint64_t *f = wo_fields(o);
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        uint8_t k = c->kinds[i];
        uint64_t v = f[i];
        if (!v || k == WO_K_SCALAR || k == WO_K_GCREF) continue;
        if (k == WO_K_MULTI) {
            wo_multi *m = (wo_multi *)(uintptr_t)v;
            if (m->elem_kind != WO_K_GCREF)
                for (uint32_t j = 0; j < m->len; j++)
                    wo_drop_kind(rt, m->elem_kind, m->items[j]);
            free(m->items);
            wo_arena_free(&rt->arena, m, sizeof(wo_multi));
        } else if (k == WO_K_MAP) {
            wo_map *mp = (wo_map *)(uintptr_t)v;
            for (uint32_t j = 0; j < mp->len; j++) {
                if (mp->key_kind != WO_K_GCREF)
                    wo_drop_kind(rt, mp->key_kind, mp->keys[j]);
                if (mp->val_kind != WO_K_GCREF)
                    wo_drop_kind(rt, mp->val_kind, mp->vals[j]);
            }
            free(mp->keys);
            free(mp->vals);
            wo_arena_free(&rt->arena, mp, sizeof(wo_map));
        } else {
            wo_drop_kind(rt, k, v); /* OWNED subtree, TEXT */
        }
    }
    wo_arena_free(&rt->arena, o, wo_obj_size(c));
}

size_t wo_gc_step(wo_rt *rt, size_t budget) {
    size_t freed = 0, consumed = 0;
    while (consumed < budget && rt->cycbuf.len > 0) {
        wo_hdr *root = rt->cycbuf.items[--rt->cycbuf.len];
        root->flags &= (uint8_t)~WO_F_BUF;
        consumed++;
        if (root->rc == 0) {
            /* died while buffered (zombie guard) — plain deterministic free */
            class_free(rt, root);
            freed++;
            continue;
        }
        /* trial deletion over this root's component (atomic per component) */
        mark_gray(rt, root);
        scan_(rt, root);
        whites_t w = {0};
        collect_white(rt, root, &w);
        if (w.oom) { /* can't track whites: restore and retry next step */
            scan_black(rt, root);
            free(w.items);
            buf_push(rt, root);
            break;
        }
        /* purge gathered whites still sitting in the buffer, then free —
         * deferred freeing removes every dangling-candidate hazard */
        for (size_t i = 0; i < w.len; i++) {
            if (w.items[i]->flags & WO_F_BUF) {
                w.items[i]->flags &= (uint8_t)~WO_F_BUF;
                buf_remove(rt, w.items[i]);
                consumed++;
            }
        }
        for (size_t i = 0; i < w.len; i++) {
            white_free(rt, w.items[i]);
            freed++;
        }
        free(w.items);
    }
    return freed;
}

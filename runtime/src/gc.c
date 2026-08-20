/* gc.c — deterministic drops + incremental tri-color mark-sweep (iteration
 * 7b, spec 2026-08-11). Replaces RC + Bacon–Rajan trial deletion outright:
 * no reference counts exist anywhere; traced objects (inferred-gc classes)
 * die only by sweep, owned values die deterministically exactly as before.
 *
 * Snapshot-at-beginning correctness argument, in one place:
 *   1. When a cycle begins, the caller shades every root (the frames' gc
 *      and owned masks) before the mutator resumes — the snapshot.
 *   2. While marking, deleting a gcref edge (a SETF overwrite, an owned
 *      value dying with a gcref inside) shades the OLD target first — the
 *      Yuasa deletion barrier. Every path that deletes such an edge
 *      funnels through wo_drop_kind's GCREF case or SETF's store site.
 *   3. Objects allocated while a cycle runs are born black (live for this
 *      cycle) — a brand-new object can never be swept by the cycle it was
 *      born into.
 *   Together: anything reachable at snapshot time, or created after it,
 *   survives; only garbage that was already unreachable is swept. A
 *   pointer the mutator holds mid-cycle was obtained from a root, a field
 *   (protected by 1+2), or an allocation (protected by 3).
 *
 * The gray worklist holds TRACED objects only — sweep is the sole freer of
 * traced objects, so a queued pointer can never dangle. Owned interiors
 * are walked eagerly (they are single-owner trees: no cycles, no dedup
 * needed), pruned by the per-class may-gcref bit. */
#include "gc.h"

#include <stdio.h>
#include <stdlib.h>

#include "cont.h"

static int is_traced(const wo_hdr *o) { return (o->flags & WO_F_GC) != 0; }

static uint8_t color_of(const wo_hdr *o) { return o->flags & WO_F_COLOR; }
static void set_color(wo_hdr *o, uint8_t c) {
    o->flags = (uint8_t)((o->flags & ~WO_F_COLOR) | c);
}

/* ---- deterministic destruction (owned values) -------------------------- */

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

/* release a class object's contents then the object itself. Serves owned
 * objects (drop paths) and dead traced objects (sweep): a traced object's
 * gcref edges are no-ops here (their targets die by their own color), its
 * owned interior is dropped — the traced object was that interior's single
 * owner. */
static void class_free(wo_rt *rt, wo_hdr *o) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    uint64_t *f = wo_fields(o);
    for (uint32_t i = 0; i < c->field_cnt; i++)
        wo_drop_kind(rt, c->kinds[i], f[i]);
    wo_arena_free(&rt->arena, o, wo_obj_size(c));
}

/* arc T6: a drop on the wrong shard routes home — the owner's arena is
 * single-threaded by doctrine, so the free travels as an envelope.
 * (vm.c owns the engine; this hook keeps gc.c engine-blind.) */
void wo_route_free(wo_hdr *h);

void wo_drop_obj(wo_rt *rt, wo_hdr *o) {
    if (o && o->shard_id != rt->shard_id && !(o->flags & WO_F_CONST)) {
        wo_route_free(o);
        return;
    }
    if (!o) return;
    switch (o->class_id) {
    case WO_CLS_STR:
    case WO_CLS_BYTES: /* iteration 19: same object shape, same free */
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
        /* the Yuasa deletion barrier: this call site is deleting a gcref
         * edge (an owned holder dying, a container element dropped). While
         * marking, the old target must be shaded or the snapshot leaks
         * reachability; any other time, tracing owns the lifetime and the
         * edge's death means nothing. */
        if (rt->gc_phase == WO_GC_MARK) wo_gc_shade(rt, (wo_hdr *)(uintptr_t)v);
        return;
    case WO_K_TEXT: {
        wo_str *sp = (wo_str *)(uintptr_t)v;
        if (sp->h.shard_id != rt->shard_id && !(sp->h.flags & WO_F_CONST)) {
            wo_route_free(&sp->h); /* home arena frees it (arc T6) */
            return;
        }
        wo_str_free(rt, sp);
        return;
    }
    default:
        return; /* loader guarantees kinds; defensive no-op */
    }
}

/* ---- traced list + shading --------------------------------------------- */

void wo_gc_track(wo_rt *rt, wo_hdr *o, size_t size) {
    o->gclink = rt->gc_traced;
    rt->gc_traced = o;
    rt->gc_traced_cnt++;
    rt->gc_alloc_bytes += size;
}

void wo_gc_shade(wo_rt *rt, wo_hdr *o) {
    if (!o || !is_traced(o)) return;
    if (color_of(o) != WO_COLOR_WHITE) return; /* gray or black: already safe */
    set_color(o, WO_COLOR_GRAY);
    if (rt->gc_gray.len == rt->gc_gray.cap) {
        size_t ncap = rt->gc_gray.cap ? rt->gc_gray.cap * 2 : 64;
        wo_hdr **ni = realloc(rt->gc_gray.items, ncap * sizeof(wo_hdr *));
        if (!ni) {
            /* cannot queue: this cycle can no longer prove anything dead.
             * Flag it; the next slice blackens the whole traced list and
             * the sweep frees nothing — a conservative, safe cycle. */
            rt->gc_gray.oom = 1;
            set_color(o, WO_COLOR_BLACK);
            return;
        }
        rt->gc_gray.items = ni;
        rt->gc_gray.cap = ncap;
    }
    rt->gc_gray.items[rt->gc_gray.len++] = o;
}

/* may this class's instances transitively hold a gcref? NULL table (or an
 * out-of-range id) answers conservatively: traverse. */
static int class_may_gcref(const wo_rt *rt, uint32_t class_id) {
    if (!rt->gc_may || class_id >= rt->class_cnt) return 1;
    return rt->gc_may[class_id] != 0;
}

/* Walk an owned value's interior, shading every traced object reachable
 * through it. Owned graphs are single-owner trees — no cycles, so plain
 * recursion terminates; depth is data-structure depth, same as class_free.
 * Traced objects themselves are shaded, never entered: their fields are
 * scanned when the mark loop pops them gray. */
void wo_gc_scan_root(wo_rt *rt, wo_hdr *o) {
    if (!o) return;
    if (is_traced(o)) {
        wo_gc_shade(rt, o);
        return;
    }
    switch (o->class_id) {
    case WO_CLS_STR:
    case WO_CLS_BYTES: /* iteration 19: leaf bytes, nothing to scan */
        return;
    case WO_CLS_MULTI: {
        wo_multi *m = (wo_multi *)o;
        if (m->elem_kind == WO_K_GCREF || m->elem_kind == WO_K_OWNED ||
            m->elem_kind == WO_K_MULTI || m->elem_kind == WO_K_MAP)
            for (uint32_t i = 0; i < m->len; i++)
                wo_gc_scan_root(rt, (wo_hdr *)(uintptr_t)m->items[i]);
        return;
    }
    case WO_CLS_MAP: {
        wo_map *mp = (wo_map *)o;
        for (uint32_t i = 0; i < mp->len; i++) {
            if (mp->key_kind == WO_K_GCREF || mp->key_kind == WO_K_OWNED ||
                mp->key_kind == WO_K_MULTI || mp->key_kind == WO_K_MAP)
                wo_gc_scan_root(rt, (wo_hdr *)(uintptr_t)mp->keys[i]);
            if (mp->val_kind == WO_K_GCREF || mp->val_kind == WO_K_OWNED ||
                mp->val_kind == WO_K_MULTI || mp->val_kind == WO_K_MAP)
                wo_gc_scan_root(rt, (wo_hdr *)(uintptr_t)mp->vals[i]);
        }
        return;
    }
    default: {
        if (o->class_id >= rt->class_cnt) return; /* defensive */
        if (!class_may_gcref(rt, o->class_id)) return;
        const wo_classdesc *c = &rt->classes[o->class_id];
        uint64_t *f = wo_fields(o);
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            uint8_t k = c->kinds[i];
            if (k == WO_K_GCREF || k == WO_K_OWNED || k == WO_K_MULTI ||
                k == WO_K_MAP)
                wo_gc_scan_root(rt, (wo_hdr *)(uintptr_t)f[i]);
        }
        return;
    }
    }
}

/* ---- the cycle ---------------------------------------------------------- */

int wo_gc_want_start(const wo_rt *rt) {
    return rt->gc_phase == WO_GC_IDLE && rt->gc_alloc_bytes >= rt->gc_goal &&
           rt->gc_traced != NULL;
}

void wo_gc_begin(wo_rt *rt) {
    rt->gc_gray.len = 0;
    rt->gc_gray.oom = 0;
    rt->gc_phase = WO_GC_MARK;
    /* the caller shades the roots now, before the mutator resumes */
}

/* scan one gray (traced) object's out-edges, then blacken it */
static void scan_traced(wo_rt *rt, wo_hdr *o) {
    const wo_classdesc *c = &rt->classes[o->class_id];
    uint64_t *f = wo_fields(o);
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        uint8_t k = c->kinds[i];
        if (k == WO_K_GCREF) {
            if (f[i]) wo_gc_shade(rt, (wo_hdr *)(uintptr_t)f[i]);
        } else if (k == WO_K_OWNED || k == WO_K_MULTI || k == WO_K_MAP) {
            wo_gc_scan_root(rt, (wo_hdr *)(uintptr_t)f[i]);
        }
    }
    set_color(o, WO_COLOR_BLACK);
}

size_t wo_gc_slice(wo_rt *rt, size_t budget) {
    size_t freed = 0;
    if (rt->gc_phase == WO_GC_IDLE) return 0;

    if (rt->gc_phase == WO_GC_MARK) {
        if (rt->gc_gray.oom) {
            /* worklist allocation failed mid-mark: blacken everything so
             * the sweep frees nothing — a wasted cycle, never a wrong one */
            for (wo_hdr *o = rt->gc_traced; o; o = o->gclink)
                set_color(o, WO_COLOR_BLACK);
            rt->gc_gray.len = 0;
        }
        size_t done = 0;
        while (done < budget && rt->gc_gray.len > 0) {
            wo_hdr *o = rt->gc_gray.items[--rt->gc_gray.len];
            scan_traced(rt, o);
            done++;
        }
        if (rt->gc_gray.len == 0) {
            rt->gc_phase = WO_GC_SWEEP;
            rt->gc_sweep = &rt->gc_traced;
        }
    }

    if (rt->gc_phase == WO_GC_SWEEP) {
        size_t done = 0;
        wo_hdr **link = rt->gc_sweep;
        while (done < budget && *link) {
            wo_hdr *o = *link;
            if (color_of(o) == WO_COLOR_WHITE) {
                *link = o->gclink; /* unlink, then free contents + object */
                rt->gc_traced_cnt--;
                class_free(rt, o);
                freed++;
            } else {
                set_color(o, WO_COLOR_WHITE); /* survivor: candidate next cycle */
                link = &o->gclink;
            }
            done++;
        }
        rt->gc_sweep = link;
        if (!*link) {
            rt->gc_phase = WO_GC_IDLE;
            rt->gc_sweep = NULL;
            rt->gc_alloc_bytes = 0;
        }
    }
    if (rt->gc_trace)
        fprintf(stderr, "gc: step %zu budget=%zu freed=%zu remaining=%zu\n",
                ++rt->gc_step_no, budget, freed, rt->gc_traced_cnt);
    return freed;
}

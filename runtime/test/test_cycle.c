/* test_cycle — incremental tri-color mark-sweep (iteration 7b).
 * Cycle classes use the malloc-path trick (~130 fields) so ASan proves
 * every free. Assertions per the 7b spec: an abandoned cycle is freed, a
 * rooted cycle survives, slices are bounded (budget), the traced list is
 * leak-free after repeated cycles, and — the load-bearing one — the Yuasa
 * deletion barrier keeps an object alive when the mutator hides it behind
 * an already-black object between marking slices. */
#include "cont.h"
#include "gc.h"
#include "t.h"

#define BIG 130

/* traced class "GNode": field0 GCREF, field1 MULTI, rest scalars */
static uint8_t gnode_kinds[BIG];
static wo_classdesc CLASSES[1];

static void setup(void) {
    gnode_kinds[0] = WO_K_GCREF;
    gnode_kinds[1] = WO_K_MULTI;
    CLASSES[0] = (wo_classdesc){
        .name = 0, .flags = WO_CLASSF_GC, .field_cnt = BIG, .kinds = gnode_kinds};
}

/* run a whole cycle: shade the given roots, then budgeted slices to idle */
static size_t run_cycle(wo_rt *rt, wo_hdr **roots, size_t nroots, size_t budget) {
    size_t freed = 0;
    wo_gc_begin(rt);
    for (size_t i = 0; i < nroots; i++) wo_gc_shade(rt, roots[i]);
    while (rt->gc_phase != WO_GC_IDLE) freed += wo_gc_slice(rt, budget);
    return freed;
}

/* an abandoned two-object cycle is unreachable and collects whole */
static void test_abandoned_cycle_collects(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0);
    wo_hdr *b = wo_obj_new(&rt, 0);
    wo_fields(a)[0] = (uint64_t)(uintptr_t)b;
    wo_fields(b)[0] = (uint64_t)(uintptr_t)a; /* a <-> b, no root */
    T_EQ(rt.gc_traced_cnt, 2);
    T_EQ(run_cycle(&rt, NULL, 0, 16), 2);
    T_EQ(rt.gc_traced_cnt, 0);
    /* nothing left: a further cycle frees nothing */
    T_EQ(run_cycle(&rt, NULL, 0, 16), 0);
    wo_rt_destroy(&rt);
}

/* a rooted cycle survives every cycle that names it a root, then dies the
 * moment it is abandoned */
static void test_rooted_cycle_survives(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0);
    wo_fields(a)[0] = (uint64_t)(uintptr_t)b;
    wo_fields(b)[0] = (uint64_t)(uintptr_t)a;
    T_EQ(run_cycle(&rt, &a, 1, 16), 0); /* rooted: survives */
    T_EQ(rt.gc_traced_cnt, 2);
    T_EQ(run_cycle(&rt, &a, 1, 16), 0); /* survives repeated cycles */
    T_EQ(rt.gc_traced_cnt, 2);
    T_EQ(run_cycle(&rt, NULL, 0, 16), 2); /* abandoned: dies */
    wo_rt_destroy(&rt);
}

/* budget = objects per slice: four dead nodes at budget 2 need two sweep
 * slices; each slice frees at most the budget */
static void test_budgeted_slices_bounded(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *n[4];
    for (int i = 0; i < 4; i++) n[i] = wo_obj_new(&rt, 0);
    wo_fields(n[0])[0] = (uint64_t)(uintptr_t)n[1];
    wo_fields(n[1])[0] = (uint64_t)(uintptr_t)n[0];
    wo_fields(n[2])[0] = (uint64_t)(uintptr_t)n[3];
    wo_fields(n[3])[0] = (uint64_t)(uintptr_t)n[2];
    wo_gc_begin(&rt);
    size_t f1 = wo_gc_slice(&rt, 2); /* mark drains (nothing gray) + sweep 2 */
    T_EQ(f1, 2);
    T_EQ(rt.gc_traced_cnt, 2);
    size_t f2 = wo_gc_slice(&rt, 2);
    T_EQ(f2, 2);
    T_EQ(rt.gc_traced_cnt, 0);
    T_EQ(rt.gc_phase, WO_GC_IDLE);
    wo_rt_destroy(&rt);
}

/* a cycle closed through a multi's gcref elements collects with its nodes */
static void test_cycle_through_multi_elements(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0);
    /* a --(multi element)--> b --(gcref field)--> a */
    wo_multi *m = wo_multi_new(&rt, WO_K_GCREF);
    T_EQ(wo_multi_push(m, (uint64_t)(uintptr_t)b), 0);
    wo_fields(a)[1] = (uint64_t)(uintptr_t)m;
    wo_fields(b)[0] = (uint64_t)(uintptr_t)a;
    T_EQ(run_cycle(&rt, NULL, 0, 16), 2); /* multi head + backing freed with a */
    T_EQ(rt.gc_traced_cnt, 0);
    wo_rt_destroy(&rt);
}

/* THE BARRIER TEST (spec §7, the design's safety net). Mid-mark, the
 * mutator hides a live object: it deletes the only still-white edge to
 * `victim` after the object holding it was already scanned black. Without
 * the deletion barrier the victim is swept while reachable — silent
 * corruption. With it, the delete shades the victim first.
 *
 *   root -> holder -> victim   (holder scanned black in slice 1)
 *   mutator: root.f0 = victim; holder.f0 deleted  <- barrier shades victim
 *   remaining slices must NOT free victim.
 */
static void test_deletion_barrier_keeps_hidden_object(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *root = wo_obj_new(&rt, 0);
    wo_hdr *holder = wo_obj_new(&rt, 0);
    wo_hdr *victim = wo_obj_new(&rt, 0);
    wo_fields(root)[0] = (uint64_t)(uintptr_t)holder;
    wo_fields(holder)[0] = (uint64_t)(uintptr_t)victim;

    wo_gc_begin(&rt);
    wo_gc_shade(&rt, root);
    /* slice 1, budget 1: scans root (blackens it, shades holder) */
    (void)wo_gc_slice(&rt, 1);
    /* slice 2, budget 1: scans holder (blackens it, shades victim)?  No —
     * order the hide BEFORE holder's scan would shade victim: rewire now,
     * while holder is still gray but victim is white and only holder-held. */
    wo_fields(root)[0] = (uint64_t)(uintptr_t)victim; /* hide behind BLACK root */
    /* delete holder's edge — the mutator's overwrite; the store path's
     * barrier is wo_drop_kind on the old value */
    wo_drop_kind(&rt, WO_K_GCREF, wo_fields(holder)[0]); /* shades victim */
    wo_fields(holder)[0] = 0;
    /* finish the cycle */
    while (rt.gc_phase != WO_GC_IDLE) (void)wo_gc_slice(&rt, 1);
    /* victim survived: still on the traced list, still readable */
    T_EQ(rt.gc_traced_cnt, 3);
    T_EQ(wo_fields(root)[0], (uint64_t)(uintptr_t)victim);
    /* abandon everything: next cycle frees all three */
    T_EQ(run_cycle(&rt, NULL, 0, 16), 3);
    wo_rt_destroy(&rt);
}

/* leak-freedom across repeated cycles: allocate, abandon, collect, N times;
 * the traced list must end empty every round (ASan proves the frees) */
static void test_repeated_cycles_leak_free(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 18, CLASSES, 1), 0);
    for (int round = 0; round < 8; round++) {
        wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0), *c = wo_obj_new(&rt, 0);
        wo_fields(a)[0] = (uint64_t)(uintptr_t)b;
        wo_fields(b)[0] = (uint64_t)(uintptr_t)c;
        wo_fields(c)[0] = (uint64_t)(uintptr_t)a;
        T_EQ(run_cycle(&rt, NULL, 0, 2), 3);
        T_EQ(rt.gc_traced_cnt, 0);
    }
    wo_rt_destroy(&rt);
}

int main(void) {
    setup();
    test_abandoned_cycle_collects();
    test_rooted_cycle_survives();
    test_budgeted_slices_bounded();
    test_cycle_through_multi_elements();
    test_deletion_barrier_keeps_hidden_object();
    test_repeated_cycles_leak_free();
    return t_report("test_cycle");
}

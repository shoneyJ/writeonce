/* test_cycle — budgeted Bacon–Rajan cycle collection.
 * Cycle classes use the malloc-path trick (~130 fields) so ASan proves
 * every free. Budget semantics: a step consumes candidates from the buffer
 * (roots popped + whites purged) up to `budget`, whole components atomic. */
#include "cont.h"
#include "gc.h"
#include "t.h"

#define BIG 130

/* @gc class "GNode": field0 GCREF, field1 MULTI, rest scalars */
static uint8_t gnode_kinds[BIG];
static wo_classdesc CLASSES[1];

static void setup(void) {
    gnode_kinds[0] = WO_K_GCREF;
    gnode_kinds[1] = WO_K_MULTI;
    CLASSES[0] = (wo_classdesc){
        .name = 0, .flags = WO_CLASSF_GC, .field_cnt = BIG, .kinds = gnode_kinds};
}

/* helper: link a->f0 = b, taking a reference on b */
static void link(wo_hdr *a, wo_hdr *b) {
    wo_fields(a)[0] = (uint64_t)(uintptr_t)b;
    wo_rc_inc(b);
}

static void test_two_object_cycle_collects(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0);
    wo_hdr *b = wo_obj_new(&rt, 0);
    link(a, b);
    link(b, a);
    /* drop both external handles: objects survive on cycle edges alone */
    wo_rc_dec(&rt, a);
    wo_rc_dec(&rt, b);
    T_EQ(rt.cycbuf.len, 2); /* both buffered as candidates */
    T_EQ(wo_gc_step(&rt, 16), 2); /* whole cycle freed (ASan proves it) */
    T_EQ(rt.cycbuf.len, 0);
    wo_rt_destroy(&rt);
}

static void test_budget_one_cycle_per_step(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0);
    wo_hdr *c = wo_obj_new(&rt, 0), *d = wo_obj_new(&rt, 0);
    link(a, b);
    link(b, a);
    link(c, d);
    link(d, c);
    wo_rc_dec(&rt, a);
    wo_rc_dec(&rt, b);
    wo_rc_dec(&rt, c);
    wo_rc_dec(&rt, d);
    T_EQ(rt.cycbuf.len, 4);
    /* budget 2 = one two-object component per step */
    T_EQ(wo_gc_step(&rt, 2), 2);
    T_EQ(rt.cycbuf.len, 2);
    T_EQ(wo_gc_step(&rt, 2), 2);
    T_EQ(rt.cycbuf.len, 0);
    /* nothing left: a further step frees nothing */
    T_EQ(wo_gc_step(&rt, 2), 0);
    wo_rt_destroy(&rt);
}

static void test_externally_held_cycle_survives_then_dies(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0);
    link(a, b);
    link(b, a);
    /* keep the external handle on a; drop only b's */
    wo_rc_dec(&rt, b);
    T_EQ(rt.cycbuf.len, 1);
    T_EQ(wo_gc_step(&rt, 16), 0); /* held from outside: survives */
    T_EQ(rt.cycbuf.len, 0);       /* candidate consumed, flag cleared */
    /* counts fully restored */
    T_EQ(a->rc, 2);
    T_EQ(b->rc, 1);
    T_EQ(a->flags & (WO_F_BUF | WO_F_COLOR), 0);
    T_EQ(b->flags & (WO_F_BUF | WO_F_COLOR), 0);
    /* still usable, then truly dead */
    wo_rc_dec(&rt, a);
    T_EQ(rt.cycbuf.len, 1); /* re-buffered on the last external decrement */
    T_EQ(wo_gc_step(&rt, 16), 2);
    wo_rt_destroy(&rt);
}

static void test_cycle_through_multi_elements(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 1), 0);
    wo_hdr *a = wo_obj_new(&rt, 0), *b = wo_obj_new(&rt, 0);
    /* a --(multi element)--> b --(gcref field)--> a */
    wo_multi *m = wo_multi_new(&rt, WO_K_GCREF);
    T_EQ(wo_multi_push(m, (uint64_t)(uintptr_t)b), 0);
    wo_rc_inc(b);
    wo_fields(a)[1] = (uint64_t)(uintptr_t)m;
    link(b, a);
    wo_rc_dec(&rt, a);
    wo_rc_dec(&rt, b);
    T_EQ(wo_gc_step(&rt, 16), 2); /* multi head + backing freed with a */
    T_EQ(rt.cycbuf.len, 0);
    wo_rt_destroy(&rt);
}

int main(void) {
    setup();
    test_two_object_cycle_collects();
    test_budget_one_cycle_per_step();
    test_externally_held_cycle_survives_then_dies();
    test_cycle_through_multi_elements();
    return t_report("test_cycle");
}

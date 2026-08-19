/* test_rc — deterministic drops + the owned/traced boundary (iteration 7b:
 * reference counting is gone; this suite now pins what replaced it on the
 * owned side, and that owned deaths never free traced objects).
 *
 * Testing trick used by every memory test from here on: classes get ~130
 * fields so instances exceed the 1024-byte size-class ceiling and take the
 * arena's malloc path — any missed free is a hard ASan leak report. */
#include "cont.h"
#include "gc.h"
#include "t.h"

#define BIG 130

/* class 0 "Node": field0 OWNED (child Node), field1 TEXT, rest scalars */
static uint8_t node_kinds[BIG];
/* class 1 "Shared" (traced): all scalars */
static uint8_t shared_kinds[BIG];
/* class 2 "Holder": field0 GCREF, field1 MULTI (of TEXT), rest scalars */
static uint8_t holder_kinds[BIG];

static wo_classdesc CLASSES[3];

static void setup_classes(void) {
    node_kinds[0] = WO_K_OWNED;
    node_kinds[1] = WO_K_TEXT;
    shared_kinds[0] = WO_K_SCALAR;
    holder_kinds[0] = WO_K_GCREF;
    holder_kinds[1] = WO_K_MULTI;
    CLASSES[0] = (wo_classdesc){.name = 0, .flags = 0, .field_cnt = BIG, .kinds = node_kinds};
    CLASSES[1] = (wo_classdesc){.name = 0, .flags = WO_CLASSF_GC, .field_cnt = BIG, .kinds = shared_kinds};
    CLASSES[2] = (wo_classdesc){.name = 0, .flags = 0, .field_cnt = BIG, .kinds = holder_kinds};
}

/* Owned tree: parent -> child -> grandchild, each with an owned name text.
 * One drop of the root must free all six allocations (ASan-proven). */
static void test_owned_tree_recursive_drop(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 3), 0);
    wo_hdr *grand = wo_obj_new(&rt, 0);
    wo_fields(grand)[1] = (uint64_t)(uintptr_t)wo_str_new(&rt, "grand", 5);
    wo_hdr *child = wo_obj_new(&rt, 0);
    wo_fields(child)[0] = (uint64_t)(uintptr_t)grand;
    wo_fields(child)[1] = (uint64_t)(uintptr_t)wo_str_new(&rt, "child", 5);
    wo_hdr *root = wo_obj_new(&rt, 0);
    wo_fields(root)[0] = (uint64_t)(uintptr_t)child;
    wo_fields(root)[1] = (uint64_t)(uintptr_t)wo_str_new(&rt, "root", 4);
    wo_drop_obj(&rt, root);
    /* nothing to assert beyond "ASan stays silent" — that IS the test */
    T_CHECK(1);
    wo_rt_destroy(&rt);
}

/* An owned holder dying must NOT free the traced object its gcref field
 * points at — tracing owns that lifetime. The object stays on the traced
 * list; a rootless cycle then frees it (and rt_destroy would too). */
static void test_holder_death_leaves_traced_alive(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 3), 0);
    wo_hdr *shared = wo_obj_new(&rt, 1);
    T_EQ(rt.gc_traced_cnt, 1);
    wo_hdr *holder = wo_obj_new(&rt, 2);
    wo_fields(holder)[0] = (uint64_t)(uintptr_t)shared;
    wo_drop_obj(&rt, holder); /* gcref edge is a no-op: shared survives */
    T_EQ(rt.gc_traced_cnt, 1);
    T_EQ(rt.gc_traced, shared);
    /* one rootless cycle reclaims it */
    wo_gc_begin(&rt);
    while (rt.gc_phase != WO_GC_IDLE) wo_gc_slice(&rt, 16);
    T_EQ(rt.gc_traced_cnt, 0);
    wo_rt_destroy(&rt);
}

/* Text and multi-of-text fields are freed with the holder. */
static void test_container_fields_freed_with_holder(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 3), 0);
    wo_multi *tags = wo_multi_new(&rt, WO_K_TEXT);
    T_EQ(wo_multi_push(tags, (uint64_t)(uintptr_t)wo_str_new(&rt, "a", 1)), 0);
    T_EQ(wo_multi_push(tags, (uint64_t)(uintptr_t)wo_str_new(&rt, "b", 1)), 0);
    wo_hdr *holder = wo_obj_new(&rt, 2);
    wo_fields(holder)[1] = (uint64_t)(uintptr_t)tags;
    wo_drop_obj(&rt, holder); /* frees holder + multi + both strings */
    T_CHECK(1);
    wo_rt_destroy(&rt);
}

/* Teardown safety net: traced objects still on the list when the runtime
 * dies are freed by rt_destroy itself (a test or a trap path that never
 * pumped must still be leak-free — ASan proves the malloc-path frees). */
static void test_rt_destroy_frees_traced_remnants(void) {
    wo_rt rt;
    T_EQ(wo_rt_init(&rt, 1 << 16, CLASSES, 3), 0);
    (void)wo_obj_new(&rt, 1);
    (void)wo_obj_new(&rt, 1);
    T_EQ(rt.gc_traced_cnt, 2);
    wo_rt_destroy(&rt); /* frees both (ASan-proven) */
    T_CHECK(1);
}

int main(void) {
    setup_classes();
    test_owned_tree_recursive_drop();
    test_holder_death_leaves_traced_alive();
    test_container_fields_freed_with_holder();
    test_rt_destroy_frees_traced_remnants();
    return t_report("test_rc");
}

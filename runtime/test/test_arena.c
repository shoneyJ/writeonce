/* test_arena — size-class reuse, region OOM, malloc fallback above 1024. */
#include "obj.h"
#include "t.h"

static void test_size_class_reuse(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 4096), 0);
    void *p = wo_arena_alloc(&a, 48);
    T_CHECK(p != NULL);
    wo_arena_free(&a, p, 48);
    /* same size class: the freed block must come straight back */
    void *q = wo_arena_alloc(&a, 48);
    T_CHECK(q == p);
    wo_arena_free(&a, q, 48);
    wo_arena_destroy(&a);
}

static void test_region_oom_returns_null(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 256), 0);
    void *p1 = wo_arena_alloc(&a, 128);
    void *p2 = wo_arena_alloc(&a, 128);
    T_CHECK(p1 != NULL && p2 != NULL);
    /* region exhausted and nothing on the free lists: null, never abort */
    T_CHECK(wo_arena_alloc(&a, 128) == NULL);
    /* freeing one block makes its class allocatable again */
    wo_arena_free(&a, p1, 128);
    T_CHECK(wo_arena_alloc(&a, 128) == p1);
    wo_arena_destroy(&a);
}

static void test_large_bypasses_region(void) {
    wo_arena a;
    T_EQ(wo_arena_init(&a, 64), 0); /* tiny region on purpose */
    /* >1024 goes to plain malloc/free regardless of region capacity */
    void *big = wo_arena_alloc(&a, 4096);
    T_CHECK(big != NULL);
    wo_arena_free(&a, big, 4096);
    wo_arena_destroy(&a);
}

int main(void) {
    test_size_class_reuse();
    test_region_oom_returns_null();
    test_large_bypasses_region();
    return t_report("test_arena");
}
